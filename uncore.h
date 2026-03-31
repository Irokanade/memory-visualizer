#ifndef UNCORE_H
#define UNCORE_H

#include "types.h"
#include <cstdint>

constexpr uint8_t NUM_L2_WAYS = 16;
struct L2Set {
    uint64_t tag[NUM_L2_WAYS];
    MESIState state[NUM_L2_WAYS];
    uint8_t core_valid[NUM_L2_WAYS]; // bit per core which L1s hold a copy of this line
    uint16_t plru_bits;
    uint8_t data[NUM_L2_WAYS][LINE_SIZE];
};

bool l2_cache_read(L2Set *l2Set, uint8_t core_id, uint64_t addr, uint8_t *data);
bool l2_cache_write(L2Set *l2Set, uint8_t core_id, uint64_t addr, uint8_t *data);
void l2_cache_fill(
    L2Set *l2Set, 
    uint8_t core_id,
    uint8_t way, 
    uint64_t tag,
    uint8_t *data,
    MESIState state);


#endif // UNCORE_H