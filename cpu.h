#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include "core.h"
#include "uncore.h"
#include "memory.h"

// Precondition: the access [virtual_address, virtual_address + data_size) must not cross a
// cache line boundary. Callers are responsible for splitting unaligned cross-line accesses.
bool cpu_read (Core *cores, uint8_t core_id, L2Set *l2Sets, Memory *mem, uint64_t virtual_address, uint8_t *data, uint8_t data_size);
bool cpu_write(Core *cores, uint8_t core_id, L2Set *l2Sets, Memory *mem, uint64_t virtual_address, uint8_t *data, uint8_t data_size);
bool cpu_fetch(Core *cores, uint8_t core_id, L2Set *l2Sets, Memory *mem, uint64_t virtual_address, uint8_t *data, uint8_t data_size);

#endif // CPU_H
