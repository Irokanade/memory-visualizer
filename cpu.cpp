#include "cpu.h"
#include "core.h"
#include "memory.h"

#include <cstdint>

void translate(Core *core, Memory *mem, uint64_t virtual_address, uint64_t *physical_address) {
    uint64_t virtual_page_num = virtual_address >> 12;
    uint64_t physical_frame;

    if (tlb_lookup(core->dtlb, DTLB_ENTRIES, virtual_page_num, &physical_frame)) {
        *physical_address = (physical_frame << 12) | (virtual_address & 0xFFF);
        return;
    }

    if (page_walk(mem, virtual_address, physical_address)) {
        // TODO: fill TLB with new mapping
        return;
    }
}

bool cpu_read(Core *core, uint64_t addr, uint8_t *data) {
    return false;
}