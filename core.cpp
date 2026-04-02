#include "core.h"
#include "plru.h"
#include <cassert>
#include <cstdint>
#include <cstring>

static_assert(L1_SETS      && (L1_SETS      & (L1_SETS      - 1)) == 0, "L1_SETS must be power of 2");
static_assert(L1_DTLB_SETS && (L1_DTLB_SETS & (L1_DTLB_SETS - 1)) == 0, "L1_DTLB_SETS must be power of 2");
static_assert(L2_DTLB_SETS && (L2_DTLB_SETS & (L2_DTLB_SETS - 1)) == 0, "L2_DTLB_SETS must be power of 2");
static_assert(ITLB_SETS    && (ITLB_SETS    & (ITLB_SETS    - 1)) == 0, "ITLB_SETS must be power of 2");

bool tlb_lookup(TLBSet *set, uint64_t virtual_page_num, uint64_t *physical_frame) {
    for (uint8_t way = 0; way < NUM_TLB_WAYS; way++) {
        if (set->valid[way] && set->virtual_page_num[way] == virtual_page_num) {
            *physical_frame = set->physical_frame[way];
            plru_update<uint8_t, NUM_TLB_WAYS>(&set->plru_bits, way);
            return true;
        }
    }

    return false;
}

void tlb_fill(TLBSet *set, uint64_t virtual_page_num, uint64_t physical_frame) {

    uint8_t way;
    for (way = 0; way < NUM_TLB_WAYS && set->valid[way]; way++);

    if (way == NUM_TLB_WAYS) {
        way = plru_victim<uint8_t, NUM_TLB_WAYS>(set->plru_bits);
    }

    set->virtual_page_num[way] = virtual_page_num;
    set->physical_frame[way] = physical_frame;
    set->valid[way] = true;
    plru_update<uint8_t, NUM_TLB_WAYS>(&set->plru_bits, way);
}

bool find_l1_way(const L1Set *set, uint64_t tag, uint8_t *way) {
    for (uint8_t w = 0; w < NUM_L1_WAYS; w++) {
        if (set->tag[w] == tag && set->state[w] != MESIState::INVALID) {
            *way = w;
            return true;
        }
    }
    return false;
}

void l1_cache_read_way(L1Set *l1Set, uint8_t way, uint8_t offset, uint8_t *data, uint8_t data_size) {
    assert(offset + data_size <= LINE_SIZE && "access crosses cache line boundary");
    std::memcpy(data, l1Set->data[way] + offset, data_size);
    plru_update<uint8_t, NUM_L1_WAYS>(&l1Set->plru_bits, way);
}

// Only succeeds on M or E (SHARED requires an RFO first — handled by cpu_write).
void l1_cache_write_way(L1Set *l1Set, uint8_t way, uint8_t offset, uint8_t *data, uint8_t data_size) {
    assert(offset + data_size <= LINE_SIZE && "access crosses cache line boundary");
    std::memcpy(l1Set->data[way] + offset, data, data_size);
    plru_update<uint8_t, NUM_L1_WAYS>(&l1Set->plru_bits, way);
    l1Set->state[way] = MESIState::MODIFIED; // E→M or M→M
}

void l1_cache_fill(L1Set *l1Set, uint64_t tag, uint8_t way, uint8_t *line, MESIState state) {
    l1Set->tag[way] = tag;
    l1Set->state[way] = state;
    std::memcpy(l1Set->data[way], line, LINE_SIZE);
    plru_update<uint8_t, NUM_L1_WAYS>(&l1Set->plru_bits, way);
}

void pf_lru_update(uint8_t *pf_lru_age, uint8_t index) {
    uint8_t old_age = pf_lru_age[index];
    for (uint8_t i = 0; i < NUM_STREAMS; i++) {
        if (i != index && pf_lru_age[i] < old_age) {
            pf_lru_age[i]++;
        }
    }
    pf_lru_age[index] = 0;
}

uint8_t pf_lru_evict(uint8_t *pf_lru_age) {
    uint8_t victim = 0;
    for (uint8_t i = 1; i < NUM_STREAMS; i++) {
        if (pf_lru_age[i] > pf_lru_age[victim]) {
            victim = i;
        }
    }

    return victim;
}
