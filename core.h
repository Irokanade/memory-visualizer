#ifndef CORE_H
#define CORE_H

#include "types.h"
#include <cstdint>

constexpr uint8_t NUM_L1_WAYS = 8;
struct L1Set {
    uint64_t tag[NUM_L1_WAYS];
    MESIState state[NUM_L1_WAYS];
    uint8_t plru_bits;
    uint8_t data[NUM_L1_WAYS][LINE_SIZE];
};


struct TLBEntry {
    uint64_t page_num;
    uint64_t page_frame;
    bool valid;
};

constexpr uint8_t L1_SETS = 8;
constexpr uint8_t DTLB_ENTRIES = 256;
constexpr uint8_t ITLB_ENTRIES = 128;
struct Core {
    L1Set l1d[L1_SETS];
    TLBEntry dtlb[DTLB_ENTRIES];
    TLBEntry itlb[ITLB_ENTRIES];
    uint8_t core_id;
};

#endif // CORE_H