#include "cpu.h"
#include "memory.h"
#include "core.h"
#include "os.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>


static constexpr int N = 256;
static constexpr uint64_t MEM_SIZE = 16 * 1024 * 1024; // 16MB physical

static constexpr uint64_t MATRIX_VA          = VA_DATA;           // 0x600000
static constexpr uint64_t FALSE_SHARING_VA   = VA_DATA + 0x100000; // 0x700000 — same cache line
static constexpr uint64_t NO_FALSE_SHARING_VA = VA_DATA + 0x200000; // 0x800000 — different cache lines

static constexpr int ITERS = 100000;

static void setup_address_space(Process *proc, FrameAllocator *fa, Memory *mem) {
    // Map matrix pages (N*N*4 bytes = 256KB = 64 pages)
    uint64_t npages = (static_cast<uint64_t>(N) * N * sizeof(int32_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < npages; i++) {
        alloc_and_map(proc, fa, mem, MATRIX_VA + i * PAGE_SIZE, true, true);
    }

    // Map counter pages for false sharing demos (one page each)
    alloc_and_map(proc, fa, mem, FALSE_SHARING_VA,    true, true);
    alloc_and_map(proc, fa, mem, NO_FALSE_SHARING_VA, true, true);
}

static void run_row_major(CPU *cpu, Memory *mem) {
    uint8_t buf[sizeof(int32_t)];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint64_t va = MATRIX_VA + static_cast<uint64_t>(i * N + j) * sizeof(int32_t);
            cpu_read(cpu, 0, mem, va, buf, sizeof(int32_t));
        }
    }
}

static void run_col_major(CPU *cpu, Memory *mem) {
    uint8_t buf[sizeof(int32_t)];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            uint64_t va = MATRIX_VA + static_cast<uint64_t>(j * N + i) * sizeof(int32_t);
            cpu_read(cpu, 0, mem, va, buf, sizeof(int32_t));
        }
    }
}

static void counter_rmw(CPU *cpu, uint8_t core_id, Memory *mem, uint64_t addr) {
    uint32_t val;
    for (int i = 0; i < ITERS; i++) {
        cpu_read (cpu, core_id, mem, addr, reinterpret_cast<uint8_t *>(&val), sizeof(val));
        val++;
        cpu_write(cpu, core_id, mem, addr, reinterpret_cast<uint8_t *>(&val), sizeof(val));
        std::this_thread::yield(); // hints the OS to switch threads to increase interleaving
    }
}

static void run_false_sharing(CPU *cpu, Memory *mem) {
    // Both counters on the same 64-byte cache line.
    std::thread t0([&]{ counter_rmw(cpu, 0, mem, FALSE_SHARING_VA); });
    std::thread t1([&]{ counter_rmw(cpu, 1, mem, FALSE_SHARING_VA + sizeof(uint32_t)); });
    t0.join();
    t1.join();
}

static void run_no_false_sharing(CPU *cpu, Memory *mem) {
    // Counters 64 bytes apart — each on its own cache line.
    std::thread t0([&]{ counter_rmw(cpu, 0, mem, NO_FALSE_SHARING_VA); });
    std::thread t1([&]{ counter_rmw(cpu, 1, mem, NO_FALSE_SHARING_VA + LINE_SIZE); });
    t0.join();
    t1.join();
}

static void print_pmc(const char *label, const PerfCounters *pmc) {
    uint64_t total = pmc->l1d_hits + pmc->l2_hits + pmc->mem_fetches;
    printf("%-20s  l1d=%6llu  l2=%6llu  mem=%6llu  total=%6llu\n",
        label,
        (unsigned long long)pmc->l1d_hits,
        (unsigned long long)pmc->l2_hits,
        (unsigned long long)pmc->mem_fetches,
        (unsigned long long)total);
}

int main() {
    uint8_t *phys = static_cast<uint8_t *>(std::calloc(MEM_SIZE, 1));
    if (!phys) return 1;

    Memory mem{};
    mem.data = phys;
    mem.size = MEM_SIZE;

    FrameAllocator fa{};
    fa.watermark = PAGE_SIZE;
    fa.limit = MEM_SIZE;

    Process *proc = process_create(1);
    setup_address_space(proc, &fa, &mem);

    CPU *cpu;

    // --- matrix traversal ---
    printf("=== matrix traversal (single core) ===\n");
    cpu = new CPU{};
    process_switch(cpu, proc);
    run_row_major(cpu, &mem);
    print_pmc("row-major:", &cpu->cores[0].pmc);
    delete cpu;

    cpu = new CPU{};
    process_switch(cpu, proc);
    run_col_major(cpu, &mem);
    print_pmc("col-major:", &cpu->cores[0].pmc);
    delete cpu;

    // --- false sharing ---
    printf("\n=== false sharing (2 cores, %d iters each, yield between iters) ===\n", ITERS);
    printf("  false sharing:    counters 4 bytes apart (same cache line)\n");
    printf("  no false sharing: counters 64 bytes apart (different cache lines)\n\n");

    cpu = new CPU{};
    process_switch(cpu, proc);
    run_false_sharing(cpu, &mem);
    print_pmc("false-share c0:", &cpu->cores[0].pmc);
    print_pmc("false-share c1:", &cpu->cores[1].pmc);
    delete cpu;

    cpu = new CPU{};
    process_switch(cpu, proc);
    run_no_false_sharing(cpu, &mem);
    print_pmc("no-false-share c0:", &cpu->cores[0].pmc);
    print_pmc("no-false-share c1:", &cpu->cores[1].pmc);
    delete cpu;

    delete proc;
    std::free(phys);
    return 0;
}
