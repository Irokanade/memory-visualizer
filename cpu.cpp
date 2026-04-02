#include "cpu.h"
#include "core.h"
#include "types.h"
#include "uncore.h"
#include "memory.h"
#include "plru.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// BusRead snoop: downgrade each sharer's L1 copy M/E → S.
// If a peer holds MODIFIED, its data is captured into line_inout and written back to L2.
// L1I is intentionally not snooped — self-modifying code requires an explicit pipeline flush
// (CPUID serialization on x86), not coherence protocol intervention. evict_l2 handles L1I
// back-invalidation when the L2 line itself is evicted.
static void snoop_downgrade_peers(
    Core *cores,
    uint8_t sharers,
    uint16_t l1_index,
    uint64_t l1_tag,
    uint8_t *line_inout,
    L2Set *l2Set,
    uint8_t l2_hit_way)
{
    // L2 transitions to SHARED unconditionally — other_sharers > 0 when called, so SHARED
    // is always correct. Setting it before the loop also handles stale core_valid entries
    // where find_l1_way fails. sharers may be 0 (e.g. only i_sharers present from cpu_fetch)
    // in which case the loop is a no-op but the L2 state update still applies.
    l2Set->state[l2_hit_way] = MESIState::SHARED;

    while (sharers) {
        uint8_t c = std::countr_zero(sharers);
        sharers &= sharers - 1;

        L1Set *peer = &cores[c].l1d[l1_index];
        uint8_t w;
        if (find_l1_way(peer, l1_tag, &w)) {
            if (peer->state[w] == MESIState::MODIFIED) {
                std::memcpy(line_inout, peer->data[w], LINE_SIZE);
                std::memcpy(l2Set->data[l2_hit_way], peer->data[w], LINE_SIZE);
            }
            peer->state[w] = MESIState::SHARED;
        }
    }
}

// SMC back-invalidate: a store to a code page must invalidate all peer L1I copies.
// Caller is responsible for zeroing core_valid_i[l2_way] after this call.
static void snoop_invalidate_peers_i(
    Core *cores,
    uint8_t i_sharers,
    uint16_t l1_index,
    uint64_t l1_tag)
{
    while (i_sharers) {
        uint8_t c = static_cast<uint8_t>(std::countr_zero(i_sharers));
        i_sharers &= i_sharers - 1;

        L1Set *peer = &cores[c].l1i[l1_index];
        uint8_t w;
        if (find_l1_way(peer, l1_tag, &w)) {
            peer->state[w] = MESIState::INVALID;
        }
    }
}

// BusRdX / upgrade RFO snoop: invalidate each sharer's L1 copy and clear its core_valid bit.
// If line_out != nullptr and the peer holds MODIFIED, fresh data is captured into line_out.
static void snoop_invalidate_peers(
    Core *cores,
    uint8_t sharers,
    uint16_t l1_index,
    uint64_t l1_tag,
    uint8_t *line_out,
    L2Set *l2Set,
    uint8_t l2_way)
{
    while (sharers) {
        uint8_t c = static_cast<uint8_t>(std::countr_zero(sharers));
        sharers &= sharers - 1;

        L1Set *peer = &cores[c].l1d[l1_index];
        uint8_t w;
        if (find_l1_way(peer, l1_tag, &w)) {
            if (line_out && peer->state[w] == MESIState::MODIFIED) {
                // Capture fresh data for the caller — L2 data is intentionally NOT updated here.
                // The line enters L1D as MODIFIED; evict_l2 will capture it from L1D before
                // writing to memory. Updating L2 here would be redundant and diverge from
                // the lazy write-back model.
                std::memcpy(line_out, peer->data[w], LINE_SIZE);
            }
            peer->state[w] = MESIState::INVALID;
        }
        l2Set->core_valid_d[l2_way] &= static_cast<uint8_t>(~(1 << c));
    }
}

template<uint8_t TLB_L1_SETS>
static bool translate(TLBSet *tlb_l1_array, Memory *mem, uint64_t virtual_address, uint64_t *physical_address) {
    uint64_t virtual_page_num = to_frame(virtual_address);
    TLBSet *tlb_l1 = &tlb_l1_array[virtual_page_num & (TLB_L1_SETS - 1)];
    uint64_t physical_frame;

    if (tlb_lookup(tlb_l1, virtual_page_num, &physical_frame)) {
        *physical_address = to_address(physical_frame, virtual_address & 0xFFF);
        return true;
    }

    if (page_walk(mem, virtual_address, physical_address)) {
        tlb_fill(tlb_l1, virtual_page_num, to_frame(*physical_address));
        return true;
    }

    return false; // page fault
}

