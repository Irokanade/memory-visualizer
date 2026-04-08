#include "renderer.h"

#include <cstdio>
#include <cstring>

static constexpr int WIN_W = 1280;
static constexpr int WIN_H = 900;

static constexpr int L1_CELL_W = 12;
static constexpr int L1_CELL_H = 5;
static constexpr int L1_GRID_X = 20;
static constexpr int L1_GRID_Y = 50;

static constexpr int DETAIL_X = 20;
static constexpr int DETAIL_Y = 390;
static constexpr int DETAIL_BYTE_W = 18;
static constexpr int DETAIL_BYTE_H = 12;

static constexpr int L2_CELL = 3;
static constexpr int L2_GRID_X = 20;
static constexpr int L2_GRID_Y = 510;

static constexpr int MEM_X = 280;
static constexpr int MEM_Y = 510;
static constexpr int MEM_LINES = 16;
static constexpr int MEM_BYTES_PER_LINE = 16;

static constexpr int LOG_X = 700;
static constexpr int LOG_Y = 50;
static constexpr int LOG_W = 560;
static constexpr int LOG_LINE_H = 12;
static constexpr int LOG_VISIBLE = 50;

struct Color {
    uint8_t r, g, b, a;
};

static constexpr Color MESI_COLORS[] = {
    {80, 80, 80, 255},
    {220, 50, 50, 255},
    {50, 100, 220, 255},
    {50, 180, 80, 255},
};
static_assert(static_cast<uint8_t>(MESIState::INVALID) == 0);
static_assert(static_cast<uint8_t>(MESIState::MODIFIED) == 1);
static_assert(static_cast<uint8_t>(MESIState::EXCLUSIVE) == 2);
static_assert(static_cast<uint8_t>(MESIState::SHARED) == 3);

static constexpr Color COLOR_PREFETCH_FLASH = {255, 220, 50, 180};
static constexpr Color COLOR_ACTIVE_BORDER = {255, 255, 255, 255};
static constexpr Color COLOR_BG = {30, 30, 30, 255};
static constexpr Color COLOR_TEXT = {200, 200, 200, 255};
static constexpr Color COLOR_HEADER = {255, 255, 255, 255};

