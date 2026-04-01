#include "cpu.h"
#include "core.h"
#include "types.h"
#include "uncore.h"
#include "memory.h"
#include "plru.h"

#include <bit>
#include <cassert>
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
    uint8_t victim = plru_victim<uint8_t, NUM_L1_WAYS>(l1Set->plru_bits);

    if (l1Set->state[victim] == MESIState::INVALID) {
        return victim;
    }

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
        }
    } else {
        // L1I is always clean (SI protocol) — just clear the tracking bit
        l2_clear_core_valid_way(&l2Set->core_valid_i[l2_way], core_id);
    }

    l1Set->state[victim] = MESIState::INVALID;
    return victim;
}

static uint8_t evict_l2(Core *cores, L2Set *l2Set, uint16_t l2_index, Memory *mem) {
    uint8_t l2_victim = plru_victim<uint16_t, NUM_L2_WAYS>(l2Set->plru_bits);

    if (l2Set->state[l2_victim] == MESIState::INVALID) {
        return l2_victim;
    }

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
        assert(l2_victim_addr + LINE_SIZE <= mem->size && "evict_l2: write back out of bounds");
        std::memcpy(&mem->data[l2_victim_addr], l2Set->data[l2_victim], LINE_SIZE);
    }

    // Clear tracking bitmasks so l2_cache_fill's precondition assert passes.
    l2Set->core_valid_d[l2_victim] = 0;
    l2Set->core_valid_i[l2_victim] = 0;
    l2Set->state[l2_victim] = MESIState::INVALID;

    return l2_victim;
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
    assert(line_base + LINE_SIZE <= mem->size && "cpu_read: mem fetch out of bounds");
    uint8_t *mem_line = &mem->data[line_base];

    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    uint8_t l1_victim = evict_l1<false>(l1Set, l1_index, l2Sets, core_id);

    l2_cache_fill(l2Set, core_id, l2_tag, l2_victim, mem_line, MESIState::EXCLUSIVE, &l2Set->core_valid_d[l2_victim], &l2Set->core_valid_i[l2_victim]);
    l1_cache_fill(l1Set, l1_tag, l1_victim, mem_line, MESIState::EXCLUSIVE);

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
        assert(l2_find_way(l2Set, l2_tag, &l2_way) && "inclusive invariant violated: L1 line has no backing L2 entry");

        if (l1Set->state[l1_hit_way] == MESIState::SHARED) {
            // RFO (upgrade S→M): invalidate all other L1 copies via L2 directory
            uint8_t other_sharers = l2Set->core_valid_d[l2_way] & ~(1 << core_id);

            // Upgrade RFO: no data capture needed (line already in our L1)
            snoop_invalidate_peers(cores, other_sharers, l1_index, l1_tag,
                                   nullptr, l2Set, l2_way);
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
    assert(line_base + LINE_SIZE <= mem->size && "cpu_write: mem fetch out of bounds");
    std::memcpy(line, &mem->data[line_base], LINE_SIZE);
    std::memcpy(line + offset, data, data_size); // apply write

    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    uint8_t l1_victim = evict_l1<false>(l1Set, l1_index, l2Sets, core_id);

    l2_cache_fill(l2Set, core_id, l2_tag, l2_victim, line, MESIState::MODIFIED, &l2Set->core_valid_d[l2_victim], &l2Set->core_valid_i[l2_victim]);
    l1_cache_fill(l1Set, l1_tag, l1_victim, line, MESIState::MODIFIED);
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
        // L1I lines are always SHARED or INVALID — evict_l1<true> handles clean eviction
        uint8_t victim = evict_l1<true>(l1Set, l1_index, l2Sets, core_id);

        l2_cache_read_way(l2Set, core_id, l2_hit_way, line, &l2Set->core_valid_i[l2_hit_way]);

        // Instruction pages must not be dirty — fetching a MODIFIED line means SMC without flush.
        // Hard abort in all build modes (not assert, which is stripped by NDEBUG).
        if (l2Set->state[l2_hit_way] == MESIState::MODIFIED) {
            std::abort();
        }
        // Filling L1I adds an I-sharer, so L2 must be SHARED regardless of prior state.
        // Also downgrade any peer L1D EXCLUSIVE copies — they must not silent-upgrade to M.
        uint8_t d_sharers = l2Set->core_valid_d[l2_hit_way] & ~(1 << core_id);
        uint8_t i_sharers = l2Set->core_valid_i[l2_hit_way] & ~(1 << core_id);
        if (d_sharers | i_sharers) {
            snoop_downgrade_peers(cores, d_sharers, l1_index, l1_tag, line, l2Set, l2_hit_way);
        } else {
            // No other sharers — snoop_downgrade_peers skipped, so set SHARED explicitly.
            l2Set->state[l2_hit_way] = MESIState::SHARED;
        }

        // L1I always fills as SHARED (SI protocol — instruction pages are read-only)
        l1_cache_fill(l1Set, l1_tag, victim, line, MESIState::SHARED);
        std::memcpy(data, line + offset, data_size);
        return true;
    }

    // L2 miss — fetch from main memory
    uint64_t line_base = to_line_base(physical_address);
    assert(line_base + LINE_SIZE <= mem->size && "cpu_fetch: mem fetch out of bounds");
    uint8_t *mem_line = &mem->data[line_base];

    uint8_t l2_victim = evict_l2(cores, l2Set, l2_index, mem);
    uint8_t l1_victim = evict_l1<true>(l1Set, l1_index, l2Sets, core_id);

    // Fresh from memory — no other sharers exist, so L2 starts EXCLUSIVE.
    l2_cache_fill(l2Set, core_id, l2_tag, l2_victim, mem_line, MESIState::EXCLUSIVE, &l2Set->core_valid_i[l2_victim], &l2Set->core_valid_d[l2_victim]);
    l1_cache_fill(l1Set, l1_tag, l1_victim, mem_line, MESIState::SHARED);

    std::memcpy(data, mem_line + offset, data_size);
    return true;
}
