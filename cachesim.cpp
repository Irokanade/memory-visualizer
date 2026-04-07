#include "cachesim.h"
#include "cpu.h"
#include "os.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr uint64_t PHYS_SIZE = 256ULL * 1024 * 1024;

static uint8_t phys[PHYS_SIZE];
static Memory mem;
static FrameAllocator fa;
static Process *proc;
static CPU cpu;

__attribute__((constructor)) static void cachesim_init(void)
{
    mem.data = phys;
    mem.size = PHYS_SIZE;
    fa.watermark = PAGE_SIZE;
    fa.limit = PHYS_SIZE;
    proc = process_create(1);
    process_switch(&cpu, proc);
}

extern "C" void cachesim_map_region(uint64_t start, uint64_t size)
{
    uint64_t page = start & ~(PAGE_SIZE - 1);
    uint64_t end = (start + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    while (page < end) {
        uint64_t pa;
        if (!page_walk(&proc->pml4, &mem, page, &pa))
            alloc_and_map(proc, &fa, &mem, page, true, true);
        page += PAGE_SIZE;
    }
}

extern "C" void cachesim_unmap_region(uint64_t start, uint64_t size)
{
    (void)start;
    (void)size;
}

static void ensure_page(uint64_t va)
{
    uint64_t pa;
    uint64_t page = va & ~(PAGE_SIZE - 1);
    if (!page_walk(&proc->pml4, &mem, va, &pa)) {
        alloc_and_map(proc, &fa, &mem, page, true, true);
    }
}

static inline bool is_cacheline_split(uint64_t addr, uint32_t size)
{
    return (addr >> 6) != ((addr + size - 1) >> 6);
}

extern "C" void cachesim_read(uint64_t addr, uint32_t size)
{
    ensure_page(addr);
    uint8_t buf[LINE_SIZE];
    __builtin_memcpy(buf, reinterpret_cast<void *>(addr), size);
    cpu_read(&cpu, 0, &mem, addr, buf, size);
    if (is_cacheline_split(addr, size)) {
        uint64_t addr2 =
            (addr + size - 1) & ~static_cast<uint64_t>(LINE_SIZE - 1);
        uint8_t size2 = static_cast<uint8_t>((addr + size) - addr2);
        ensure_page(addr2);
        uint8_t buf2[LINE_SIZE];
        __builtin_memcpy(buf2, reinterpret_cast<void *>(addr2), size2);
        cpu_read(&cpu, 0, &mem, addr2, buf2, size2);
    }
}

extern "C" void cachesim_write(uint64_t addr, uint32_t size)
{
    ensure_page(addr);
    uint8_t buf[LINE_SIZE];
    __builtin_memcpy(buf, reinterpret_cast<void *>(addr), size);
    cpu_write(&cpu, 0, &mem, addr, buf, size);
    if (is_cacheline_split(addr, size)) {
        uint64_t addr2 =
            (addr + size - 1) & ~static_cast<uint64_t>(LINE_SIZE - 1);
        uint8_t size2 = static_cast<uint8_t>((addr + size) - addr2);
        ensure_page(addr2);
        uint8_t buf2[LINE_SIZE];
        __builtin_memcpy(buf2, reinterpret_cast<void *>(addr2), size2);
        cpu_write(&cpu, 0, &mem, addr2, buf2, size2);
    }
}

extern "C" void cachesim_fetch(uint64_t addr, uint32_t size)
{
    uint64_t end = addr + size;
    while (addr < end) {
        uint64_t line_end = ((addr >> 6) + 1) << 6;
        if (line_end > end) {
            line_end = end;
        }
        uint8_t chunk = static_cast<uint8_t>(line_end - addr);
        ensure_page(addr);
        uint8_t buf[LINE_SIZE];
        __builtin_memcpy(buf, reinterpret_cast<void *>(addr), chunk);
        cpu_fetch(&cpu, 0, &mem, addr, buf, chunk);
        addr = line_end;
    }
}

__attribute__((destructor)) extern "C" void cachesim_finish(void)
{
    uint64_t total_d = 0;

    printf("%-12s  %10s  %10s  %10s\n", "core", "l1d_hits", "l2_hits",
           "mem_fetches");

    for (uint8_t i = 0; i < NUM_CORES; i++) {
        const PerfCounters *p = &cpu.cores[i].pmc;
        printf("core%-8u  %10llu  %10llu  %10llu\n", i,
               (unsigned long long)p->l1d_hits, (unsigned long long)p->l2_hits,
               (unsigned long long)p->mem_fetches);
        total_d += p->l1d_hits + p->l2_hits + p->mem_fetches;
    }

    printf("\ntotal data accesses: %llu\n", (unsigned long long)total_d);
}
