#ifndef TYPES_H
#define TYPES_H

#include <cstdint>

constexpr uint8_t LINE_SIZE = 64;
constexpr uint8_t LINE_OFFSET_MASK = LINE_SIZE - 1; // 0x3F
constexpr uint8_t NUM_CORES = 2;                    // Intel Core 2 Duo

enum MESIState : uint8_t {
    INVALID = 0,
    MODIFIED = 1,
    EXCLUSIVE = 2,
    SHARED = 3
};

constexpr uint64_t to_frame(uint64_t address) { return address >> 12; }

constexpr uint64_t to_address(uint64_t frame, uint64_t offset)
{
    return (frame << 12) | offset;
}

constexpr uint16_t l1_to_index(uint64_t address)
{
    return (address >> 6) & 0x3F;
}

constexpr uint64_t l1_to_tag(uint64_t address) { return address >> 12; }

constexpr uint8_t l1_to_offset(uint64_t address)
{
    return address & LINE_OFFSET_MASK;
}

constexpr uint16_t l2_to_index(uint64_t address)
{
    return (address >> 6) & 0xFFF;
}

constexpr uint64_t l2_to_tag(uint64_t address) { return address >> 18; }

constexpr uint64_t to_line_base(uint64_t address)
{
    return address & ~static_cast<uint64_t>(LINE_OFFSET_MASK);
}

constexpr uint64_t l1_to_addr(uint64_t tag, uint16_t index)
{
    return (tag << 12) | (static_cast<uint64_t>(index) << 6);
}

constexpr uint64_t l2_to_addr(uint64_t tag, uint16_t index)
{
    return (tag << 18) | (static_cast<uint64_t>(index) << 6);
}

#endif // TYPES_H