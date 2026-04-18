/*
 * combined_attack_victim.c — Multi-tenant DoS demonstration for Toleo
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

/* Attacker: 32 MB per thread */
#define ATTACKER_PAGES   8192
#define ATTACKER_BUF     ((size_t)ATTACKER_PAGES * PAGE_SIZE)
#define WARMUP_PASSES    4
#define ATTACK_PASSES    10  /* Reduced from 20 to avoid simulator timeouts */

/* Victim: 8 MB per thread */
#define VICTIM_PAGES     2048
#define VICTIM_BUF       ((size_t)VICTIM_PAGES * PAGE_SIZE)
#define VICTIM_PASSES    10

static int g_num_threads;
static int g_num_attackers;
static pthread_barrier_t g_barrier;

static uint8_t **g_attack_bufs;
static uint8_t **g_victim_bufs;

static void *attacker_thread(void *arg)
{
    int tid = (int)(intptr_t)arg;
    volatile uint8_t *buf = g_attack_bufs[tid];

    for (size_t i = 0; i < ATTACKER_BUF; i += PAGE_SIZE) buf[i] = 0;

    for (int pass = 0; pass < WARMUP_PASSES; pass++) {
        for (int p = 0; p < ATTACKER_PAGES; p++) {
            buf[(size_t)p * PAGE_SIZE] = (uint8_t)(pass ^ p);
        }
    }

    /* Master thread triggers ROI Start */
    int res = pthread_barrier_wait(&g_barrier);
    if (res == PTHREAD_BARRIER_SERIAL_THREAD) SimRoiStart();
    pthread_barrier_wait(&g_barrier);

    for (int pass = 0; pass < ATTACK_PASSES; pass++) {
        for (int p = 0; p < ATTACKER_PAGES; p++) {
            buf[(size_t)p * PAGE_SIZE] = (uint8_t)(pass ^ p ^ 0xA5);
        }
    }

    /* Master thread triggers ROI End */
    res = pthread_barrier_wait(&g_barrier);
    if (res == PTHREAD_BARRIER_SERIAL_THREAD) SimRoiEnd();
    pthread_barrier_wait(&g_barrier);

    return NULL;
}

static void *victim_thread(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int vid = tid - g_num_attackers;
    volatile uint8_t *buf = g_victim_bufs[vid];

    for (size_t i = 0; i < VICTIM_BUF; i += PAGE_SIZE) buf[i] = 0;

    /* Wait for warmup to finish */
    pthread_barrier_wait(&g_barrier);
    pthread_barrier_wait(&g_barrier);

    for (int pass = 0; pass < VICTIM_PASSES; pass++) {
        for (size_t off = 0; off < VICTIM_BUF; off += CL_SIZE) {
            buf[off] = (uint8_t)(pass ^ (off & 0xFF));
        }
    }

    /* ROI End barrier */
    pthread_barrier_wait(&g_barrier);
    pthread_barrier_wait(&g_barrier);

    return NULL;
}

int main(int argc, char *argv[])
{
    g_num_threads   = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREADS;
    g_num_attackers = g_num_threads / 2;
    int num_victims = g_num_threads - g_num_attackers;

    if (g_num_threads < 2 || g_num_threads > 32) return 1;

    printf("Combined DoS demo: %d attacker(s) + %d victim(s)\n", g_num_attackers, num_victims);

    g_attack_bufs = calloc(g_num_attackers, sizeof(uint8_t *));
    g_victim_bufs = calloc(num_victims,     sizeof(uint8_t *));

    for (int i = 0; i < g_num_attackers; i++) g_attack_bufs[i] = malloc(ATTACKER_BUF);
    for (int i = 0; i < num_victims; i++)     g_victim_bufs[i] = malloc(VICTIM_BUF);

    pthread_barrier_init(&g_barrier, NULL, g_num_threads);

    pthread_t *threads = calloc(g_num_threads, sizeof(pthread_t));
    for (int i = 0; i < g_num_threads; i++) {
        void *(*fn)(void *) = (i < g_num_attackers) ? attacker_thread : victim_thread;
        pthread_create(&threads[i], NULL, fn, (void *)(intptr_t)i);
    }

    for (int i = 0; i < g_num_threads; i++) pthread_join(threads[i], NULL);

    pthread_barrier_destroy(&g_barrier);
    for (int i = 0; i < g_num_attackers; i++) free(g_attack_bufs[i]);
    for (int i = 0; i < num_victims;     i++) free(g_victim_bufs[i]);
    free(g_attack_bufs); free(g_victim_bufs);
    free(threads);
    return 0;
}