template<uint8_t TLB_L1_SETS, uint8_t TLB_L2_SETS>
static bool translate(TLBSet *tlb_l1_array, TLBSet *tlb_l2_array, Memory *mem, uint64_t virtual_address, uint64_t *physical_address) {
    uint64_t virtual_page_num = to_frame(virtual_address);
    TLBSet *tlb_l1 = &tlb_l1_array[virtual_page_num & (TLB_L1_SETS - 1)];
    TLBSet *tlb_l2 = &tlb_l2_array[virtual_page_num & (TLB_L2_SETS - 1)];
    uint64_t physical_frame;

    if (tlb_lookup(tlb_l1, virtual_page_num, &physical_frame)) {
        *physical_address = to_address(physical_frame, virtual_address & 0xFFF);
        return true;
    }

    if (tlb_lookup(tlb_l2, virtual_page_num, &physical_frame)) {
        *physical_address = to_address(physical_frame, virtual_address & 0xFFF);
        tlb_fill(tlb_l1, virtual_page_num, physical_frame);
        return true;
    }

    if (page_walk(mem, virtual_address, physical_address)) {
        uint64_t frame = to_frame(*physical_address);
        tlb_fill(tlb_l1, virtual_page_num, frame);
        tlb_fill(tlb_l2, virtual_page_num, frame);
        return true;
    }

    return false; // page fault
}

template<bool IS_INSTR>
static uint8_t evict_l1(L1Set *l1Set, uint16_t l1_index, L2Set *l2Sets, uint8_t core_id) {
    // Prefer any INVALID way — real hardware knows back-invalidated slots are free and
    // uses them before evicting a valid line. Scanning all ways matches that behavior.
    for (uint8_t w = 0; w < NUM_L1_WAYS; w++) {
        if (l1Set->state[w] == MESIState::INVALID) {
            return w;
        }
    }

    uint8_t victim = plru_victim<uint8_t, NUM_L1_WAYS>(l1Set->plru_bits);

    uint64_t victim_addr = l1_to_addr(l1Set->tag[victim], l1_index);
    L2Set *l2Set = &l2Sets[l2_to_index(victim_addr)];
    uint64_t l2_tag = l2_to_tag(victim_addr);
    uint8_t l2_way;
    if (!l2_find_way(l2Set, l2_tag, &l2_way)) {
        // Inclusive invariant violated — this is a hard bug; abort in all build modes.
        std::abort();
    }

    if constexpr (!IS_INSTR) {
        if (l1Set->state[victim] == MESIState::MODIFIED) {
            l2_cache_write_way(l2Set, core_id, l2_way, l1Set->data[victim]);
        } else {
            l2_clear_core_valid_way(&l2Set->core_valid_d[l2_way], core_id);
            if (l2Set->core_valid_d[l2_way] == 0 && l2Set->core_valid_i[l2_way] == 0 &&
                l2Set->state[l2_way] == MESIState::SHARED) {
                l2Set->state[l2_way] = MESIState::EXCLUSIVE;
            }
        }
    } else {
        l2_clear_core_valid_way(&l2Set->core_valid_i[l2_way], core_id);
        if (l2Set->core_valid_d[l2_way] == 0 && l2Set->core_valid_i[l2_way] == 0 &&
            l2Set->state[l2_way] == MESIState::SHARED) {
            l2Set->state[l2_way] = MESIState::EXCLUSIVE;
        }
    }

    l1Set->state[victim] = MESIState::INVALID;
    return victim;
}

