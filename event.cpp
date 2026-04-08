#include "event.h"

#include <cstdio>

EventQueue *g_event_queue = nullptr;

static thread_local uint32_t current_seq = 0;

void event_callback(CacheEventType type, uint8_t core_id, uint8_t level,
                    uint16_t set, uint8_t way, uint64_t addr,
                    uint8_t old_state, uint8_t new_state, const uint8_t *data)
{
    if (!g_event_queue) {
        return;
    }
    g_event_queue->push(type, core_id, level, set, way, addr,
                        static_cast<MESIState>(old_state),
                        static_cast<MESIState>(new_state), data, current_seq);
}

void event_begin_step()
{
    if (!g_event_queue) {
        return;
    }
    current_seq = g_event_queue->begin_step();
}

bool write_trace(const char *path, EventQueue *queue)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }

    TraceHeader hdr{TRACE_MAGIC, TRACE_VERSION,
                    static_cast<uint16_t>(sizeof(TraceRecord))};
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    for (Event *e = queue->first; e; e = e->next) {
        TraceRecord rec{};
        rec.address = e->address;
        rec.seq = e->seq;
        rec.set_index = e->set_index;
        rec.way = e->way;
        rec.core_id = e->core_id;
        rec.type = static_cast<uint8_t>(e->type);
        rec.old_state = static_cast<uint8_t>(e->old_state);
        rec.new_state = static_cast<uint8_t>(e->new_state);
        rec.cache_level = e->cache_level;
        rec.has_data = e->has_data ? 1 : 0;
        if (e->has_data) {
            std::memcpy(rec.data, e->data, LINE_SIZE);
        }
        if (fwrite(&rec, sizeof(rec), 1, f) != 1) {
            fclose(f);
            return false;
        }
    }

    return fclose(f) == 0;
}

EventQueue *read_trace(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return nullptr;
    }

    TraceHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != TRACE_MAGIC) {
        fclose(f);
        return nullptr;
    }
    if (hdr.version != TRACE_VERSION ||
        hdr.record_size != sizeof(TraceRecord)) {
        fclose(f);
        return nullptr;
    }

    auto *queue = new EventQueue();
    TraceRecord rec{};
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        queue->push(static_cast<CacheEventType>(rec.type), rec.core_id,
                    rec.cache_level, rec.set_index, rec.way, rec.address,
                    static_cast<MESIState>(rec.old_state),
                    static_cast<MESIState>(rec.new_state),
                    rec.has_data ? rec.data : nullptr, rec.seq);
    }
    if (ferror(f)) {
        fclose(f);
        delete queue;
        return nullptr;
    }
    queue->next_seq = queue->last ? queue->last->seq + 1 : 0;

    fclose(f);
    return queue;
}
