### Cache Simulator

Intel Core 2 Duo cache simulator with MESI coherence, TLB, and hardware prefetching. Uses an LLVM pass to instrument programs and trace memory access through the simulator.

#### Prerequisites

- g++ with C++20 support
- LLVM

#### Building

```bash
make
make benchmarks
```

#### Running benchmarks

```bash
make run
```

#### Instrumenting your own program

Drop a `.c` file in `benchmarks/` and `make run` picks it up automatically.

Or compile manually:

```bash
make instrument SRC=your_program.c
DYLD_LIBRARY_PATH=build ./build/your_program
```

#### Limitations

- **Data cache only** — the simulator has full L1I/ITLB/instruction prefetcher support (`cpu_fetch`), but the LLVM pass currently only instruments data loads/stores. Instruction cache simulation requires real instruction addresses which are not available at the IR level. The pass will be updated to support this in the future.
- **Requires source code** — unlike cachegrind, programs must be recompiled with the LLVM pass.
- **Fake physical addresses** — virtual addresses are real, but PA→cache set mapping uses simulator-assigned PAs. L1 indexing is exact (index bits within page offset). L2 conflict misses may differ from real hardware.