static uint8_t evict_l2(Core *cores, L2Set *l2Set, uint16_t l2_index, Memory *mem) {
    for (uint8_t w = 0; w < NUM_L2_WAYS; w++) {
        if (l2Set->state[w] == MESIState::INVALID) {
            return w;
        }
    }

    uint8_t l2_victim = plru_victim<uint16_t, NUM_L2_WAYS>(l2Set->plru_bits);

    uint64_t l2_victim_addr = l2_to_addr(l2Set->tag[l2_victim], l2_index);
    uint8_t valid_d = l2Set->core_valid_d[l2_victim];
    uint8_t valid_i = l2Set->core_valid_i[l2_victim];
    uint8_t all_valid = valid_d | valid_i;
    uint16_t victim_l1_idx = l1_to_index(l2_victim_addr);
    uint64_t victim_l1_tag = l1_to_tag(l2_victim_addr);

    while (all_valid) {
        uint8_t c = std::countr_zero(all_valid);
        all_valid &= all_valid - 1;

        if (valid_d & (1 << c)) {
            L1Set *peer_l1d = &cores[c].l1d[victim_l1_idx];
            uint8_t w;
            if (find_l1_way(peer_l1d, victim_l1_tag, &w)) {
                if (peer_l1d->state[w] == MESIState::MODIFIED) {
                    std::memcpy(l2Set->data[l2_victim], peer_l1d->data[w], LINE_SIZE);
                    l2Set->state[l2_victim] = MESIState::MODIFIED;
                }
                peer_l1d->state[w] = MESIState::INVALID;
            } else {
                // Stale core_valid_d — L1 line is gone but bit was never cleared.
                // If L2 is MODIFIED, the fresh data is unrecoverable: a prior write path
                // failed to keep the line in L1D. Abort rather than silently corrupt memory.
                if (l2Set->state[l2_victim] == MESIState::MODIFIED) std::abort();
            }
        }

        if (valid_i & (1 << c)) {
            // L1I is always clean (SI protocol) — no writeback needed
            L1Set *peer_l1i = &cores[c].l1i[victim_l1_idx];
            uint8_t w;
            if (find_l1_way(peer_l1i, victim_l1_tag, &w)) {
                peer_l1i->state[w] = MESIState::INVALID;
            }
        }
    }

    if (l2Set->state[l2_victim] == MESIState::MODIFIED) {
        if (l2_victim_addr + LINE_SIZE > mem->size) {
            std::abort();
        }
        std::memcpy(&mem->data[l2_victim_addr], l2Set->data[l2_victim], LINE_SIZE);
    }

    // Clear tracking bitmasks so l2_cache_fill's precondition assert passes.
    l2Set->core_valid_d[l2_victim] = 0;
    l2Set->core_valid_i[l2_victim] = 0;
    l2Set->state[l2_victim] = MESIState::INVALID;

    return l2_victim;
}

// Issue a silent prefetch into L2 only — no L1 fill, no owner tracking.
// Returns true if the line was (or already was) in L2, false if blocked by page boundary
// or bounds — caller should stop the burst and demote the stream to TRAINING.
static bool pf_fill_l2(Core *cores, L2Set *l2Sets, Memory *mem, uint64_t from_line, uint64_t next_line) {
    // Never cross a 4KB page boundary (hardware constraint, per Drepper §6.3)
    if ((next_line & ~static_cast<uint64_t>(0xFFF)) != (from_line & ~static_cast<uint64_t>(0xFFF))) {
        return false;
    }
    // Guard against backward-wrap near address 0 and forward out-of-bounds.
    if (next_line < LINE_SIZE || next_line + LINE_SIZE > mem->size) {
        return false;
    }
    uint16_t l2_index = l2_to_index(next_line);
    L2Set *l2Set = &l2Sets[l2_index];
    uint64_t l2_tag = l2_to_tag(next_line);
    uint8_t way;
    if (l2_find_way(l2Set, l2_tag, &way)) {
        return true; // already present
    }
    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    // Prefetch has no L1 owner — use the no-core overload which sets neither core_valid field.
    l2_cache_fill(l2Set, l2_tag, l2_victim, &mem->data[next_line], MESIState::EXCLUSIVE);
    return true;
}

// Next-line instruction prefetcher — unconditionally prefetches the line after an L1I miss.
// No stream detection: instruction fetch is almost always sequential.
static void pf_nextline_on_miss(Core *cores, L2Set *l2Sets, Memory *mem, uint64_t miss_line) {
    pf_fill_l2(cores, l2Sets, mem, miss_line, miss_line + LINE_SIZE);
}

