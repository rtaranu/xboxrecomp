/*
 * nv2a_ack_stats.h - what each pass of the NV2A ack thread found
 * (RECOMP_ACK_STATS; docs/technical/nv2a-ack-thread.md, Measurements).
 *
 * Plain increments on the ack thread (the walker runs there too), read by a
 * reporter thread that appends one block to a file every few seconds. Off,
 * each site costs one load and branch.
 */
#ifndef NV2A_ACK_STATS_H
#define NV2A_ACK_STATS_H

#include <stddef.h>
#include <stdint.h>

enum { ACK_HIST_BUCKETS = 24, ACK_INTR_REGS = 5, ACK_IDLE_REGS = 2,
       ACK_WRITERS = 48 };

/* RECOMP_ACK_WRITES: one register and one writing instruction. */
typedef struct {
    uint32_t offset;     /* in the register file */
    uint32_t rva;        /* the writing instruction, from the exe's base */
    int toolkit;         /* written by the ack thread, not the title */
    uint64_t n;
} nv2a_ack_writer;

/* The previous write by the same title thread, for the order of a kickoff:
 * the PFB flush, DMA_PUT, or another register on those pages. */
enum { ACKW_OTHER, ACKW_FLUSH, ACKW_PUT, ACKW_KINDS };

typedef struct {
    uint64_t passes, passes_work, flips;
    /* The PFB flush, by who cleared it: the pass, the walker between
     * commands, or the announced-kickoff spin. */
    uint64_t flush_pass, flush_walk, flush_spin, kick_spin_timeout;
    uint64_t intr[ACK_INTR_REGS];        /* PMC PBUS PFIFO PGRAPH PCRTC */
    uint32_t intr_bits[ACK_INTR_REGS];   /* every bit ever cleared */
    uint64_t idle[ACK_IDLE_REGS];        /* idle bits found clear */
    uint64_t put_moved;
    /* Mirrors that changed something, and how many of those changes came on
     * a pass where PUT had not moved. */
    uint64_t fence, dsp, poke, counter, mirror_noput;
    uint64_t frame_tick;                 /* frame counters' time fallback */
    uint64_t vbl_backstop;               /* xbox_VblankAckHeld gave the bits back */
    /* RECOMP_ACK_SLEEP: sleeps, how each ended (an announced kickoff, or
     * the safety net's timeout), and timeouts whose pass found work. */
    uint64_t sleeps, wake_kick, wake_net, net_work;
    /* Sleeps while a flip is held, and those a vblank (or a kickoff) ended. */
    uint64_t hold_sleeps, hold_woken;
    /* Passes that did not end in a sleep, by why: spinning on an announced
     * kickoff's flush (not counted in passes), a kickoff announced at the
     * pass's end, RECOMP_ACK_SPIN_US since the last work, the wait for the
     * PUT after a flush, and a sleep that work called off at once. */
    uint64_t up_kick_spin, up_kick_wait, up_spin, up_put_wait, up_refused;
    /* Powers of two in microseconds: bucket 0 is < 1 us, bucket i < 2^i us. */
    uint64_t h_pass[ACK_HIST_BUCKETS];   /* start of a pass to the next */
    uint64_t h_kick[ACK_HIST_BUCKETS];   /* kickoff announced to flush cleared */
    uint64_t h_walk[ACK_HIST_BUCKETS];   /* PUT seen to GET = PUT */
    uint64_t h_gap[ACK_HIST_BUCKETS];    /* one kickoff announced to the next */
    /* RECOMP_ACK_WRITES: writes to the PFB and PFIFO USER pages. */
    uint64_t w_title, w_toolkit, w_lost;  /* lost: the writer table was full */
    uint64_t w_order[ACKW_KINDS][ACKW_KINDS];   /* [previous][this], title */
    int w_count;
    nv2a_ack_writer w[ACK_WRITERS];
} nv2a_ack_stats;

extern int g_ack_stats_on;
extern int g_ack_pass_work;
extern nv2a_ack_stats g_ack_stats;
/* When the title announced its kickoff (xbox_HostNowNs), 0 once answered. */
extern volatile uint64_t g_ack_kick_ns;

/* Reads RECOMP_ACK_STATS once and starts the reporter. */
void nv2a_ack_stats_init(void);
void nv2a_ack_stats_hist(uint64_t *h, uint64_t ns);

/* The ack loop's call sites (xbox_memory_layout.c), called only with
 * g_ack_stats_on unless they say otherwise. */
void nv2a_ack_stats_pass(volatile const uint32_t *regs);   /* a pass begins */
/* A busy-bit pass cleared bits of NV2A_ACK[i] (at offset): i 0 is the PFB
 * flush, kick_ns its announced kickoff's stamp as read before the clear. */
void nv2a_ack_stats_cleared(size_t i, uint32_t offset, uint32_t bits,
                            int from_walk, uint64_t kick_ns);
/* The announced kickoff's flush was answered by the walker or the spin. */
void nv2a_ack_stats_flush(int from_walk, uint64_t kick_ns);
void nv2a_ack_stats_mirrors(int fence, int dsp, int poke, int counter);
/* A kickoff announced; stamp: it will be waited for. */
void nv2a_ack_stats_kick_announced(int stamp);
/* The walk of a moved PUT: begin returns its start (0 with the stats off),
 * end takes it. Both are safe with the stats off. */
uint64_t nv2a_ack_stats_walk_begin(void);
void nv2a_ack_stats_walk_end(uint64_t start_ns);
/* Ends the kickoff stamped t, unless the title has announced the next one
 * meanwhile: its stamp stays. Safe with the stats off (t is then 0). */
void nv2a_ack_stats_kick_end(uint64_t t);
/* RECOMP_ACK_WRITES=1, with the stats: arm the write trap on nv2a_memory
 * (the register file), ack_tid being the ack thread. */
void nv2a_ack_stats_writes_init(void *nv2a_memory, unsigned long ack_tid);

/* A count that is work for the pass, and one that is not. */
#define ACK_COUNT(field) \
    do { if (g_ack_stats_on) { g_ack_stats.field++; g_ack_pass_work = 1; } } while (0)
#define ACK_ADD(field) \
    do { if (g_ack_stats_on) g_ack_stats.field++; } while (0)

#endif /* NV2A_ACK_STATS_H */
