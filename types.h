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

#endif // TYPES_H