// Stream detector — called on every L2 data demand miss. Trains the stream table and
// burst-prefetches into L2 for confirmed streams. Never crosses 4KB page boundaries.
static void pf_stream_on_miss(Prefetcher *pf, Core *cores, L2Set *l2Sets, Memory *mem, uint64_t miss_line) {
    for (uint8_t i = 0; i < NUM_STREAMS; i++) {
        Stream *stream = &pf->streams[i];

        if (stream->confidence == StreamConfidence::INVALID) {
            continue;
        }

        if (stream->confidence == StreamConfidence::TRAINING) {
            if (miss_line == stream->last_line + LINE_SIZE) {
                stream->direction  = StreamDirection::FORWARD;
                stream->confidence = StreamConfidence::STEADY;
                stream->prefetch_head = miss_line;
            } else if (miss_line == stream->last_line - LINE_SIZE) {
                stream->direction  = StreamDirection::BACKWARD;
                stream->confidence = StreamConfidence::STEADY;
                stream->prefetch_head = miss_line;
            } else {
                // No match — leave entry untouched and let LRU age it out.
                // Hardware behavior: content-addressed lookup finds at most one match;
                // mismatching entries are never restarted in-place.
                continue;
            }
        } else {
            // STEADY match: miss must be within [last_line+stride .. prefetch_head+stride].
            // Demand hits inside the prefetch window are L2 hits and don't call pf_stream_on_miss,
            // so the next miss arrives at prefetch_head+stride, not last_line+stride.
            // Signed gap handles both FORWARD (+1) and BACKWARD (-1) without branching.
            int8_t  dir              = static_cast<int8_t>(stream->direction);
            int64_t dist_from_last   = (static_cast<int64_t>(miss_line)
                                        - static_cast<int64_t>(stream->last_line))
                                       * static_cast<int64_t>(dir);
            int64_t dist_from_head   = (static_cast<int64_t>(miss_line)
                                        - static_cast<int64_t>(stream->prefetch_head))
                                       * static_cast<int64_t>(dir);
            if (dist_from_last < static_cast<int64_t>(LINE_SIZE) ||
                dist_from_head  > static_cast<int64_t>(LINE_SIZE)) {
                continue;
            }
        }

        // Matched — advance demand pointer, then burst-prefetch until prefetch_head
        // is PREFETCH_DISTANCE lines ahead of last_line (building lookahead distance).
        // If a prefetch is blocked (page boundary or bounds), demote to TRAINING so the
        // slot can be reclaimed rather than sitting stuck at the boundary forever.
        // stride read after direction is set — valid for both TRAINING→STEADY and STEADY paths.
        int8_t stride = static_cast<int8_t>(stream->direction);
        stream->last_line = miss_line;
        for (uint8_t issued = 0; issued < PREFETCH_DISTANCE + 1; issued++) {
            // Signed gap: positive when prefetch_head is ahead in the stream direction.
            int64_t gap = (static_cast<int64_t>(stream->prefetch_head)
                           - static_cast<int64_t>(miss_line))
                          * static_cast<int64_t>(stride);
            if (gap >= static_cast<int64_t>(PREFETCH_DISTANCE) * LINE_SIZE) break;
            uint64_t next = stream->prefetch_head
                            + static_cast<uint64_t>(static_cast<int64_t>(stride) * LINE_SIZE);
            if (!pf_fill_l2(cores, l2Sets, mem, stream->prefetch_head, next)) {
                // Blocked by page boundary or bounds — fully reset to TRAINING.
                // Hardware resets the entry on demotion; direction is re-derived from the
                // next two consecutive misses, so leaving it stale would be functionally
                // equivalent but not accurate to hardware state.
                stream->confidence = StreamConfidence::TRAINING;
                stream->direction  = StreamDirection::UNKNOWN;
                break;
            }
            stream->prefetch_head = next;
        }
        pf_lru_update(pf->lru_age, i);
        return;
    }

    // No stream matched — allocate a new TRAINING entry for this miss address
    uint8_t victim = pf_lru_evict(pf->lru_age);
    pf->streams[victim].last_line = miss_line;
    pf->streams[victim].prefetch_head = miss_line;
    pf->streams[victim].direction = StreamDirection::UNKNOWN;
    pf->streams[victim].confidence = StreamConfidence::TRAINING;
    pf_lru_update(pf->lru_age, victim);
}

