### Cache Simulator

Intel Core 2 Duo cache simulator with MESI coherence, TLB, and hardware prefetching. Uses an LLVM pass to instrument programs and trace memory accesses through the simulator. Includes an SDL3 visualizer for stepping through cache events frame-by-frame.

#### Prerequisites

- CMake 3.20+
- g++ with C++20 support
- LLVM (`brew install llvm`)
- SDL3 (`brew install sdl3`)

#### Building

```bash
cmake -B build
cmake --build build
```

#### Running with the visualizer

```bash
cmake --build build --target run_row_major
cmake --build build --target run_col_major
cmake --build build --target run_false_sharing
cmake --build build --target run_false_sharing_padded
cmake --build build --target run_producer_consumer
```

This records a trace and opens the SDL3 visualizer automatically.

#### Visualizer controls

- **Space** — play/pause auto-play (60fps)
- **Left/Right** — step backward/forward
- **Up/Down** — increase/decrease playback speed
- **Esc** — quit

#### Adding your own benchmark

Drop a `.c` or `.cpp` file in `benchmarks/`, then:

```bash
cmake -B build
cmake --build build --target run_your_program
```

CMake auto-discovers new files in `benchmarks/`. Single-core `.c` benchmarks are instrumented via the LLVM pass. Multi-core `.cpp` benchmarks drive the simulator directly.

#### Limitations

- **Data cache only** — the simulator has full L1I/ITLB/instruction prefetcher support (`cpu_fetch`), but the LLVM pass only instruments data loads/stores. Instruction cache simulation requires real instruction addresses which are not available at the IR level.
- **Requires source code** — unlike cachegrind, programs must be recompiled with the LLVM pass.
- **Fake physical addresses** — virtual addresses are real, but PA-to-cache set mapping uses simulator-assigned PAs. L1 indexing is exact (index bits within page offset). L2 conflict misses may differ from real hardware.
