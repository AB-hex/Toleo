/*
 * sparse_writer.c — Attacker workload for Weaponizing Version Locality
 *
 * Attack mechanism (corrected for the simulator's abstraction):
 *
 * The simulator resets VN_Page to ONE_STEP after every state transition
 * (vv_perf_model_1step.cc:168), so OVERFLOW is unreachable. The actual
 * amplification comes from frequent ONE_STEP -> VAULT transitions:
 *
 *   - Each transition triggers an 8-iteration DRAM write penalty
 *     (128 bytes / 16 bytes per entry = 8 VN DRAM writes)
 *   - Normal ONE_STEP access costs 1 read + 1 write = 2 DRAM ops
 *   - Transition access costs 1 read + 1 write + 8 transition writes = 10 ops
 *   - Amplification: 5x per transition on the shared 3.32 GB/s CXL VN bus
 *
 * To trigger a transition, the same cacheline must receive 2 VN_UPDATEs
 * (vn_private > 1). We achieve this by repeatedly writing CL 0 of every
 * page. With 16,384 pages at stride 4096, only 256 of 16,384 L3 sets
 * are used (stride-64 in set space), causing ~75% conflict evictions
 * and ensuring CL 0 is written back to CXL between passes.
 *
 * Every 2 passes, all evicted pages trigger ONE_STEP -> VAULT transitions.
 *
 * Usage:
 *   ../../run-sniper -n 32 -c zen4_vn --roi -d attack_output -- ./sparse_writer
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "sim_api.h"

#define PAGE_SIZE        4096
#define CL_SIZE          64
#define NUM_PAGES        16384         /* 64 MB buffer, ~75% L3 conflict eviction rate */
#define BUF_SIZE         ((size_t)NUM_PAGES * PAGE_SIZE)
#define WARMUP_PASSES    4             /* fill L3, establish eviction steady-state */
#define ATTACK_PASSES    20            /* measured phase: each pair of passes = ~1 transition/page */

static volatile uint8_t attack_buf[BUF_SIZE];

int main(void)
{
    /* Fault in all pages */
    for (size_t i = 0; i < BUF_SIZE; i += PAGE_SIZE)
        attack_buf[i] = 0;

    /*
     * Warmup: populate L3 and establish eviction steady-state.
     * After this, most pages' CL 0 have been evicted at least once.
     */
    for (int pass = 0; pass < WARMUP_PASSES; pass++) {
        for (int p = 0; p < NUM_PAGES; p++) {
            attack_buf[(size_t)p * PAGE_SIZE] = (uint8_t)(pass ^ p);
        }
    }

    /*
     * ROI: measured attack phase.
     * Write CL 0 of every page repeatedly. Due to L3 set conflicts
     * (stride-4096 → only 256/16384 sets → 64 lines/set vs 16-way),
     * ~75% of CL 0 lines are evicted within each pass, generating
     * CXL write-backs (VN_UPDATEs). Every 2nd VN_UPDATE per page
     * triggers vn_private[0] > 1 → ONE_STEP → VAULT transition → reset.
     *
     * Each transition costs 8 extra DRAM writes on the shared VN bus.
     */
    SimRoiStart();

    for (int pass = 0; pass < ATTACK_PASSES; pass++) {
        for (int p = 0; p < NUM_PAGES; p++) {
            attack_buf[(size_t)p * PAGE_SIZE] = (uint8_t)(pass ^ p ^ 0xA5);
        }
    }

    SimRoiEnd();

    printf("Attack workload complete.\n");
    printf("Key metrics in sim.out:\n");
    printf("  vv[0].one-step-to-vault  — transition count (each = 8 extra DRAM writes)\n");
    printf("  vv[0].dram-writes        — total VN DRAM writes (includes transition penalty)\n");
    printf("  mee[i].vn-evictions      — VN table eviction pressure\n");
    return 0;
}