bool cpu_read(
    CPU *cpu,
    uint8_t core_id,
    Memory *mem,
    uint64_t virtual_address,
    uint8_t *data,
    uint8_t data_size)
{
    Core *cores = cpu->cores;
    L2Set *l2Sets = cpu->l2Sets;
    Core *core = &cores[core_id];
    uint64_t physical_address;
    if (!translate<L1_DTLB_SETS, L2_DTLB_SETS>(core->l1_dtlb, core->l2_dtlb, mem, virtual_address, &physical_address)) {
        return false;
    }

    uint16_t l1_index = l1_to_index(physical_address);
    L1Set *l1Set = &core->l1d[l1_index];
    uint64_t l1_tag = l1_to_tag(physical_address);
    uint8_t offset = l1_to_offset(physical_address);
    uint16_t l2_index = l2_to_index(physical_address);
    L2Set *l2Set = &l2Sets[l2_index];
    uint64_t l2_tag = l2_to_tag(physical_address);

    // L1 hit — handles M/E/S
    uint8_t l1_hit_way;
    if (find_l1_way(l1Set, l1_tag, &l1_hit_way)) {
        l1_cache_read_way(l1Set, l1_hit_way, offset, data, data_size);
        return true;
    }

    uint8_t line[LINE_SIZE];
    uint8_t l2_hit_way;

    if (l2_find_way(l2Set, l2_tag, &l2_hit_way)) {
        uint8_t victim = evict_l1<false>(l1Set, l1_index, l2Sets, core_id);

        // l2_cache_read_way sets this core's bit in core_valid_d first;
        // d_sharers masks it back out to get only the other cores' bits.
        l2_cache_read_way(l2Set, core_id, l2_hit_way, line, &l2Set->core_valid_d[l2_hit_way]);

        // Use L2 core_valid (both D and I) to drive coherence (L2 acts as directory)
        uint8_t d_sharers = l2Set->core_valid_d[l2_hit_way] & ~(1 << core_id);
        uint8_t i_sharers = l2Set->core_valid_i[l2_hit_way] & ~(1 << core_id);
        uint8_t other_sharers = d_sharers | i_sharers;
        MESIState fill_state;

        if (other_sharers) {
            fill_state = MESIState::SHARED;
            // Snoop L1D peers only: M→S (fresh data), E→S (clean in L2)
            // L1I is always SHARED — no action needed
            snoop_downgrade_peers(cores, d_sharers, l1_index,
                                  l1_tag, line, l2Set, l2_hit_way);
        } else {
            fill_state = MESIState::EXCLUSIVE;
        }

        l1_cache_fill(l1Set, l1_tag, victim, line, fill_state);
        std::memcpy(data, line + offset, data_size);
        return true;
    }

    // L2 miss — fetch full line from main memory
    uint64_t line_base = to_line_base(physical_address);
    if (line_base + LINE_SIZE > mem->size) std::abort(); // cpu_read: mem fetch out of bounds
    uint8_t *mem_line = &mem->data[line_base];

    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    uint8_t l1_victim = evict_l1<false>(l1Set, l1_index, l2Sets, core_id);

    l2_cache_fill(l2Set, core_id, l2_tag, l2_victim, mem_line, MESIState::EXCLUSIVE, &l2Set->core_valid_d[l2_victim], &l2Set->core_valid_i[l2_victim]);
    l1_cache_fill(l1Set, l1_tag, l1_victim, mem_line, MESIState::EXCLUSIVE);

    // Hook fires after the demand fill so pf_fill_l2 never aliases the same L2 set
    // as the demand line and evicts it before it's installed.
    pf_stream_on_miss(&core->prefetcher, cores, l2Sets, mem, line_base);

    std::memcpy(data, mem_line + offset, data_size);
    return true;
}

