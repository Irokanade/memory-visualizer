#ifndef RENDERER_H
#define RENDERER_H

#include "core.h"
#include "event.h"
#include "types.h"
#include "uncore.h"

#include <SDL3/SDL.h>

struct ShadowL1 {
    uint64_t tag[NUM_L1_WAYS];
    MESIState state[NUM_L1_WAYS];
    uint8_t data[NUM_L1_WAYS][LINE_SIZE];
};

struct ShadowL2 {
    uint64_t tag[NUM_L2_WAYS];
    MESIState state[NUM_L2_WAYS];
    uint8_t data[NUM_L2_WAYS][LINE_SIZE];
};

struct ShadowCounters {
    uint64_t l1d_hits = 0;
    uint64_t l2_hits = 0;
    uint64_t mem_fetches = 0;
    uint64_t snoops = 0;
    uint64_t prefetches = 0;
};

struct ShadowState {
    ShadowL1 l1d[NUM_CORES][L1_SETS];
    ShadowL2 l2[L2_SETS];
    uint8_t flash[L2_SETS][NUM_L2_WAYS];
    MESIState l2_dominant[L2_SETS];
    ShadowCounters counters[NUM_CORES];
};

struct Renderer {
    SDL_Window *window;
    SDL_Renderer *sdl;
    EventQueue *queue;
    Event *cursor;
    ShadowState shadow;
    uint32_t current_seq;
    bool playing;
    int speed;
    uint8_t *phys_mem;
    uint64_t phys_size;
};

void renderer_init(Renderer *r, EventQueue *queue, uint8_t *phys_mem,
                   uint64_t phys_size);
void renderer_run(Renderer *r);
void renderer_destroy(Renderer *r);

void step_forward(Renderer *r);
void step_backward(Renderer *r);

#endif // RENDERER_H
