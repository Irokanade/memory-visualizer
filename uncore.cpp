#include "uncore.h"
#include "plru.h"
#include <cstring>

bool l2_cache_read(L2Set *l2Set, uint8_t core_id, uint64_t addr, uint8_t *data) {
    uint8_t offset = addr & 0x3F; // low 6 bits
    uint16_t index = (addr >> 6) & 0xFFF; // next 12 bits
    uint64_t tag = addr >> 18;

    for (uint8_t way = 0; way < NUM_L2_WAYS; way++) {
        if (l2Set->tag[way] == tag && l2Set->state[way] != MESIState::INVALID) {
            std::memcpy(data, l2Set->data[way], LINE_SIZE);
            plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, way);
            l2Set->core_valid[way] |= (1 << core_id);
            return true;
        }
    }

    return false;
}

bool l2_cache_write(L2Set *l2Set, uint8_t core_id, uint64_t addr, uint8_t *data) {
    uint8_t offset = addr & 0x3F; // low 6 bits
    uint16_t index = (addr >> 6) & 0xFFF; // next 12 bits
    uint64_t tag = addr >> 18;

    for (uint8_t way = 0; way < NUM_L2_WAYS; way++) {
        if (l2Set->tag[way] == tag && l2Set->state[way] != MESIState::INVALID) {
            std::memcpy(l2Set->data[way], data, LINE_SIZE);
            plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, way);
            l2Set->state[way] = MESIState::MODIFIED;
            l2Set->core_valid[way] |= (1 << core_id);
            return true;
        }
    }

    return false;
}

void l2_cache_fill(
    L2Set *l2Set, 
    uint8_t core_id,
    uint8_t way, 
    uint64_t tag,
    uint8_t *data,
    MESIState state) 
{
    l2Set->tag[way] = tag;
    l2Set->state[way] = state;
    std::memcpy(l2Set->data[way], data, LINE_SIZE);
    plru_update<uint16_t, NUM_L2_WAYS>(&l2Set->plru_bits, way);
    l2Set->core_valid[way] = (1 << core_id);
}