#ifndef EVENT_H
#define EVENT_H

#include "cpu_callback.h"
#include "types.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

struct Event {
    Event *prev;
    Event *next;
    uint64_t address;
    uint32_t seq;
    uint16_t set_index;
    uint8_t way;
    uint8_t core_id;
    CacheEventType type;
    MESIState old_state;
    MESIState new_state;
    uint8_t cache_level;
    bool has_data;
    uint8_t data[LINE_SIZE];
};

struct EventPool {
    static constexpr size_t BLOCK_SIZE = 4 * 1024 * 1024;

    struct Block {
        uint8_t *data;
        Block *next;
    };

    Block *head = nullptr;
    size_t offset = 0;

    void *alloc(size_t size)
    {
        if (size > BLOCK_SIZE) {
            std::abort();
        }
        offset = (offset + alignof(Event) - 1) & ~(alignof(Event) - 1);
        if (!head || offset + size > BLOCK_SIZE) {
            auto *b = new Block;
            b->data = static_cast<uint8_t *>(std::malloc(BLOCK_SIZE));
            if (!b->data) {
                std::abort();
            }
            b->next = head;
            head = b;
            offset = 0;
        }
        void *ptr = &head->data[offset];
        offset += size;
        return ptr;
    }

    ~EventPool()
    {
        Block *b = head;
        while (b) {
            Block *next = b->next;
            std::free(b->data);
            delete b;
            b = next;
        }
    }
};

struct EventQueue {
    EventPool pool;
    Event *first = nullptr;
    Event *last = nullptr;
    uint32_t next_seq = 0;
    uint64_t count = 0;
    std::mutex lock;

    uint32_t begin_step()
    {
        std::lock_guard<std::mutex> lk(lock);
        return next_seq++;
    }

    Event *push(CacheEventType type, uint8_t core_id, uint8_t level,
                uint16_t set, uint8_t way, uint64_t addr,
                MESIState old_state, MESIState new_state,
                const uint8_t *line_data, uint32_t seq)
    {
        std::lock_guard<std::mutex> lk(lock);
        auto *e = static_cast<Event *>(pool.alloc(sizeof(Event)));
        e->prev = last;
        e->next = nullptr;
        e->address = addr;
        e->seq = seq;
        e->set_index = set;
        e->way = way;
        e->core_id = core_id;
        e->type = type;
        e->old_state = old_state;
        e->new_state = new_state;
        e->cache_level = level;

        if (line_data) {
            e->has_data = true;
            std::memcpy(e->data, line_data, LINE_SIZE);
        } else {
            e->has_data = false;
        }

        if (last) {
            last->next = e;
        } else {
            first = e;
        }
        last = e;
        count++;
        return e;
    }
};

extern EventQueue *g_event_queue;

void event_callback(CacheEventType type, uint8_t core_id, uint8_t level,
                    uint16_t set, uint8_t way, uint64_t addr,
                    uint8_t old_state, uint8_t new_state,
                    const uint8_t *data);

void event_begin_step();

static constexpr uint32_t TRACE_MAGIC = 0x4D565452;
static constexpr uint16_t TRACE_VERSION = 1;

struct TraceHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
};

struct TraceRecord {
    uint64_t address;
    uint32_t seq;
    uint16_t set_index;
    uint8_t way;
    uint8_t core_id;
    uint8_t type;
    uint8_t old_state;
    uint8_t new_state;
    uint8_t cache_level;
    uint8_t has_data;
    uint8_t pad[6];
    uint8_t data[LINE_SIZE];
};
static_assert(sizeof(TraceRecord) == 96, "TraceRecord size changed");

bool write_trace(const char *path, EventQueue *queue);
EventQueue *read_trace(const char *path);

#endif // EVENT_H