static void set_color(SDL_Renderer *r, Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void update_counters_forward(ShadowState *s, Event *e)
{
    if (e->core_id >= NUM_CORES) {
        return;
    }
    ShadowCounters *c = &s->counters[e->core_id];
    switch (e->type) {
    case CACHE_L1D_HIT:
        c->l1d_hits++;
        break;
    case CACHE_L2_HIT:
        c->l2_hits++;
        break;
    case CACHE_MEM_FETCH:
        c->mem_fetches++;
        break;
    case CACHE_SNOOP_DOWNGRADE:
    case CACHE_SNOOP_INVALIDATE:
        c->snoops++;
        break;
    case CACHE_PREFETCH_L2:
        c->prefetches++;
        break;
    default:
        break;
    }
}

static void update_counters_backward(ShadowState *s, Event *e)
{
    if (e->core_id >= NUM_CORES) {
        return;
    }
    ShadowCounters *c = &s->counters[e->core_id];
    switch (e->type) {
    case CACHE_L1D_HIT:
        c->l1d_hits--;
        break;
    case CACHE_L2_HIT:
        c->l2_hits--;
        break;
    case CACHE_MEM_FETCH:
        c->mem_fetches--;
        break;
    case CACHE_SNOOP_DOWNGRADE:
    case CACHE_SNOOP_INVALIDATE:
        c->snoops--;
        break;
    case CACHE_PREFETCH_L2:
        c->prefetches--;
        break;
    default:
        break;
    }
}

static uint8_t mesi_priority(MESIState s)
{
    switch (s) {
    case MESIState::MODIFIED:
        return 3;
    case MESIState::EXCLUSIVE:
        return 2;
    case MESIState::SHARED:
        return 1;
    case MESIState::INVALID:
        return 0;
    }
    return 0;
}

static void recompute_l2_dominant(ShadowState *s, uint16_t set_index)
{
    ShadowL2 *set = &s->l2[set_index];
    MESIState dom = MESIState::INVALID;
    uint8_t best = 0;
    for (uint8_t w = 0; w < NUM_L2_WAYS; w++) {
        uint8_t p = mesi_priority(set->state[w]);
        if (p > best) {
            best = p;
            dom = set->state[w];
        }
    }
    s->l2_dominant[set_index] = dom;
}

static void apply_event_forward(ShadowState *s, Event *e)
{
    update_counters_forward(s, e);

    if (e->cache_level == 1) {
        ShadowL1 *set = &s->l1d[e->core_id][e->set_index];
        switch (e->type) {
        case CACHE_L1D_FILL:
            set->tag[e->way] = e->address;
            set->state[e->way] = e->new_state;
            if (e->has_data) {
                std::memcpy(set->data[e->way], e->data, LINE_SIZE);
            }
            break;
        case CACHE_L1D_EVICT:
        case CACHE_SNOOP_INVALIDATE:
            set->state[e->way] = MESIState::INVALID;
            break;
        case CACHE_L1D_HIT:
        case CACHE_SNOOP_DOWNGRADE:
            set->state[e->way] = e->new_state;
            if (e->has_data) {
                std::memcpy(set->data[e->way], e->data, LINE_SIZE);
            }
            break;
        default:
            break;
        }
    } else if (e->cache_level == 2) {
        ShadowL2 *set = &s->l2[e->set_index];
        switch (e->type) {
        case CACHE_L2_FILL:
        case CACHE_PREFETCH_L2:
            set->tag[e->way] = e->address;
            set->state[e->way] = e->new_state;
            if (e->has_data) {
                std::memcpy(set->data[e->way], e->data, LINE_SIZE);
            }
            if (e->type == CACHE_PREFETCH_L2) {
                s->flash[e->set_index][e->way] = 10;
            }
            break;
        case CACHE_L2_EVICT:
            set->state[e->way] = MESIState::INVALID;
            break;
        case CACHE_L2_HIT:
            set->state[e->way] = e->new_state;
            break;
        default:
            break;
        }
        recompute_l2_dominant(s, e->set_index);
    }
}

static void apply_event_backward(ShadowState *s, Event *e)
{
    update_counters_backward(s, e);

    if (e->cache_level == 1) {
        ShadowL1 *set = &s->l1d[e->core_id][e->set_index];
        switch (e->type) {
        case CACHE_L1D_FILL:
            set->state[e->way] = MESIState::INVALID;
            break;
        case CACHE_L1D_EVICT:
            set->state[e->way] = e->old_state;
            if (e->has_data) {
                set->tag[e->way] = e->address;
                std::memcpy(set->data[e->way], e->data, LINE_SIZE);
            }
            break;
        case CACHE_SNOOP_INVALIDATE:
        case CACHE_SNOOP_DOWNGRADE:
        case CACHE_L1D_HIT:
            set->state[e->way] = e->old_state;
            break;
        default:
            break;
        }
    } else if (e->cache_level == 2) {
        ShadowL2 *set = &s->l2[e->set_index];
        switch (e->type) {
        case CACHE_L2_FILL:
        case CACHE_PREFETCH_L2:
            set->state[e->way] = MESIState::INVALID;
            break;
        case CACHE_L2_EVICT:
            set->state[e->way] = e->old_state;
            if (e->has_data) {
                set->tag[e->way] = e->address;
                std::memcpy(set->data[e->way], e->data, LINE_SIZE);
            }
            break;
        case CACHE_L2_HIT:
            set->state[e->way] = e->old_state;
            break;
        default:
            break;
        }
        recompute_l2_dominant(s, e->set_index);
    }
}

void step_forward(Renderer *r)
{
    if (!r->cursor) {
        if (!r->queue->first) {
            return;
        }
        r->cursor = r->queue->first;
        apply_event_forward(&r->shadow, r->cursor);
        uint32_t seq = r->cursor->seq;
        while (r->cursor->next && r->cursor->next->seq == seq) {
            r->cursor = r->cursor->next;
            apply_event_forward(&r->shadow, r->cursor);
        }
        r->current_seq = seq;
        return;
    }

    if (!r->cursor->next) {
        return;
    }

    uint32_t next_seq = r->cursor->next->seq;
    while (r->cursor->next && r->cursor->next->seq == next_seq) {
        r->cursor = r->cursor->next;
        apply_event_forward(&r->shadow, r->cursor);
    }
    r->current_seq = next_seq;
}

void step_backward(Renderer *r)
{
    if (!r->cursor) {
        return;
    }

    uint32_t cur_seq = r->cursor->seq;
    while (r->cursor && r->cursor->seq == cur_seq) {
        apply_event_backward(&r->shadow, r->cursor);
        r->cursor = r->cursor->prev;
    }

    if (r->cursor) {
        r->current_seq = r->cursor->seq;
    } else {
        r->current_seq = 0;
    }
}

static void draw_l1d(Renderer *r, uint8_t core_id, int base_x, int base_y)
{
    char label[32];
    snprintf(label, sizeof(label), "L1D Core %u", core_id);
    set_color(r->sdl, COLOR_HEADER);
    SDL_RenderDebugText(r->sdl, static_cast<float>(base_x),
                        static_cast<float>(base_y - 14), label);

    Event *cur = r->cursor;

    for (int s = 0; s < L1_SETS; s++) {
        for (int w = 0; w < NUM_L1_WAYS; w++) {
            MESIState st = r->shadow.l1d[core_id][s].state[w];
            float x = static_cast<float>(base_x + w * L1_CELL_W);
            float y = static_cast<float>(base_y + s * L1_CELL_H);
            SDL_FRect rect = {x, y, static_cast<float>(L1_CELL_W - 1),
                              static_cast<float>(L1_CELL_H - 1)};

            set_color(r->sdl, MESI_COLORS[static_cast<uint8_t>(st)]);
            SDL_RenderFillRect(r->sdl, &rect);

            if (cur && cur->cache_level == 1 && cur->core_id == core_id &&
                cur->set_index == s && cur->way == w) {
                set_color(r->sdl, COLOR_ACTIVE_BORDER);
                SDL_RenderRect(r->sdl, &rect);
            }
        }
    }
}

static void draw_l2_overview(Renderer *r, int base_x, int base_y)
{
    set_color(r->sdl, COLOR_HEADER);
    SDL_RenderDebugText(r->sdl, static_cast<float>(base_x),
                        static_cast<float>(base_y - 14), "L2 Cache (shared)");

    for (int s = 0; s < L2_SETS; s++) {
        int col = s / 64;
        int row = s % 64;

        float x = static_cast<float>(base_x + col * L2_CELL);
        float y = static_cast<float>(base_y + row * L2_CELL);
        SDL_FRect rect = {x, y, static_cast<float>(L2_CELL - 1),
                          static_cast<float>(L2_CELL - 1)};

        set_color(r->sdl,
                  MESI_COLORS[static_cast<uint8_t>(r->shadow.l2_dominant[s])]);
        SDL_RenderFillRect(r->sdl, &rect);

        for (int w = 0; w < NUM_L2_WAYS; w++) {
            if (r->shadow.flash[s][w] > 0) {
                set_color(r->sdl, COLOR_PREFETCH_FLASH);
                SDL_RenderFillRect(r->sdl, &rect);
                break;
            }
        }
    }
}

static const char *event_type_name(CacheEventType type)
{
    switch (type) {
    case CACHE_L1D_HIT:
        return "L1D HIT";
    case CACHE_L1D_FILL:
        return "L1D FILL";
    case CACHE_L1D_EVICT:
        return "L1D EVICT";
    case CACHE_L2_HIT:
        return "L2 HIT";
    case CACHE_L2_FILL:
        return "L2 FILL";
    case CACHE_L2_EVICT:
        return "L2 EVICT";
    case CACHE_SNOOP_DOWNGRADE:
        return "SNOOP DN";
    case CACHE_SNOOP_INVALIDATE:
        return "SNOOP INV";
    case CACHE_PREFETCH_L2:
        return "PREFETCH";
    case CACHE_MEM_FETCH:
        return "MEM FETCH";
    case CACHE_L1I_SNOOP_INVALIDATE:
        return "L1I INV";
    }
    return "???";
}

static const char *mesi_name(MESIState s)
{
    switch (s) {
    case MESIState::INVALID:
        return "I";
    case MESIState::MODIFIED:
        return "M";
    case MESIState::EXCLUSIVE:
        return "E";
    case MESIState::SHARED:
        return "S";
    }
    return "?";
}

static void draw_event_log(Renderer *r)
{
    set_color(r->sdl, COLOR_HEADER);
    SDL_RenderDebugText(r->sdl, static_cast<float>(LOG_X),
                        static_cast<float>(LOG_Y - 14), "Event Log");

    if (!r->cursor) {
        return;
    }

    Event *e = r->cursor;
    int lines_back = LOG_VISIBLE / 2;
    for (int i = 0; i < lines_back && e->prev; i++) {
        e = e->prev;
    }

    int y = LOG_Y;
    for (int i = 0; i < LOG_VISIBLE && e; i++, e = e->next) {
        char line[128];
        snprintf(line, sizeof(line), "#%-5u %-10s c%u s%-4u w%u %s->%s 0x%llx",
                 e->seq, event_type_name(e->type), e->core_id, e->set_index,
                 e->way, mesi_name(e->old_state), mesi_name(e->new_state),
                 (unsigned long long)e->address);

        if (e == r->cursor) {
            SDL_FRect highlight = {
                static_cast<float>(LOG_X - 2), static_cast<float>(y - 1),
                static_cast<float>(LOG_W), static_cast<float>(LOG_LINE_H)};
            set_color(r->sdl, {60, 60, 80, 255});
            SDL_RenderFillRect(r->sdl, &highlight);
            set_color(r->sdl, COLOR_ACTIVE_BORDER);
        } else {
            set_color(r->sdl, COLOR_TEXT);
        }

        SDL_RenderDebugText(r->sdl, static_cast<float>(LOG_X),
                            static_cast<float>(y), line);
        y += LOG_LINE_H;
    }
}

static void draw_memory(Renderer *r)
{
    if (!r->phys_mem) {
        return;
    }
    set_color(r->sdl, COLOR_HEADER);
    SDL_RenderDebugText(r->sdl, static_cast<float>(MEM_X),
                        static_cast<float>(MEM_Y - 14), "Memory");

    uint64_t center = 0;
    if (r->cursor && r->cursor->address > 0) {
        center = r->cursor->address & ~static_cast<uint64_t>(0xF);
    }

    uint64_t start = 0;
    if (center >= static_cast<uint64_t>(MEM_LINES / 2) * MEM_BYTES_PER_LINE) {
        start =
            center - static_cast<uint64_t>(MEM_LINES / 2) * MEM_BYTES_PER_LINE;
    }

    uint64_t active_line =
        r->cursor ? (r->cursor->address & ~static_cast<uint64_t>(LINE_SIZE - 1))
                  : UINT64_MAX;

    for (int row = 0; row < MEM_LINES; row++) {
        uint64_t addr = start + static_cast<uint64_t>(row) * MEM_BYTES_PER_LINE;
        if (addr + MEM_BYTES_PER_LINE > r->phys_size) {
            break;
        }

        bool is_active =
            (addr >= active_line && addr < active_line + LINE_SIZE);
        if (is_active) {
            SDL_FRect bg = {static_cast<float>(MEM_X - 2),
                            static_cast<float>(MEM_Y + row * LOG_LINE_H - 1),
                            340.0f, static_cast<float>(LOG_LINE_H)};
            set_color(r->sdl, {50, 50, 70, 255});
            SDL_RenderFillRect(r->sdl, &bg);
        }

        char line[128];
        int pos =
            snprintf(line, sizeof(line), "%06llx: ", (unsigned long long)addr);
        for (int b = 0; b < MEM_BYTES_PER_LINE && pos < 120; b++) {
            pos +=
                snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos),
                         "%02x ", r->phys_mem[addr + static_cast<uint64_t>(b)]);
        }

        set_color(r->sdl, is_active ? COLOR_ACTIVE_BORDER : COLOR_TEXT);
        SDL_RenderDebugText(r->sdl, static_cast<float>(MEM_X),
                            static_cast<float>(MEM_Y + row * LOG_LINE_H), line);
    }
}

