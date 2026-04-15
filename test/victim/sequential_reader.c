/*
 * sequential_reader.c — Victim workload for Weaponizing Version Locality
 *
 * Represents a benign co-tenant performing sequential, cache-friendly writes.
 * This pattern maintains version locality:
 *   - All 64 CLs per page are written uniformly in order
 *   - m_total_offset reaches 64 → VN_Page rolls over (stays ONE_STEP)
 *   - Each VN_UPDATE costs 1 DRAM read + 1 DRAM write (no transition penalty)
 *
 * When run alone (baseline), victim achieves low, predictable VN latency.
 * When run alongside sparse_writer (attack), victim's VN_UPDATE requests
 * queue behind the attacker's 5× amplified transition traffic on the shared
 * 3.32 GB/s CXL VN bus, causing measurable latency degradation.
 *
 * Usage:
 *   ../../run-sniper -n 32 -c zen4_vn --roi -d victim_output -- ./sequential_reader
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "sim_api.h"

#define PAGE_SIZE     4096
#define CL_SIZE       64
#define NUM_PAGES     4096              /* 16 MB working set */
#define BUF_SIZE      ((size_t)NUM_PAGES * PAGE_SIZE)
#define NUM_PASSES    10

static volatile uint8_t victim_buf[BUF_SIZE];

int main(void)
{
    /* Fault in all pages */
    for (size_t i = 0; i < BUF_SIZE; i += PAGE_SIZE)
        victim_buf[i] = 0;

    SimRoiStart();

    /*
     * Sequential write pattern: every cacheline of every page, in order.
     * This maintains perfect version locality — pages stay in ONE_STEP
     * and benefit from Toleo's 240x compression.
     */
    for (int pass = 0; pass < NUM_PASSES; pass++) {
        for (size_t off = 0; off < BUF_SIZE; off += CL_SIZE) {
            victim_buf[off] = (uint8_t)(pass ^ (off & 0xFF));
        }
    }

    SimRoiEnd();

    printf("Victim workload complete.\n");
    printf("Key metrics in sim.out:\n");
    printf("  cxl[i].total-effective-read-latency — victim read latency\n");
    printf("  vv[0].one-step-to-vault             — should be ~0 for sequential writes\n");
    return 0;
}
