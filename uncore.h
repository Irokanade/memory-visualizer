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

#endif // UNCORE_H