static void draw_core_cacheline(Renderer *r, uint8_t core_id, uint16_t l1_set,
                                int base_x, int base_y, int highlight_offset,
                                bool is_active)
{
    ShadowL1 *set = &r->shadow.l1d[core_id][l1_set];
    uint8_t way = 0;
    MESIState state = MESIState::INVALID;
    const uint8_t *line_data = nullptr;

    if (is_active && r->cursor) {
        way = r->cursor->way;
        state = set->state[way];
        line_data = (state != MESIState::INVALID) ? set->data[way] : nullptr;
    } else {
        for (uint8_t w = 0; w < NUM_L1_WAYS; w++) {
            if (set->state[w] != MESIState::INVALID) {
                way = w;
                state = set->state[w];
                line_data = set->data[w];
                break;
            }
        }
    }

    char header[128];
    snprintf(header, sizeof(header), "Core %u L1D  set %u way %u  [%s]",
             core_id, l1_set, way, mesi_name(state));

    Color hdr_color = is_active ? COLOR_ACTIVE_BORDER : COLOR_HEADER;
    set_color(r->sdl, hdr_color);
    SDL_RenderDebugText(r->sdl, static_cast<float>(base_x),
                        static_cast<float>(base_y - 14), header);

    if (!line_data) {
        set_color(r->sdl, COLOR_TEXT);
        SDL_RenderDebugText(r->sdl, static_cast<float>(base_x),
                            static_cast<float>(base_y), "(empty)");
        return;
    }

    for (int row = 0; row < 2; row++) {
        char off_label[8];
        snprintf(off_label, sizeof(off_label), "+%02x:", row * 32);
        set_color(r->sdl, COLOR_TEXT);
        SDL_RenderDebugText(
            r->sdl, static_cast<float>(base_x),
            static_cast<float>(base_y + row * (DETAIL_BYTE_H + 4)), off_label);

        for (int b = 0; b < 32; b++) {
            int byte_idx = row * 32 + b;
            float x = static_cast<float>(base_x + 40 + b * DETAIL_BYTE_W);
            float y = static_cast<float>(base_y + row * (DETAIL_BYTE_H + 4));

            bool is_accessed = is_active && (byte_idx >= highlight_offset &&
                                             byte_idx < highlight_offset + 4);
            if (is_accessed) {
                SDL_FRect bg = {x - 1, y - 1, static_cast<float>(DETAIL_BYTE_W),
                                static_cast<float>(DETAIL_BYTE_H)};
                set_color(r->sdl, {100, 60, 20, 255});
                SDL_RenderFillRect(r->sdl, &bg);
                set_color(r->sdl, {255, 200, 50, 255});
            } else {
                set_color(r->sdl, COLOR_TEXT);
            }

            char hex[4];
            snprintf(hex, sizeof(hex), "%02x", line_data[byte_idx]);
            SDL_RenderDebugText(r->sdl, x, y, hex);
        }
    }
}

