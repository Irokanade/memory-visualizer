#ifndef CORE_H
#define CORE_H

#include "types.h"
#include "memory.h"
#include <cstdint>


struct PerfCounters {
    uint64_t l1d_hits;
    uint64_t l1i_hits;
    uint64_t l2_hits;
    uint64_t mem_fetches;
};

enum class StreamDirection : int8_t { UNKNOWN = 0, FORWARD = 1, BACKWARD = -1 };
enum class StreamConfidence : uint8_t { INVALID, TRAINING, STEADY };

struct Stream {
    uint64_t last_line;
    uint64_t prefetch_head;
    StreamDirection direction;
    StreamConfidence confidence;
};

constexpr uint8_t NUM_STREAMS = 8;
constexpr uint8_t PREFETCH_DISTANCE = 8;
struct Prefetcher {
    Stream streams[NUM_STREAMS] = {};
    uint8_t lru_age[NUM_STREAMS] = {7, 6, 5, 4, 3, 2, 1, 0};
};

constexpr uint8_t NUM_L1_WAYS = 8;
struct L1Set {
    uint64_t tag[NUM_L1_WAYS];
    MESIState state[NUM_L1_WAYS];
    uint8_t plru_bits;
    uint8_t data[NUM_L1_WAYS][LINE_SIZE];
};

constexpr uint8_t NUM_TLB_WAYS = 4;
struct TLBSet {
    uint64_t virtual_page_num[NUM_TLB_WAYS];
    uint64_t physical_frame[NUM_TLB_WAYS];
    uint8_t plru_bits;
    bool valid[NUM_TLB_WAYS];
};

// Intel Core 2: 64-set 8-way L1D and L1I, 4-way TLBs
// L1 DTLB: 4 sets x 4 ways = 16 entries  (load μTLB, 2-cycle miss penalty)
// L2 DTLB: 64 sets x 4 ways = 256 entries (main DTLB for 4KB pages, 8-cycle miss penalty)
// ITLB:    32 sets x 4 ways = 128 entries
// No unified STLB on Conroe/Merom (added in Sandy Bridge)
constexpr uint8_t L1_SETS      = 64;
constexpr uint8_t L1_DTLB_SETS = 4;
constexpr uint8_t L2_DTLB_SETS = 64;
constexpr uint8_t ITLB_SETS    = 32;
struct Core {
    L1Set        l1d[L1_SETS];
    L1Set        l1i[L1_SETS];
    TLBSet       l1_dtlb[L1_DTLB_SETS];
    TLBSet       l2_dtlb[L2_DTLB_SETS];
    TLBSet       itlb[ITLB_SETS];
    PageTable   *active_pml4;
    Prefetcher   prefetcher;
    PerfCounters pmc;
    uint8_t      core_id;
};

bool tlb_lookup(TLBSet *set, uint64_t virtual_page_num, uint64_t *physical_frame);
void tlb_fill(TLBSet *set, uint64_t virtual_page_num, uint64_t physical_frame);

// Sets *way to the matching non-INVALID way index. Returns false on miss.
bool find_l1_way(const L1Set *set, uint64_t tag, uint8_t *way);
void l1_cache_read_way(L1Set *l1Set, uint8_t way, uint8_t offset, uint8_t *data, uint8_t data_size);
void l1_cache_write_way(L1Set *l1Set, uint8_t way, uint8_t offset, uint8_t *data, uint8_t data_size);
void l1_cache_fill(L1Set *l1Set, uint64_t tag, uint8_t way, uint8_t *line, MESIState state);

void pf_lru_update(uint8_t *pf_lru_age, uint8_t index);
uint8_t pf_lru_evict(uint8_t *pf_lru_age);

#endif // CORE_H
