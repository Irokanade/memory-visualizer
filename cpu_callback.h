#ifndef CPU_CALLBACK_H
#define CPU_CALLBACK_H

#include <cstdint>

enum CacheEventType : uint8_t {
    CACHE_L1D_HIT,
    CACHE_L1D_FILL,
    CACHE_L1D_EVICT,
    CACHE_L2_HIT,
    CACHE_L2_FILL,
    CACHE_L2_EVICT,
    CACHE_SNOOP_DOWNGRADE,
    CACHE_SNOOP_INVALIDATE,
    CACHE_PREFETCH_L2,
    CACHE_MEM_FETCH,
    CACHE_L1I_SNOOP_INVALIDATE,
};

using CpuCallback = void (*)(CacheEventType type, uint8_t core_id,
                              uint8_t level, uint16_t set, uint8_t way,
                              uint64_t addr, uint8_t old_state,
                              uint8_t new_state, const uint8_t *data);

extern CpuCallback g_cpu_callback;

#endif // CPU_CALLBACK_H
