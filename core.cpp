#include "core.h"
#include "plru.h"
#include <cstdint>
#include <cstring>

bool tlb_lookup(
    TLBSet *tlb,
    uint16_t num_sets,
    uint64_t virtual_page_num,
    uint64_t *physical_frame)
{
    uint16_t index = virtual_page_num % num_sets;
    TLBSet *set = &tlb[index];

    for (uint8_t way = 0; way < NUM_TLB_WAYS; way++) {
        if (set->valid[way] && set->virtual_page_num[way] == virtual_page_num) {
            *physical_frame = set->physical_frame[way];
            return true;
        }
    }

    return false;
}

void tlb_fill(
    TLBSet *tlb,
    uint16_t num_sets,
    uint64_t virtual_page_num,
    uint64_t physical_frame)
{
    uint16_t index = virtual_page_num & (num_sets-1);
    TLBSet *set = &tlb[index];

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

bool l1_cache_read(L1Set *l1Set, uint64_t addr, uint8_t *data) {
    uint8_t offset = addr & 0x3F; // low 6 bits
    uint16_t index = (addr >> 6) & 0x3F; // next 6 bits
    uint64_t tag = addr >> 12;

    for (uint8_t way = 0; way < NUM_L1_WAYS; way++) {
        if (l1Set->tag[way] == tag && l1Set->state[way] != MESIState::INVALID) {
            std::memcpy(data, l1Set->data[way], LINE_SIZE);
            plru_update<uint8_t, NUM_L1_WAYS>(&l1Set->plru_bits, way);
            return true;
        }
    }

    return false;
}

bool l1_cache_write(L1Set *l1Set, uint64_t addr, uint8_t *data) {
    uint8_t offset = addr & 0x3F; // low 6 bits
    uint16_t index = (addr >> 6) & 0x3F; // next 6 bits
    uint64_t tag = addr >> 12;

    for (uint8_t way = 0; way < NUM_L1_WAYS; way++) {
        if (l1Set->tag[way] == tag && l1Set->state[way] != MESIState::INVALID) {
            std::memcpy(l1Set->data[way], data, LINE_SIZE);
            plru_update<uint8_t, NUM_L1_WAYS>(&l1Set->plru_bits, way);
            l1Set->state[way] = MESIState::MODIFIED;
            return true;
        }
    }

    return false;
}

void l1_cache_fill(L1Set *l1Set, 
    uint8_t way, 
    uint64_t tag, 
    uint8_t *data,
    MESIState state) 
{
    l1Set->tag[way] = tag;
    l1Set->state[way] = state;
    std::memcpy(l1Set->data[way], data, LINE_SIZE);
    plru_update<uint8_t, NUM_L1_WAYS>(&l1Set->plru_bits, way);
}
