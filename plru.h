#ifndef PLRU_H
#define PLRU_H

#include <cstdint>

template <typename T, uint8_t WAYS> void plru_update(T *plru_bits, uint8_t way)
{
    uint8_t node = (WAYS - 1) + way;

    while (node > 0) {
        uint8_t parent = (node - 1) / 2;
        T mask = 1 << parent;
        *plru_bits = (*plru_bits & ~mask) | ((node & 1) << parent);
        node = parent;
    }
}

template <typename T, uint8_t WAYS> uint8_t plru_victim(T plru_bits)
{
    uint8_t node = 0;

    while (node < WAYS - 1) {
        uint8_t bit = (plru_bits >> node) & 1;
        node = 2 * node + 1 + bit;
    }
    return node - (WAYS - 1);
}

#endif // PLRU_H