static void draw_cacheline_detail(Renderer *r)
{
    Event *cur = r->cursor;
    if (!cur || cur->cache_level != 1) {
        return;
    }

    uint8_t offset = static_cast<uint8_t>(cur->address & 0x3F);
    uint16_t l1_set = cur->set_index;

    draw_core_cacheline(r, 0, l1_set, DETAIL_X, DETAIL_Y, offset,
                        cur->core_id == 0);
    draw_core_cacheline(r, 1, l1_set, DETAIL_X, DETAIL_Y + 50, offset,
                        cur->core_id == 1);
}

static void draw_stats(Renderer *r)
{
    int x = 350;
    int y = 50;

    set_color(r->sdl, COLOR_HEADER);
    SDL_RenderDebugText(r->sdl, static_cast<float>(x), static_cast<float>(y),
                        "Stats");

    for (uint8_t c = 0; c < NUM_CORES; c++) {
        ShadowCounters *sc = &r->shadow.counters[c];
        uint64_t total = sc->l1d_hits + sc->l2_hits + sc->mem_fetches;
        int cy = y + 16 + c * 80;

        char hdr[32];
        snprintf(hdr, sizeof(hdr), "Core %u", c);
        set_color(r->sdl, COLOR_HEADER);
        SDL_RenderDebugText(r->sdl, static_cast<float>(x),
                            static_cast<float>(cy), hdr);

        char lines[5][64];
        snprintf(lines[0], 64, "L1D hits:    %llu",
                 (unsigned long long)sc->l1d_hits);
        snprintf(lines[1], 64, "L2 hits:     %llu",
                 (unsigned long long)sc->l2_hits);
        snprintf(lines[2], 64, "Mem fetches: %llu",
                 (unsigned long long)sc->mem_fetches);
        snprintf(lines[3], 64, "Snoops:      %llu",
                 (unsigned long long)sc->snoops);
        snprintf(lines[4], 64, "Prefetches:  %llu",
                 (unsigned long long)sc->prefetches);

        set_color(r->sdl, COLOR_TEXT);
        for (int i = 0; i < 5; i++) {
            SDL_RenderDebugText(r->sdl, static_cast<float>(x),
                                static_cast<float>(cy + 12 + i * 12), lines[i]);
        }

        if (total > 0) {
            char rate[64];
            snprintf(rate, 64, "L1D rate:    %.1f%%",
                     100.0 * static_cast<double>(sc->l1d_hits) /
                         static_cast<double>(total));
            set_color(r->sdl, {255, 200, 50, 255});
            SDL_RenderDebugText(r->sdl, static_cast<float>(x),
                                static_cast<float>(cy + 12 + 5 * 12), rate);
        }
    }
}

