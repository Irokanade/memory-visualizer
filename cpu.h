#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include "core.h"

bool cpu_read(Core *core, uint64_t addr, uint8_t *data);
bool cpu_write(Core *core, uint64_t addr, uint8_t *data);

#endif // CPU_H