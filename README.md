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
