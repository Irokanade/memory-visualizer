### Cache Simulator

Intel Core 2 Duo cache simulator with MESI coherence, TLB, hardware prefetching, and an SDL3 visualizer. Write C++ programs that drive the simulator directly — call `cpu_read`/`cpu_write` with addresses and watch cache state transitions frame-by-frame.

#### Prerequisites

- CMake 3.20+
- g++ with C++20 support
- SDL3 (`brew install sdl3`)

#### Building

```bash
cmake -B build
cmake --build build
```

#### Running benchmarks

```bash
cmake --build build --target run_false_sharing
cmake --build build --target run_false_sharing_padded
cmake --build build --target run_producer_consumer
```

This records a trace and opens the SDL3 visualizer automatically.

#### Writing your own benchmark

Create a `.cpp` file in `benchmarks/`. CMake auto-discovers it.

```cpp
#include "cpu.h"
#include "event.h"
#include "cpu_callback.h"
#include "os.h"

#include <cstdio>
#include <cstring>

static constexpr uint64_t PHYS_SIZE = 256ULL * 1024 * 1024;
static constexpr uint64_t DATA_VA = 0x600000;

static uint8_t phys[PHYS_SIZE];
static Memory mem;
static FrameAllocator fa;
static Process *proc;
static CPU cpu;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("usage: %s <trace.bin>\n", argv[0]);
        return 1;
    }

    // Init simulator
    mem.data = phys;
    mem.size = PHYS_SIZE;
    fa.watermark = PAGE_SIZE;
    fa.limit = PHYS_SIZE;
    proc = process_create(1);
    process_switch(&cpu, proc);

    // Map pages for your data region
    alloc_and_map(proc, &fa, &mem, DATA_VA, true, true);

    // Enable trace recording
    g_event_queue = new EventQueue();
    g_cpu_callback = event_callback;

    // Drive the simulator
    uint32_t val = 42;
    event_begin_step();
    cpu_write(&cpu, 0, &mem, DATA_VA,
              reinterpret_cast<uint8_t *>(&val), sizeof(val));

    event_begin_step();
    cpu_read(&cpu, 0, &mem, DATA_VA,
             reinterpret_cast<uint8_t *>(&val), sizeof(val));

    // Write trace
    if (write_trace(argv[1], g_event_queue)) {
        printf("trace written to %s (%llu events)\n", argv[1],
               (unsigned long long)g_event_queue->count);
    }

    delete g_event_queue;
    delete proc;
    return 0;
}
```

#### Multi-core example

Use two threads with different `core_id` values. The simulator's `bus_lock` serializes access:

```cpp
std::thread t0([&] {
    for (int i = 0; i < 1000; i++) {
        event_begin_step();
        cpu_write(&cpu, 0, &mem, addr, buf, 4);  // core 0
    }
});
std::thread t1([&] {
    for (int i = 0; i < 1000; i++) {
        event_begin_step();
        cpu_write(&cpu, 1, &mem, addr + 4, buf, 4);  // core 1, same cache line
    }
});
t0.join();
t1.join();
```

#### API reference

```cpp
// Simulate a data read. Returns false on page fault.
bool cpu_read(CPU *cpu, uint8_t core_id, Memory *mem,
              uint64_t virtual_address, uint8_t *data, uint8_t data_size);

// Simulate a data write. Returns false on page fault.
bool cpu_write(CPU *cpu, uint8_t core_id, Memory *mem,
               uint64_t virtual_address, uint8_t *data, uint8_t data_size);

// Simulate an instruction fetch. Returns false on page fault.
bool cpu_fetch(CPU *cpu, uint8_t core_id, Memory *mem,
               uint64_t virtual_address, uint8_t *data, uint8_t data_size);

// Call before each cpu_read/cpu_write to assign a sequence number for the visualizer.
void event_begin_step();

// Write/read trace files for the visualizer.
bool write_trace(const char *path, EventQueue *queue);
EventQueue *read_trace(const char *path);
```

#### Setup boilerplate

Every benchmark needs this setup before calling `cpu_read`/`cpu_write`:

1. Allocate a `uint8_t phys[]` array (simulated physical memory)
2. Init `Memory`, `FrameAllocator`, `Process`, `CPU` structs
3. Call `alloc_and_map()` for each virtual page your program uses
4. Set `g_event_queue` and `g_cpu_callback` for trace recording

See `benchmarks/false_sharing.cpp` for a complete example.

#### Visualizer controls

- **Space** — play/pause auto-play (60fps)
- **Left/Right** — step backward/forward
- **Up/Down** — increase/decrease playback speed
- **Esc** — quit

#### Limitations

- **Data cache focus** — `cpu_fetch` is fully implemented (L1I, ITLB, next-line prefetcher) but benchmarks only exercise data paths. Instruction cache simulation requires real instruction addresses from a binary emulator.
- **Two cores max** — models Intel Core 2 Duo. The `core_valid` bitmasks are `uint8_t` (supports up to 8 cores) but only 2 are configured.
- **Fake physical addresses** — virtual addresses map to sequential physical frames via `FrameAllocator`. L1 indexing is exact (bits [11:6] within page offset). L2 conflict misses may differ from real hardware.
- **No OS interaction** — programs drive the simulator directly via C++ API calls. There is no syscall emulation or binary loading.
