#ifndef OS_H
#define OS_H

#include "memory.h"
#include "cpu.h"
#include "core.h"
#include <cstdint>
#include <vector>

// x86-64 Linux-like virtual address space layout (no ASLR)
constexpr uint64_t PAGE_SIZE = 0x1000;
constexpr uint64_t VA_TEXT = 0x400000;
constexpr uint64_t VA_DATA = 0x600000;
constexpr uint64_t VA_STACK_TOP = (1ULL << 47) - PAGE_SIZE;

struct FrameAllocator {
    uint64_t watermark;
    uint64_t limit;
    std::vector<uint64_t> free_list;
};

struct Process {
    uint32_t  pid;
    PageTable pml4;
    uint64_t  brk;
    uint64_t  stack_top;
};

uint64_t alloc_frame(FrameAllocator *fa, Memory *mem);
void free_frame(FrameAllocator *fa, uint64_t pa);

bool map_page(Process *proc, FrameAllocator *fa, Memory *mem,
              uint64_t va, uint64_t pa, bool writable, bool user);

uint64_t alloc_and_map(Process *proc, FrameAllocator *fa, Memory *mem,
                       uint64_t va, bool writable, bool user);

Process *process_create(uint32_t pid);
void process_switch(CPU *cpu, Process *proc);
void tlb_flush(Core *core);

#endif // OS_H
