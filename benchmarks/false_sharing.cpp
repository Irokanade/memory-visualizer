#include "cpu.h"
#include "event.h"
#include "cpu_callback.h"
#include "os.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static constexpr uint64_t PHYS_SIZE = 256ULL * 1024 * 1024;
static constexpr uint64_t COUNTER_VA = 0x600000;
static constexpr int ITERS = 5000;

static uint8_t phys[PHYS_SIZE];
static Memory mem;
static FrameAllocator fa;
static Process *proc;
static CPU cpu;

static void counter_rmw(uint8_t core_id, uint64_t addr)
{
    uint32_t val;
    for (int i = 0; i < ITERS; i++) {
        event_begin_step();
        cpu_read(&cpu, core_id, &mem, addr,
                 reinterpret_cast<uint8_t *>(&val), sizeof(val));
        val++;
        event_begin_step();
        cpu_write(&cpu, core_id, &mem, addr,
                  reinterpret_cast<uint8_t *>(&val), sizeof(val));
        std::this_thread::yield();
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("usage: %s <trace.bin>\n", argv[0]);
        return 1;
    }

    mem.data = phys;
    mem.size = PHYS_SIZE;
    fa.watermark = PAGE_SIZE;
    fa.limit = PHYS_SIZE;
    proc = process_create(1);
    process_switch(&cpu, proc);

    alloc_and_map(proc, &fa, &mem, COUNTER_VA, true, true);

    g_event_queue = new EventQueue();
    g_cpu_callback = event_callback;

    printf("=== false sharing (4 bytes apart, same cache line) ===\n");
    std::thread t0([&] { counter_rmw(0, COUNTER_VA); });
    std::thread t1([&] { counter_rmw(1, COUNTER_VA + sizeof(uint32_t)); });
    t0.join();
    t1.join();

    printf("core 0: l1d=%llu l2=%llu mem=%llu\n",
           (unsigned long long)cpu.cores[0].pmc.l1d_hits,
           (unsigned long long)cpu.cores[0].pmc.l2_hits,
           (unsigned long long)cpu.cores[0].pmc.mem_fetches);
    printf("core 1: l1d=%llu l2=%llu mem=%llu\n",
           (unsigned long long)cpu.cores[1].pmc.l1d_hits,
           (unsigned long long)cpu.cores[1].pmc.l2_hits,
           (unsigned long long)cpu.cores[1].pmc.mem_fetches);

    if (write_trace(argv[1], g_event_queue)) {
        printf("trace written to %s (%llu events)\n", argv[1],
               (unsigned long long)g_event_queue->count);
    }

    delete g_event_queue;
    delete proc;
    return 0;
}