bool cpu_write(
    CPU *cpu,
    uint8_t core_id,
    Memory *mem,
    uint64_t virtual_address,
    uint8_t *data,
    uint8_t data_size)
{
    Core *cores = cpu->cores;
    L2Set *l2Sets = cpu->l2Sets;
    Core *core = &cores[core_id];
    uint64_t physical_address;
    if (!translate<L1_DTLB_SETS, L2_DTLB_SETS>(core->l1_dtlb, core->l2_dtlb, mem, virtual_address, &physical_address)) {
        return false;
    }

    uint16_t l1_index = l1_to_index(physical_address);
    L1Set *l1Set = &core->l1d[l1_index];
    uint64_t l1_tag = l1_to_tag(physical_address);
    uint8_t offset = l1_to_offset(physical_address);
    uint16_t l2_index = l2_to_index(physical_address);
    L2Set *l2Set = &l2Sets[l2_index];
    uint64_t l2_tag = l2_to_tag(physical_address);

    // L1 hit check
    uint8_t l1_hit_way;
    if (find_l1_way(l1Set, l1_tag, &l1_hit_way)) {
        uint8_t l2_way;
        if (!l2_find_way(l2Set, l2_tag, &l2_way)) {
            std::abort();
        }

        if (l1Set->state[l1_hit_way] == MESIState::SHARED) {
            // RFO (upgrade S→M): invalidate all other L1 copies via L2 directory.
            // BusRdX is handled by L2, so update L2 PLRU (E→M silent upgrade does not).
            uint8_t other_sharers = l2Set->core_valid_d[l2_way] & ~(1 << core_id);
            snoop_invalidate_peers(cores, other_sharers, l1_index, l1_tag,
                                   nullptr, l2Set, l2_way);
            plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, l2_way);
        }

        // All write paths: update L2 state and back-invalidate any peer L1I copies (SMC).
        // S→M: L2 was SHARED, now MODIFIED. E→M: L2 was EXCLUSIVE, now MODIFIED.
        // M→M: re-does this harmlessly.
        snoop_invalidate_peers_i(cores, l2Set->core_valid_i[l2_way], l1_index, l1_tag);
        l2Set->core_valid_i[l2_way] = 0;
        l2Set->core_valid_d[l2_way] = static_cast<uint8_t>(1 << core_id);
        l2Set->state[l2_way] = MESIState::MODIFIED;

        // l1_cache_write_way sets state → MODIFIED regardless of prior state (M/E/S all valid here)
        l1_cache_write_way(l1Set, l1_hit_way, offset, data, data_size);
        return true;
    }

    // L1 miss — write-allocate: fetch full line, apply write, fill into L1
    uint8_t line[LINE_SIZE];
    uint8_t l2_hit_way;

    if (l2_find_way(l2Set, l2_tag, &l2_hit_way)) {
        // Fetch full line from L2 for write-allocate
        std::memcpy(line, l2Set->data[l2_hit_way], LINE_SIZE);

        // RFO: snoop and invalidate all other L1D copies (BusRdX), capture MODIFIED data
        uint8_t other_sharers = l2Set->core_valid_d[l2_hit_way] & ~(1 << core_id);
        snoop_invalidate_peers(cores, other_sharers, l1_index, l1_tag,
                               line, l2Set, l2_hit_way);

        // Apply the write to the fetched line, then fill L1
        std::memcpy(line + offset, data, data_size);

        uint8_t victim = evict_l1<false>(l1Set, l1_index, l2Sets, core_id);
        l1_cache_fill(l1Set, l1_tag, victim, line, MESIState::MODIFIED);

        // L2: this core is sole owner, line is MODIFIED
        // SMC: back-invalidate any peer L1I copies before zeroing the tracking bits
        snoop_invalidate_peers_i(cores, l2Set->core_valid_i[l2_hit_way], l1_index, l1_tag);
        l2Set->core_valid_i[l2_hit_way] = 0;
        l2Set->core_valid_d[l2_hit_way] = static_cast<uint8_t>(1 << core_id);
        l2Set->state[l2_hit_way] = MESIState::MODIFIED;
        plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, l2_hit_way);
        return true;
    }

    // L2 miss — fetch from main memory, write-allocate into L2 then L1
    uint64_t line_base = to_line_base(physical_address);
    if (line_base + LINE_SIZE > mem->size) {
        std::abort();
    }
    std::memcpy(line, &mem->data[line_base], LINE_SIZE);
    std::memcpy(line + offset, data, data_size); // apply write

    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    uint8_t l1_victim = evict_l1<false>(l1Set, l1_index, l2Sets, core_id);

    l2_cache_fill(l2Set, core_id, l2_tag, l2_victim, line, MESIState::MODIFIED, &l2Set->core_valid_d[l2_victim], &l2Set->core_valid_i[l2_victim]);
    l1_cache_fill(l1Set, l1_tag, l1_victim, line, MESIState::MODIFIED);

    // Write-allocate fetches a full line from memory — feed the streamer the same as a load miss.
    pf_stream_on_miss(&core->prefetcher, cores, l2Sets, mem, line_base);
    return true;
}

