CXX      := g++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra
BUILD    := build

LLVM_DIR := /opt/homebrew/Cellar/llvm/22.1.2
LLVM_CC  := $(LLVM_DIR)/bin/clang
LLVM_CXX := $(LLVM_DIR)/bin/clang++
LLVM_CXXFLAGS := $(shell $(LLVM_DIR)/bin/llvm-config --cxxflags) -fPIC -shared -undefined dynamic_lookup

LIB_SRCS := cachesim.cpp core.cpp uncore.cpp cpu.cpp memory.cpp os.cpp
LIB_OBJS := $(addprefix $(BUILD)/,$(LIB_SRCS:.cpp=.o))
LIB      := $(BUILD)/libcachesim.so
PASS     := $(BUILD)/mem_trace_pass.so

BENCH_SRCS := $(wildcard benchmarks/*.c)
BENCH_BINS := $(addprefix $(BUILD)/,$(notdir $(BENCH_SRCS:.c=)))

.PHONY: all clean benchmarks run instrument

all: $(LIB) $(PASS)

benchmarks: $(BENCH_BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(LIB): $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^

$(PASS): mem_trace_pass.cpp | $(BUILD)
	$(LLVM_CXX) $(LLVM_CXXFLAGS) -o $@ $<

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

$(BUILD)/%: benchmarks/%.c $(LIB) $(PASS) | $(BUILD)
	$(LLVM_CC) -O0 -fpass-plugin=$(PASS) -L$(BUILD) -lcachesim $< -o $@

run: benchmarks
	@for bin in $(BENCH_BINS); do \
		echo "=== $$(basename $$bin) ==="; \
		DYLD_LIBRARY_PATH=$(BUILD) $$bin; \
		echo ""; \
	done

instrument: $(LIB) $(PASS) | $(BUILD)
	$(LLVM_CC) -O0 -fpass-plugin=$(PASS) -L$(BUILD) -lcachesim $(SRC) -o $(BUILD)/$(basename $(notdir $(SRC)))

clean:
	rm -rf $(BUILD)
