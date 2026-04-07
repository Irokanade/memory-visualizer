#ifndef MEMORY_H
#define MEMORY_H

#include <cstdint>

struct PageTableEntry {
    uint64_t physical_address;
    bool present;
    bool writable;
    bool user;
};

constexpr uint16_t PAGE_TABLE_ENTRIES = 512;
struct PageTable {
    PageTableEntry entries[PAGE_TABLE_ENTRIES];
};

struct Memory {
    uint8_t *data;
    uint64_t size;
};

bool page_walk(PageTable *pml4, Memory *mem, uint64_t virtual_address,
               uint64_t *physical_address);

#endif // MEMORY_H