bool cpu_fetch(
    CPU *cpu,
    uint8_t core_id,
    Memory *mem,
    uint64_t virtual_address,
    uint8_t *data,
    uint8_t data_size)
{
    Core *cores = cpu->cores;
    L2Set *l2Sets = cpu->l2Sets;
    Core *core = &cores[core_id];
    uint64_t physical_address;
    if (!translate<ITLB_SETS>(core->itlb, mem, virtual_address, &physical_address)) {
        return false;
    }

    uint16_t l1_index = l1_to_index(physical_address);
    L1Set *l1Set = &core->l1i[l1_index];
    uint64_t l1_tag = l1_to_tag(physical_address);
    uint8_t offset = l1_to_offset(physical_address);
    uint16_t l2_index = l2_to_index(physical_address);
    L2Set *l2Set = &l2Sets[l2_index];
    uint64_t l2_tag = l2_to_tag(physical_address);

    // L1I hit
    uint8_t l1_hit_way;
    if (find_l1_way(l1Set, l1_tag, &l1_hit_way)) {
        l1_cache_read_way(l1Set, l1_hit_way, offset, data, data_size);
        return true;
    }

    uint8_t line[LINE_SIZE];
    uint8_t l2_hit_way;

    if (l2_find_way(l2Set, l2_tag, &l2_hit_way)) {
        // Instruction pages must not be dirty — fetching a MODIFIED line means SMC without flush.
        // Check before any side effects (evict_l1, l2_cache_read_way) so nothing is committed
        // to a state that is about to be declared unreachable.
        if (l2Set->state[l2_hit_way] == MESIState::MODIFIED) {
            std::abort();
        }

        // L1I lines are always SHARED or INVALID — evict_l1<true> handles clean eviction
        uint8_t victim = evict_l1<true>(l1Set, l1_index, l2Sets, core_id);
        l2_cache_read_way(l2Set, core_id, l2_hit_way, line, &l2Set->core_valid_i[l2_hit_way]);
        // If other D/I sharers exist, downgrade L2→SHARED and force peer L1D E→S.
        // If this core is the sole holder, L2 stays EXCLUSIVE — matches hardware: L2 only
        // transitions to SHARED when a second cache fills from it.
        uint8_t d_sharers = l2Set->core_valid_d[l2_hit_way] & ~(1 << core_id);
        uint8_t i_sharers = l2Set->core_valid_i[l2_hit_way] & ~(1 << core_id);
        if (d_sharers | i_sharers) {
            snoop_downgrade_peers(cores, d_sharers, l1_index, l1_tag, line, l2Set, l2_hit_way);
        }

        // L1I always fills as SHARED (SI protocol — instruction pages are read-only)
        l1_cache_fill(l1Set, l1_tag, victim, line, MESIState::SHARED);
        std::memcpy(data, line + offset, data_size);
        return true;
    }

    // L2 miss — fetch from main memory
    uint64_t line_base = to_line_base(physical_address);
    if (line_base + LINE_SIZE > mem->size) std::abort(); // cpu_fetch: mem fetch out of bounds
    uint8_t *mem_line = &mem->data[line_base];

    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    uint8_t l1_victim = evict_l1<true>(l1Set, l1_index, l2Sets, core_id);

    // Fresh from memory — no other sharers exist, so L2 starts EXCLUSIVE.
    l2_cache_fill(l2Set, core_id, l2_tag, l2_victim, mem_line, MESIState::EXCLUSIVE, &l2Set->core_valid_i[l2_victim], &l2Set->core_valid_d[l2_victim]);
    l1_cache_fill(l1Set, l1_tag, l1_victim, mem_line, MESIState::SHARED);

    pf_nextline_on_miss(cores, l2Sets, mem, line_base);

    std::memcpy(data, mem_line + offset, data_size);
    return true;
}
