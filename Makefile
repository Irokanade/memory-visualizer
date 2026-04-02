CXX      := g++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra

SRCS := main.cpp core.cpp uncore.cpp cpu.cpp memory.cpp
OBJS := $(SRCS:.cpp=.o)
TARGET := main

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
