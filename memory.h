#ifndef MEMORY_H
#define MEMORY_H

#include <cstdint>

struct PageTableEntry {
    uint64_t physical_address;
    bool present;
};

constexpr uint16_t PAGE_TABLE_ENTRIES = 512;
struct PageTable {
    PageTableEntry entries[PAGE_TABLE_ENTRIES];
};
 
struct Memory {
    uint8_t *data;
    uint64_t size;
    PageTable pml4;
};

#endif // MEMORY_H