static void draw_controls(Renderer *r)
{
    bool at_end = r->cursor && !r->cursor->next;
    const char *state =
        at_end ? "[FINISHED]" : (r->playing ? "[PLAYING]" : "[PAUSED]");
    char status[128];
    uint32_t max_seq = r->queue->last ? r->queue->last->seq : 0;
    snprintf(status, sizeof(status), "%s  Step %u / %u  Speed: %dx", state,
             r->current_seq, max_seq, r->speed);

    set_color(r->sdl, COLOR_HEADER);
    SDL_RenderDebugText(r->sdl, 20.0f, 10.0f, status);

    set_color(r->sdl, COLOR_TEXT);
    SDL_RenderDebugText(r->sdl, 20.0f, 24.0f,
                        "SPACE=play/pause  LEFT/RIGHT=step  UP/DOWN=speed  "
                        "ESC=quit");

    float lx = 500.0f;
    float ly = 10.0f;
    constexpr float SWATCH = 10.0f;
    constexpr float GAP = 70.0f;

    SDL_FRect sw;
    sw = {lx, ly, SWATCH, SWATCH};
    set_color(r->sdl, MESI_COLORS[static_cast<uint8_t>(MESIState::MODIFIED)]);
    SDL_RenderFillRect(r->sdl, &sw);
    set_color(r->sdl, COLOR_TEXT);
    SDL_RenderDebugText(r->sdl, lx + 14, ly + 1, "M");

    sw = {lx + GAP, ly, SWATCH, SWATCH};
    set_color(r->sdl, MESI_COLORS[static_cast<uint8_t>(MESIState::EXCLUSIVE)]);
    SDL_RenderFillRect(r->sdl, &sw);
    set_color(r->sdl, COLOR_TEXT);
    SDL_RenderDebugText(r->sdl, lx + GAP + 14, ly + 1, "E");

    sw = {lx + GAP * 2, ly, SWATCH, SWATCH};
    set_color(r->sdl, MESI_COLORS[static_cast<uint8_t>(MESIState::SHARED)]);
    SDL_RenderFillRect(r->sdl, &sw);
    set_color(r->sdl, COLOR_TEXT);
    SDL_RenderDebugText(r->sdl, lx + GAP * 2 + 14, ly + 1, "S");

    sw = {lx + GAP * 3, ly, SWATCH, SWATCH};
    set_color(r->sdl, MESI_COLORS[static_cast<uint8_t>(MESIState::INVALID)]);
    SDL_RenderFillRect(r->sdl, &sw);
    set_color(r->sdl, COLOR_TEXT);
    SDL_RenderDebugText(r->sdl, lx + GAP * 3 + 14, ly + 1, "I");
}

