/*
 * combined_attack_victim.c — Multi-tenant DoS demonstration for Toleo
 *
 * Models two co-located tenants sharing a single Toleo VN server:
 *
 *   Attacker threads (IDs 0 .. NUM_ATTACKERS-1):
 *     Write CL 0 of every page repeatedly. Due to L3 set conflicts at
 *     stride 4096, most cache lines are evicted within a pass, generating
 *     CXL write-backs. Every 2nd VN_UPDATE triggers vn_private > 1 →
 *     ONE_STEP → VAULT transition → 8 extra DRAM writes per transition
 *     on the shared 3.32 GB/s VN bus.
 *
 *   Victim threads (IDs NUM_ATTACKERS .. NUM_THREADS-1):
 *     Sequential writes over their own buffer. These maintain version
 *     locality (ONE_STEP) and produce minimal VN metadata traffic.
 *
 * Expected result under zen4_vn (Toleo):
 *   Victim latency degrades because attacker's 5× amplified VN traffic
 *   monopolizes the shared CXL VN bus queueing model.
 *
 * Control run under zen4_cxl (no freshness):
 *   No VN bus contention → victim unaffected → proves the DoS is
 *   Toleo-specific, not a generic memory bandwidth issue.
 *
 * Usage:
 *   ../../run-sniper -n 32 -c zen4_vn --roi -d combined_output \
 *       -- ./combined_attack_victim [num_threads]
 */

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sim_api.h"

#define DEFAULT_THREADS  8
#define PAGE_SIZE        4096
#define CL_SIZE          64

/* Attacker: 32 MB per thread, stride-4096 for L3 conflict evictions */
#define ATTACKER_PAGES   8192
#define ATTACKER_BUF     ((size_t)ATTACKER_PAGES * PAGE_SIZE)
#define WARMUP_PASSES    4
#define ATTACK_PASSES    20

/* Victim: 8 MB per thread, sequential access */
#define VICTIM_PAGES     2048
#define VICTIM_BUF       ((size_t)VICTIM_PAGES * PAGE_SIZE)
#define VICTIM_PASSES    10

static int g_num_threads;
static int g_num_attackers;
static pthread_barrier_t g_barrier;

static uint8_t **g_attack_bufs;
static uint8_t **g_victim_bufs;

/* ------------------------------------------------------------------ */
static void *attacker_thread(void *arg)
{
    int tid = (int)(intptr_t)arg;
    volatile uint8_t *buf = g_attack_bufs[tid];

    /* Fault in pages */
    for (size_t i = 0; i < ATTACKER_BUF; i += PAGE_SIZE)
        buf[i] = 0;

    /* Warmup: fill L3, establish eviction steady-state */
    for (int pass = 0; pass < WARMUP_PASSES; pass++) {
        for (int p = 0; p < ATTACKER_PAGES; p++) {
            buf[(size_t)p * PAGE_SIZE] = (uint8_t)(pass ^ p);
        }
    }

    pthread_barrier_wait(&g_barrier); /* sync: warmup done, ROI begins */

    /* Attack: write CL 0 of every page repeatedly.
     * Every 2 VN_UPDATEs per page triggers ONE_STEP → VAULT → reset cycle.
     * Each transition: 8 extra DRAM writes on shared VN bus. */
    for (int pass = 0; pass < ATTACK_PASSES; pass++) {
        for (int p = 0; p < ATTACKER_PAGES; p++) {
            buf[(size_t)p * PAGE_SIZE] = (uint8_t)(pass ^ p ^ 0xA5);
        }
    }

    pthread_barrier_wait(&g_barrier); /* sync: ROI ends */
    return NULL;
}

/* ------------------------------------------------------------------ */
static void *victim_thread(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int vid = tid - g_num_attackers;
    volatile uint8_t *buf = g_victim_bufs[vid];

    /* Fault in pages */
    for (size_t i = 0; i < VICTIM_BUF; i += PAGE_SIZE)
        buf[i] = 0;

    pthread_barrier_wait(&g_barrier); /* sync: wait for attacker warmup */

    /* Sequential writes: maintains version locality (ONE_STEP) */
    for (int pass = 0; pass < VICTIM_PASSES; pass++) {
        for (size_t off = 0; off < VICTIM_BUF; off += CL_SIZE) {
            buf[off] = (uint8_t)(pass ^ (off & 0xFF));
        }
    }

    pthread_barrier_wait(&g_barrier); /* sync: ROI ends */
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    g_num_threads   = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREADS;
    g_num_attackers = g_num_threads / 2;
    int num_victims = g_num_threads - g_num_attackers;

    if (g_num_threads < 2 || g_num_threads > 32) {
        fprintf(stderr, "Usage: %s [num_threads]  (2-32, default 8)\n", argv[0]);
        return 1;
    }

    printf("Combined DoS demo: %d attacker(s) + %d victim(s)\n",
           g_num_attackers, num_victims);

    g_attack_bufs = calloc(g_num_attackers, sizeof(uint8_t *));
    g_victim_bufs = calloc(num_victims,     sizeof(uint8_t *));

    for (int i = 0; i < g_num_attackers; i++) {
        g_attack_bufs[i] = malloc(ATTACKER_BUF);
        if (!g_attack_bufs[i]) { perror("malloc"); return 1; }
    }
    for (int i = 0; i < num_victims; i++) {
        g_victim_bufs[i] = malloc(VICTIM_BUF);
        if (!g_victim_bufs[i]) { perror("malloc"); return 1; }
    }

    pthread_barrier_init(&g_barrier, NULL, g_num_threads);

    pthread_t *threads = calloc(g_num_threads, sizeof(pthread_t));
    for (int i = 0; i < g_num_threads; i++) {
        void *(*fn)(void *) = (i < g_num_attackers) ? attacker_thread : victim_thread;
        pthread_create(&threads[i], NULL, fn, (void *)(intptr_t)i);
    }

    SimRoiStart();

    for (int i = 0; i < g_num_threads; i++)
        pthread_join(threads[i], NULL);

    SimRoiEnd();

    printf("\nCheck sim.out for:\n");
    printf("  vv[0].one-step-to-vault          — transition count (attack amplification)\n");
    printf("  vv[0].dram-writes                — total VN DRAM writes\n");
    printf("  cxl[i].total-effective-read-latency — victim read latency (DoS impact)\n");
    printf("  mee[i].vn-evictions              — VN table eviction pressure\n");
    printf("\nCompare with zen4_cxl run (no Toleo) — victim latency should be unaffected.\n");

    pthread_barrier_destroy(&g_barrier);
    for (int i = 0; i < g_num_attackers; i++) free(g_attack_bufs[i]);
    for (int i = 0; i < num_victims;     i++) free(g_victim_bufs[i]);
    free(g_attack_bufs); free(g_victim_bufs);
    free(threads);
    return 0;
}
