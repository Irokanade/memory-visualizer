#include "cpu.h"
#include "event.h"
#include "os.h"
#include "cpu_callback.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static constexpr uint64_t PHYS_SIZE = 256ULL * 1024 * 1024;
static constexpr uint64_t QUEUE_VA = 0x600000;
static constexpr int ITEMS = 2000;

// Queue layout in simulated memory:
//   offset 0:  head (4 bytes) — written by consumer
//   offset 4:  tail (4 bytes) — written by producer
//   offset 64: buffer[1024]   — written by producer, read by consumer
//
// FALSE SHARING: head and tail are 4 bytes apart on the same cache line.
// Every producer write to tail invalidates the consumer's copy (and vice
// versa).

static constexpr uint64_t HEAD_VA = QUEUE_VA;
static constexpr uint64_t TAIL_VA = QUEUE_VA + sizeof(uint32_t);
static constexpr uint64_t BUF_VA = QUEUE_VA + LINE_SIZE;
static constexpr uint32_t BUF_SIZE = 1024;

static uint8_t phys[PHYS_SIZE];
static Memory mem;
static FrameAllocator fa;
static Process *proc;
static CPU cpu;

static void write32(uint8_t core_id, uint64_t va, uint32_t val)
{
    event_begin_step();
    cpu_write(&cpu, core_id, &mem, va, reinterpret_cast<uint8_t *>(&val),
              sizeof(val));
}

static uint32_t read32(uint8_t core_id, uint64_t va)
{
    uint32_t val;
    event_begin_step();
    cpu_read(&cpu, core_id, &mem, va, reinterpret_cast<uint8_t *>(&val),
             sizeof(val));
    return val;
}

static void producer()
{
    for (uint32_t i = 0; i < ITEMS; i++) {
        uint32_t tail = read32(0, TAIL_VA);
        uint32_t next = (tail + 1) % BUF_SIZE;

        // spin until not full
        while (next == read32(0, HEAD_VA)) {
            std::this_thread::yield();
        }

        // write item to buffer
        uint64_t slot_va = BUF_VA + (tail % BUF_SIZE) * sizeof(uint32_t);
        write32(0, slot_va, i);

        // advance tail
        write32(0, TAIL_VA, next);
    }
}

static void consumer()
{
    for (uint32_t i = 0; i < ITEMS; i++) {
        // spin until not empty
        uint32_t head;
        while ((head = read32(1, HEAD_VA)) == read32(1, TAIL_VA)) {
            std::this_thread::yield();
        }

        // read item from buffer
        uint64_t slot_va = BUF_VA + (head % BUF_SIZE) * sizeof(uint32_t);
        read32(1, slot_va);

        // advance head
        uint32_t next = (head + 1) % BUF_SIZE;
        write32(1, HEAD_VA, next);
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

    uint64_t end_va = BUF_VA + BUF_SIZE * sizeof(uint32_t);
    uint64_t npages = (end_va - QUEUE_VA + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < npages; i++) {
        alloc_and_map(proc, &fa, &mem, QUEUE_VA + i * PAGE_SIZE, true, true);
    }

    // init head = tail = 0
    uint32_t zero = 0;
    cpu_write(&cpu, 0, &mem, HEAD_VA, reinterpret_cast<uint8_t *>(&zero),
              sizeof(zero));
    cpu_write(&cpu, 0, &mem, TAIL_VA, reinterpret_cast<uint8_t *>(&zero),
              sizeof(zero));

    g_event_queue = new EventQueue();
    g_cpu_callback = event_callback;

    printf("=== producer-consumer (head/tail on same cache line) ===\n");
    std::thread t0(producer);
    std::thread t1(consumer);
    t0.join();
    t1.join();

    printf("core 0 (producer): l1d=%llu l2=%llu mem=%llu\n",
           (unsigned long long)cpu.cores[0].pmc.l1d_hits,
           (unsigned long long)cpu.cores[0].pmc.l2_hits,
           (unsigned long long)cpu.cores[0].pmc.mem_fetches);
    printf("core 1 (consumer): l1d=%llu l2=%llu mem=%llu\n",
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
