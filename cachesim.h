#ifndef CACHESIM_H
#define CACHESIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cachesim_map_region(uint64_t start, uint64_t size);
void cachesim_unmap_region(uint64_t start, uint64_t size);

void cachesim_read(uint64_t addr, uint32_t size);
void cachesim_write(uint64_t addr, uint32_t size);
void cachesim_fetch(uint64_t addr, uint32_t size);

void cachesim_finish(void);

#ifdef __cplusplus
}
#endif

#endif // CACHESIM_H
