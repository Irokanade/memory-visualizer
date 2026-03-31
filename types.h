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

constexpr uint64_t to_frame(uint64_t addr) {
    return addr >> 12;
}

constexpr uint64_t to_address(uint64_t frame, uint64_t offset) {
    return (frame << 12) | offset;
}

#endif // TYPES_H