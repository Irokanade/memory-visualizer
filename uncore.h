#ifndef UNCORE_H
#define UNCORE_H

#include "types.h"
#include <cstdint>
#include <cstring>

// Intel Core 2: 4MB shared L2, 16-way, 4096 sets, 64-byte lines
constexpr uint8_t  NUM_L2_WAYS = 16;
constexpr uint16_t L2_SETS     = 4096;

struct L2Set {
    uint64_t  tag[NUM_L2_WAYS];
    MESIState state[NUM_L2_WAYS];
    uint8_t   core_valid_d[NUM_L2_WAYS]; // bitmask: which cores' L1D holds a copy
    uint8_t   core_valid_i[NUM_L2_WAYS]; // bitmask: which cores' L1I holds a copy
    uint16_t  plru_bits;
    uint8_t   data[NUM_L2_WAYS][LINE_SIZE];
};

// Sets *way to the matching non-INVALID way index. Returns false on miss.
bool l2_find_way(L2Set *l2Set, uint64_t tag, uint8_t *way);
void l2_cache_read_way(L2Set *l2Set, uint8_t core_id, uint8_t way, uint8_t *data, uint8_t *core_valid);

// Dirty write-back from an L1D eviction. Copies data, sets state to MODIFIED,
// and clears the evicting core's core_valid_d bit. Does not update PLRU.
void l2_cache_write_way(L2Set *l2Set, uint8_t core_id, uint8_t way, uint8_t *data);
void l2_clear_core_valid_way(uint8_t *core_valid, uint8_t core_id);
void l2_cache_fill(L2Set *l2Set, uint8_t core_id, uint64_t tag, uint8_t way, uint8_t *data, MESIState state, uint8_t *core_valid_set, uint8_t *core_valid_clr);
void l2_cache_fill(L2Set *l2Set, uint64_t tag, uint8_t way, uint8_t *data, MESIState state); // prefetch: no L1 owner

#endif // UNCORE_H
