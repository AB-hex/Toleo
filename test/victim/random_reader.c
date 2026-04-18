/*
 * random_reader.c — CXL-sensitive victim workload for Weaponizing Version Locality
 *
 * Represents a benign co-tenant performing random reads over a large working set
 * (64 MB, 4× L3 cache). This workload is CXL-bound:
 *   - Working set exceeds L3 → most reads go to CXL memory
 *   - Random access pattern defeats prefetching and spatial locality
 *   - READS (not writes) stall the pipeline — each miss blocks dependent instructions
 *   - Still maintains version locality (reads don't cause VN transitions)
 *
 * When run alone, victim achieves moderate IPC limited by CXL latency.
 * When run alongside sparse_writer, the attacker's amplified VN traffic
 * contends on the shared CXL bus, increasing read latency and reducing IPC.
 *
 * Usage:
 *   ../../run-sniper -n 32 -c zen4_vn --roi -d victim_output -- ./random_reader
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "sim_api.h"

#define PAGE_SIZE     4096
#define CL_SIZE       64
#define NUM_PAGES     16384             /* 64 MB working set — 4x L3 */
#define BUF_SIZE      ((size_t)NUM_PAGES * PAGE_SIZE)
#define NUM_ACCESSES  (50 * 1024)        /* 50K random reads — fast simulation */

static volatile uint8_t victim_buf[BUF_SIZE];

/*
 * Simple xorshift32 PRNG — fast, no library dependency, deterministic.
 * Generates pseudo-random indices into the buffer.
 */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void)
{
    uint32_t rng_state = 0xDEADBEEF;
    volatile uint8_t sink = 0;

    /* Fault in all pages and fill with data */
    for (size_t i = 0; i < BUF_SIZE; i += CL_SIZE)
        victim_buf[i] = (uint8_t)(i & 0xFF);

    SimRoiStart();

    /*
     * Random read pattern: each access reads a random cacheline from the
     * 64 MB buffer. With 16-way 16 MB L3, ~75% of accesses miss in L3
     * and go to CXL memory. Each CXL read stalls the core until data
     * arrives, making IPC directly sensitive to CXL bus latency.
     *
     * The read result feeds into the next iteration (dependent load chain)
     * to prevent the OoO engine from hiding all latency.
     */
    for (int i = 0; i < NUM_ACCESSES; i++) {
        uint32_t r = xorshift32(&rng_state);
        /* Align to cacheline boundary within buffer */
        size_t offset = ((size_t)r % (BUF_SIZE / CL_SIZE)) * CL_SIZE;
        sink += victim_buf[offset];
        /* Feed back to create dependent chain (prevents full MLP hiding) */
        rng_state ^= sink;
    }

    SimRoiEnd();

    /* Prevent sink from being optimized away */
    printf("Victim workload complete (sink=%u).\n", (unsigned)sink);
    printf("Key metrics in sim.out:\n");
    printf("  cxl[i].total-effective-read-latency — victim read latency\n");
    printf("  IPC — should degrade under CXL bus contention\n");
    return 0;
}
