#include "core.h"
#include "plru.h"
#include <cstdint>
#include <cstring>

bool tlb_lookup(
    TLBEntry *tlb, 
    uint16_t num_entries, 
    uint64_t virtual_page_num,
    uint64_t *physical_frame) 
{
    for (uint16_t i = 0; i < num_entries; i++) {
        if (tlb[i].valid && tlb[i].virtual_page_num == virtual_page_num) {
            *physical_frame = tlb[i].physical_frame;
            return true;
        }
    }

    return false;
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
