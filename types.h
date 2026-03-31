#ifndef TYPES_H
#define TYPES_H

#include <cstdint>

constexpr uint8_t LINE_SIZE = 64;

enum MESIState : uint8_t {
    MODIFIED  = 0,
    EXCLUSIVE = 1,
    SHARED    = 2,
    INVALID   = 3
};

constexpr uint64_t to_frame(uint64_t address) {
    return address >> 12;
}

constexpr uint64_t to_address(uint64_t frame, uint64_t offset) {
    return (frame << 12) | offset;
}

constexpr uint64_t l1_to_index(uint64_t address) {
    return (address >> 6) & 0x3F;
}

constexpr uint64_t l2_to_index(uint64_t address) {
    return (address >> 6) & 0xFFF;
}

#endif // TYPES_H