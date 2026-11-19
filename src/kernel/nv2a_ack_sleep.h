/*
 * nv2a_ack_sleep.h - the NV2A ack thread sleeps between kickoffs
 * (RECOMP_ACK_SLEEP; docs/technical/nv2a-ack-thread.md, phase 3).
 *
 * The ack loop (xbox_memory_layout.c) calls in at three places: the start,
 * a kickoff's announcement and the end of each pass. Off, every call
 * returns at once and the loop yields with Sleep(0) as before.
 */
#ifndef NV2A_ACK_SLEEP_H
#define NV2A_ACK_SLEEP_H

#include <stdint.h>

/* What one pass of the ack loop found. */
typedef struct {
    int flip_held;            /* the pass began with a flip held */
    int kicked;               /* an announced kickoff ended at its start */
    int put_moved;            /* DMA_PUT moved, so the pass walked */
    int mirrored;             /* mirror words the pass changed */
    uint32_t seen_put;        /* DMA_PUT as the pass last read it */
    uint64_t hold_sleep_ns;   /* RECOMP_ACK_HOLD_SLEEP_US */
} nv2a_ack_pass;

/* RECOMP_ACK_SLEEP, read once. */
int nv2a_ack_sleep_on(void);

/* Before the ack thread starts. put_reg is DMA_PUT in the register file;
 * kick_wakes is 0 when RECOMP_KICK_SPIN_US=0 announces no kickoffs. */
void nv2a_ack_sleep_init(volatile uint32_t *put_reg, int kick_wakes);

/* A kickoff was announced (g_nv2a_kick_waiting set): wake the thread if it
 * sleeps. */
void nv2a_ack_sleep_kick(void);

/* At the end of a pass: spins, waits or sleeps as RECOMP_ACK_SLEEP says and
 * returns 1, or returns 0 (off) for the loop's own yield. */
int nv2a_ack_sleep_pass_end(const nv2a_ack_pass *p);

/* How far the MCPX counters move this pass (RECOMP_MCPX_COUNTER_HZ). */
uint32_t nv2a_ack_mcpx_step(void);

#endif /* NV2A_ACK_SLEEP_H */
