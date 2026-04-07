#include "os.h"
#include "types.h"
#include <cstdlib>
#include <cstring>

uint64_t alloc_frame(FrameAllocator *fa, Memory *mem)
{
    uint64_t pa;
    if (!fa->free_list.empty()) {
        pa = fa->free_list.back();
        fa->free_list.pop_back();
    } else {
        if (fa->watermark + PAGE_SIZE > fa->limit) {
            return UINT64_MAX; // OOM
        }
        pa = fa->watermark;
        fa->watermark += PAGE_SIZE;
    }
    std::memset(&mem->data[pa], 0, PAGE_SIZE);
    return pa;
}

void free_frame(FrameAllocator *fa, uint64_t pa)
{
    fa->free_list.push_back(pa);
}

bool map_page(Process *proc, FrameAllocator *fa, Memory *mem, uint64_t va,
              uint64_t pa, bool writable, bool user)
{
    PageTable *table = &proc->pml4;

    for (int level = 3; level >= 1; level--) {
        uint16_t idx = (va >> (12 + level * 9)) & 0x1FF;
        PageTableEntry *entry = &table->entries[idx];

        if (!entry->present) {
            uint64_t new_pa = alloc_frame(fa, mem);
            if (new_pa == UINT64_MAX) {
                return false;
            }
            entry->physical_address = new_pa;
            entry->present = true;
            entry->writable = true;
            entry->user = user;
        }

        table =
            reinterpret_cast<PageTable *>(&mem->data[entry->physical_address]);
    }

    uint16_t pt_idx = (va >> 12) & 0x1FF;
    table->entries[pt_idx] = {pa, true, writable, user};
    return true;
}

uint64_t alloc_and_map(Process *proc, FrameAllocator *fa, Memory *mem,
                       uint64_t va, bool writable, bool user)
{
    uint64_t pa = alloc_frame(fa, mem);
    if (pa == UINT64_MAX) {
        return UINT64_MAX;
    }
    if (!map_page(proc, fa, mem, va, pa, writable, user)) {
        free_frame(fa, pa);
        return UINT64_MAX;
    }
    return pa;
}

Process *process_create(uint32_t pid)
{
    Process *proc = new Process{};
    proc->pid = pid;
    proc->brk = 0;
    proc->stack_top = 0;
    return proc;
}

void tlb_flush(Core *core)
{
    for (uint8_t s = 0; s < L1_DTLB_SETS; s++) {
        for (uint8_t w = 0; w < NUM_TLB_WAYS; w++) {
            core->l1_dtlb[s].valid[w] = false;
        }
    }
    for (uint8_t s = 0; s < L2_DTLB_SETS; s++) {
        for (uint8_t w = 0; w < NUM_TLB_WAYS; w++) {
            core->l2_dtlb[s].valid[w] = false;
        }
    }
    for (uint8_t s = 0; s < ITLB_SETS; s++) {
        for (uint8_t w = 0; w < NUM_TLB_WAYS; w++) {
            core->itlb[s].valid[w] = false;
        }
    }
}

void process_switch(CPU *cpu, Process *proc)
{
    for (uint8_t c = 0; c < NUM_CORES; c++) {
        cpu->cores[c].active_pml4 = &proc->pml4;
        tlb_flush(&cpu->cores[c]);
    }
}
