#include "uncore.h"
#include "plru.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

static_assert(NUM_CORES <= 8,
              "core_valid is uint8_t — supports at most 8 cores");
static_assert(L2_SETS && (L2_SETS & (L2_SETS - 1)) == 0,
              "L2_SETS must be power of 2");

bool l2_find_way(L2Set *l2Set, uint64_t tag, uint8_t *way)
{
    for (uint8_t w = 0; w < NUM_L2_WAYS; w++) {
        if (l2Set->tag[w] == tag && l2Set->state[w] != MESIState::INVALID) {
            *way = w;
            return true;
        }
    }
    return false;
}

// Clean eviction from L1: clears this core's bit from the given core_valid
// field (d or i). Used when the L1 line is clean (E or S state) — dirty
// evictions go through l2_cache_write_way.
void l2_clear_core_valid_way(uint8_t *core_valid, uint8_t core_id)
{
    *core_valid &= ~(1 << core_id);
}

void l2_cache_read_way(L2Set *l2Set, uint8_t core_id, uint8_t way,
                       uint8_t *data, uint8_t *core_valid)
{
    std::memcpy(data, l2Set->data[way], LINE_SIZE);
    plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, way);
    *core_valid |= static_cast<uint8_t>(1 << core_id);
}

void l2_cache_fill(L2Set *l2Set, uint8_t core_id, uint64_t tag, uint8_t way,
                   uint8_t *data, MESIState state, uint8_t *core_valid_set,
                   uint8_t *core_valid_clr)
{
    // Precondition: caller must have evicted the way first — both core_valid
    // fields must be 0. Hard abort (not assert) so this fires in release builds
    // too.
    if (*core_valid_set != 0 || *core_valid_clr != 0) {
        abort();
    }
    l2Set->tag[way] = tag;
    l2Set->state[way] = state;
    *core_valid_set = static_cast<uint8_t>(1 << core_id);
    std::memcpy(l2Set->data[way], data, LINE_SIZE);
    plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, way);
}

// Prefetch overload — no L1 owner, neither core_valid field is set.
void l2_cache_fill(L2Set *l2Set, uint64_t tag, uint8_t way, uint8_t *data,
                   MESIState state)
{
    if (l2Set->core_valid_d[way] != 0 || l2Set->core_valid_i[way] != 0) {
        abort();
    }
    l2Set->tag[way] = tag;
    l2Set->state[way] = state;
    std::memcpy(l2Set->data[way], data, LINE_SIZE);
    plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, way);
}

void l2_cache_write_way(L2Set *l2Set, uint8_t core_id, uint8_t way,
                        uint8_t *data)
{
    std::memcpy(l2Set->data[way], data, LINE_SIZE);
    l2Set->state[way] = MESIState::MODIFIED;
    l2Set->core_valid_d[way] &= ~(1 << core_id);
}
