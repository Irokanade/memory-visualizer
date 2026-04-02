#include "cpu.h"
#include "memory.h"
#include "core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// 256x256 matrix of int32_t
static constexpr int      N         = 256;
static constexpr uint64_t MEM_SIZE  = 16 * 1024 * 1024; // 16MB physical

// Physical layout (identity-mapped: VA == PA for the matrix range):
//   0x000000 — PDPT  (8KB)
//   0x002000 — PD    (8KB)
//   0x004000 — PT    (8KB)
//   0x010000 — matrix data (N*N*4 = 256KB, 64 pages)
static constexpr uint64_t PDPT_PA   = 0x000000;
static constexpr uint64_t PD_PA     = 0x002000;
static constexpr uint64_t PT_PA     = 0x004000;
static constexpr uint64_t MATRIX_PA = 0x010000;
static constexpr uint64_t MATRIX_VA = 0x010000;

static void setup_memory(Memory *mem) {
    // PML4[0] → PDPT
    mem->pml4.entries[0] = { PDPT_PA, true };

    // PDPT[0] → PD
    auto *pdpt = reinterpret_cast<PageTable *>(&mem->data[PDPT_PA]);
    pdpt->entries[0] = { PD_PA, true };

    // PD[0] → PT
    auto *pd = reinterpret_cast<PageTable *>(&mem->data[PD_PA]);
    pd->entries[0] = { PT_PA, true };

    // PT: map each matrix page (VA == PA identity)
    auto *pt        = reinterpret_cast<PageTable *>(&mem->data[PT_PA]);
    uint64_t base   = (MATRIX_VA >> 12) & 0x1FF; // PT index of first matrix page
    uint64_t npages = (static_cast<uint64_t>(N) * N * sizeof(int32_t) + 0xFFF) >> 12;
    for (uint64_t i = 0; i < npages; i++) {
        pt->entries[base + i] = { MATRIX_PA + i * 0x1000, true };
    }
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

static void print_pmc(const char *label, const PerfCounters *pmc) {
    uint64_t total = pmc->l1d_hits + pmc->l2_hits + pmc->l2_prefetch_hits + pmc->mem_fetches;
    printf("%-14s  l1d=%6llu  l2=%6llu  pf_hits=%6llu  mem=%6llu  total=%6llu\n",
        label,
        (unsigned long long)pmc->l1d_hits,
        (unsigned long long)pmc->l2_hits,
        (unsigned long long)pmc->l2_prefetch_hits,
        (unsigned long long)pmc->mem_fetches,
        (unsigned long long)total);
}

int main() {
    uint8_t *phys = static_cast<uint8_t *>(std::calloc(MEM_SIZE, 1));
    if (!phys) return 1;

    Memory mem{};
    mem.data = phys;
    mem.size = MEM_SIZE;
    setup_memory(&mem);

    CPU *cpu = new CPU{};

    run_row_major(cpu, &mem);
    print_pmc("row-major:", &cpu->cores[0].pmc);
    delete cpu;

    cpu = new CPU{};
    run_col_major(cpu, &mem);
    print_pmc("col-major:", &cpu->cores[0].pmc);
    delete cpu;
    std::free(phys);
    return 0;
}
