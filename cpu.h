#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include <mutex>
#include "core.h"
#include "types.h"
#include "uncore.h"
#include "memory.h"

struct CPU {
    Core cores[NUM_CORES];
    L2Set l2Sets[L2_SETS];
    std::mutex bus_lock; // serialises all bus transactions — models the shared Core 2 FSB
};

// Precondition: the access [virtual_address, virtual_address + data_size) must not cross a
// cache line boundary. Callers are responsible for splitting unaligned cross-line accesses.
bool cpu_read (CPU *cpu, uint8_t core_id, Memory *mem, uint64_t virtual_address, uint8_t *data, uint8_t data_size);
bool cpu_write(CPU *cpu, uint8_t core_id, Memory *mem, uint64_t virtual_address, uint8_t *data, uint8_t data_size);
bool cpu_fetch(CPU *cpu, uint8_t core_id, Memory *mem, uint64_t virtual_address, uint8_t *data, uint8_t data_size);

#endif // CPU_H
