#include "cpu.h"
#include "core.h"
#include "memory.h"

#include <cstdint>

bool translate(Core *core, Memory *mem, uint64_t virtual_address, uint64_t *physical_address) {
    uint64_t virtual_page_num = to_frame(virtual_address);
    uint64_t physical_frame;

    if (tlb_lookup(core->l1_dtlb, L1_DTLB_SETS, virtual_page_num, &physical_frame)) {
        *physical_address = to_address(physical_frame, virtual_address & 0xFFF);
        return true;
    }

    if (tlb_lookup(core->l2_dtlb, L2_DTLB_SETS, virtual_page_num, &physical_frame)) {
        *physical_address = to_address(physical_frame, virtual_address & 0xFFF);
        tlb_fill(core->l1_dtlb, L1_DTLB_SETS, virtual_page_num, physical_frame);
        return true;
    }

    if (page_walk(mem, virtual_address, physical_address)) {
        uint64_t frame = to_frame(*physical_address);
        tlb_fill(core->l1_dtlb, L1_DTLB_SETS, virtual_page_num, frame);
        tlb_fill(core->l2_dtlb, L2_DTLB_SETS, virtual_page_num, frame);
        return true;
    }

    return false; // page fault
}

bool cpu_read(Core *core, uint64_t addr, uint8_t *data) {
    return false;
}