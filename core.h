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

constexpr uint8_t NUM_TLB_WAYS = 4;
struct TLBSet {
    uint64_t virtual_page_num[NUM_TLB_WAYS];
    uint64_t physical_frame[NUM_TLB_WAYS];
    uint8_t plru_bits;
    bool valid[NUM_TLB_WAYS];
};

constexpr uint8_t L1_SETS = 64;
constexpr uint8_t L1_DTLB_SETS = 4;   // 16 entries / 4 ways
constexpr uint8_t L2_DTLB_SETS = 64;  // 256 entries / 4 ways
constexpr uint8_t ITLB_SETS = 32;     // 128 entries / 4 ways
struct Core {
    L1Set l1d[L1_SETS];
    TLBSet l1_dtlb[L1_DTLB_SETS];
    TLBSet l2_dtlb[L2_DTLB_SETS];
    TLBSet itlb[ITLB_SETS];
    uint8_t core_id;
};

bool tlb_lookup(
    TLBSet *tlb,
    uint16_t num_sets,
    uint64_t virtual_page_num,
    uint64_t *physical_frame);

void tlb_fill(
    TLBSet *tlb,
    uint16_t num_sets,
    uint64_t virtual_page_num,
    uint64_t physical_frame);

#endif // CORE_H