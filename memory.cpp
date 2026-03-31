#include "memory.h"

bool page_walk(Memory *mem, uint64_t virtual_address, uint64_t *physical_address) {
    PageTable *table = &mem->pml4;

    for (int level = 3; level >= 0; level--) {
        uint16_t index = (virtual_address >> (12 + level * 9)) & 0x1FF;
        PageTableEntry *entry = &table->entries[index];

        if (!entry->present) {
            return false;
        }

        if (level == 0) {
            *physical_address = entry->physical_address | (virtual_address & 0xFFF);
            return true;
        }

        if (mem->size < sizeof(PageTable) ||
            entry->physical_address > mem->size - sizeof(PageTable)) {
            return false;
        }
        table = reinterpret_cast<PageTable*>(&mem->data[entry->physical_address]);
    }

    return false;
}