void renderer_init(Renderer *r, EventQueue *queue, uint8_t *phys_mem,
                   uint64_t phys_size)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        std::abort();
    }
    r->window = SDL_CreateWindow("Cache Visualizer", WIN_W, WIN_H,
                                 SDL_WINDOW_RESIZABLE);
    if (!r->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        std::abort();
    }
    r->sdl = SDL_CreateRenderer(r->window, nullptr);
    if (!r->sdl) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        std::abort();
    }
    if (!SDL_SetRenderVSync(r->sdl, 1)) {
        SDL_Log("VSync not available: %s", SDL_GetError());
    }
    r->queue = queue;
    r->cursor = nullptr;
    r->current_seq = 0;
    r->playing = false;
    r->speed = 1;
    r->phys_mem = phys_mem;
    r->phys_size = phys_size;
    r->shadow = ShadowState{};
}

void renderer_run(Renderer *r)
{
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_SPACE:
                    r->playing = !r->playing;
                    break;
                case SDLK_RIGHT:
                    step_forward(r);
                    break;
                case SDLK_LEFT:
                    step_backward(r);
                    break;
                case SDLK_UP:
                    if (r->speed < 64) {
                        r->speed *= 2;
                    }
                    break;
                case SDLK_DOWN:
                    if (r->speed > 1) {
                        r->speed /= 2;
                    }
                    break;
                default:
                    break;
                }
                break;
            default:
                break;
            }
        }

        if (r->playing) {
            for (int i = 0; i < r->speed; i++) {
                if (!r->cursor || r->cursor->next) {
                    step_forward(r);
                } else {
                    r->playing = false;
                    break;
                }
            }
        }

        for (int s = 0; s < L2_SETS; s++) {
            for (int w = 0; w < NUM_L2_WAYS; w++) {
                if (r->shadow.flash[s][w] > 0) {
                    r->shadow.flash[s][w]--;
                }
            }
        }

        set_color(r->sdl, COLOR_BG);
        SDL_RenderClear(r->sdl);

        draw_controls(r);
        draw_l1d(r, 0, L1_GRID_X, L1_GRID_Y);
        draw_l1d(r, 1, L1_GRID_X + NUM_L1_WAYS * L1_CELL_W + 40, L1_GRID_Y);
        draw_stats(r);
        draw_cacheline_detail(r);
        draw_l2_overview(r, L2_GRID_X, L2_GRID_Y);
        draw_memory(r);
        draw_event_log(r);

        SDL_RenderPresent(r->sdl);
    }
}

void renderer_destroy(Renderer *r)
{
    SDL_DestroyRenderer(r->sdl);
    SDL_DestroyWindow(r->window);
    SDL_Quit();
}
