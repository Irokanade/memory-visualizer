CXX      := g++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra
BUILD    := build

LLVM_DIR := /opt/homebrew/Cellar/llvm/22.1.2
LLVM_CXX := $(LLVM_DIR)/bin/clang++
LLVM_CXXFLAGS := $(shell $(LLVM_DIR)/bin/llvm-config --cxxflags) -fPIC -shared -undefined dynamic_lookup

LIB_SRCS := cachesim.cpp core.cpp uncore.cpp cpu.cpp memory.cpp os.cpp
LIB_OBJS := $(addprefix $(BUILD)/,$(LIB_SRCS:.cpp=.o))
LIB      := $(BUILD)/libcachesim.so
PASS     := $(BUILD)/mem_trace_pass.so

.PHONY: all clean

all: $(LIB) $(PASS)

$(BUILD):
	mkdir -p $(BUILD)

$(LIB): $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^

$(PASS): mem_trace_pass.cpp | $(BUILD)
	$(LLVM_CXX) $(LLVM_CXXFLAGS) -o $@ $<

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

clean:
	rm -rf $(BUILD)
