/*
 * nv2a_flip_hold.h - when a flip may complete, on the guest's vblank count.
 *
 * On the console a flip completes at a vblank. The XDK's Swap pushes a
 * NO_OPERATION whose parameter is software method 1 (the swap: framebuffer
 * address, presentation interval, IMMEDIATE), then FLIP_INCREMENT_WRITE and
 * FLIP_STALL. PGRAPH traps the NOP to D3D's interrupt handler, which queues
 * the flip for a vblank; the vblank DPC flips and writes PGRAPH_INCREMENT,
 * and only then does the GPU get past FLIP_STALL. The title feels that as
 * a KickOff or a fence that waits.
 *
 * The toolkit raises no GPU interrupts, so none of that runs, and a
 * FLIP_STALL used to end at once: a title that does not pace itself on the
 * CPU flipped as fast as the executor could draw (hundreds per second in a
 * simple menu) while its vblank ran at 60. The walker now holds after a
 * FLIP_STALL until the guest's vblank count allows the flip (nv2a_pb_exec.c
 * arms it, the ack loop in xbox_memory_layout.c holds the next walk, the
 * DMA_GET advance and the PFB flush ack).
 *
 * The rule here is pure: counts and times come in as arguments, so
 * tests/flip_hold drives it without a kernel.
 *
 *   target  = last_flip + interval
 *   release when (int32)(count - target) >= 0
 *
 * Not D3D's max(last + interval, now + 1): a title that already takes its
 * interval per frame (one that waits for two vblanks itself) is never held,
 * and a late frame on a slow backend is not pushed to the next vblank.
 * The hold is opt-in: RECOMP_FLIP_PACING=1 turns it on, =edge with D3D's
 * rule for A/B runs; unset, flips complete at once as before.
 */
#ifndef NV2A_FLIP_HOLD_H
#define NV2A_FLIP_HOLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No hold lasts longer than this, whatever the count says. */
#define XBOX_FLIP_HOLD_MAX_NS    100000000ull
/* A vblank count that has not moved for this long is not a clock. */
#define XBOX_FLIP_CLOCK_STALE_NS 100000000ull

/* A title's per-frame choice: given the interval its swap asked for (0 for
 * IMMEDIATE; 0-3, more reads as 3), the interval to hold the flip for (0
 * flips at once; above 3 reads as 3). Answering what it was asked keeps
 * the swap's own interval, so a policy cannot cut a swap's 4-7 (which XDK
 * D3D does not write) to 3. Runs on the GPU thread at each
 * FLIP_STALL: it may read guest memory but must not call guest code or
 * block. One call may still be running, or start, just after the policy is
 * cleared or replaced: it and what it reads stay valid for the whole run. */
typedef unsigned (*XboxFlipIntervalPolicy)(unsigned asked);

typedef struct {
    int      edge;          /* RECOMP_FLIP_PACING=edge: D3D's rule */
    /* The interval a flip is held for, when it is not the one asked for
     * (docs/technical/flip-interval.md). interval_max: a swap asking for
     * more vblanks is held for this many (0: no cap), e.g. 1 shows a TWO
     * title's frames at 60 Hz. policy, when set, decides instead. Both are
     * the title's and kept while forced. forced: forced_max is held to
     * instead and the policy is not asked (RECOMP_FLIP_INTERVAL_MAX, or a
     * debug switch). Written from the title's threads, read at each stall. */
    volatile unsigned interval_max;
    XboxFlipIntervalPolicy volatile policy;
    volatile int      forced;
    volatile unsigned forced_max;
    /* The latest swap NOP since the previous FLIP_STALL. */
    int      have_swap;
    unsigned swap_interval;
    int      swap_immediate;
    /* The vblank the previous flip was released on, and the interval it
     * asked for (0: IMMEDIATE). */
    int      have_last;
    uint32_t last_flip;
    unsigned last_interval;
    /* A pending hold. */
    int      held;
    uint32_t target;
    uint32_t stall_count;
    uint64_t held_since_ns;
    /* Statistics, for the [GPU] report; the caller resets them. */
    uint32_t holds, timeouts, clock_stops, nonop, iv[4], changed;
    uint64_t held_ns;
} XboxFlipHold;

/* RECOMP_FLIP_INTERVAL_MAX: hold every flip for at most max vblanks (0: as
 * asked) and ignore the title's cap and policy until released. */
void xbox_FlipHoldForceCap(XboxFlipHold *h, unsigned max);

/* End a forced cap: the title's last cap and policy decide again. */
void xbox_FlipHoldReleaseCap(XboxFlipHold *h);

/* The cap held to now: the forced one, else the title's. */
unsigned xbox_FlipHoldCap(const XboxFlipHold *h);

/* The title's cap (0: as asked) and policy (NULL: none). Kept while the cap
 * is forced, for its release; return 0 then, as they change nothing yet. */
int xbox_FlipHoldSetCap(XboxFlipHold *h, unsigned max);
int xbox_FlipHoldSetPolicy(XboxFlipHold *h, XboxFlipIntervalPolicy fn);

/* A NO_OPERATION parameter. Returns 1 when it is the swap (software method
 * 1) and was recorded, 0 for anything else, which changes nothing. */
int xbox_FlipHoldSwap(XboxFlipHold *h, uint32_t param);

/* A FLIP_STALL, with the vblank count sampled when the walk that reached it
 * began (a stall reached late in a walk is not taken for a new frame).
 * clock_ok: the count is moving (vblanks on, advanced recently). Returns 1
 * when the flip is now held, 0 when it completes at once. */
int xbox_FlipHoldStall(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                       int clock_ok);

/* Once per pass of the walker's loop. Returns 1 while the flip is still
 * held, 0 once it is released (or none was pending). */
int xbox_FlipHoldPoll(XboxFlipHold *h, uint32_t count, uint64_t now_ns,
                      int clock_ok);

/* Drop a pending hold without waiting (a walk that lost its place). */
void xbox_FlipHoldCancel(XboxFlipHold *h, uint64_t now_ns);

/* The vblank the title's next flip is due on, for a backend that presents
 * on the title's thread and would otherwise show each frame whenever the
 * title gets to its Swap: the pending hold's target, or the last flip, plus
 * the interval the latest swap asked for. A present that waits for it lands
 * on the vblank, and the stall that follows finds its target reached, so
 * the two never hold the same frame twice. Returns 0 when there is nothing
 * to wait for: no flip yet, IMMEDIATE, or the edge rule (which holds a
 * frame stalled on its target one more vblank). */
int xbox_FlipHoldDue(const XboxFlipHold *h, uint32_t *due);

/* A frame counter the toolkit bumps: does something else (the title's own
 * vblank DPC) keep moving it? Called once per bump with the value the bump
 * found and the time in ms. Returns 1 while the counter is the title's, and
 * then the caller does not bump it. Never the title's before the first bump
 * has seeded last_written.
 *
 * One move is not enough. Burnout 3's counter moves once on its own at boot
 * (3 after the toolkit's 2) and never again; taken as the title's there, it
 * was left alone, and the title waited on it at its first movie for good.
 * Nor is a slow trickle: something writing the counter once a second would
 * pile up moves over a few seconds. So a counter is the title's once it has
 * moved on its own at XBOX_FRAME_COUNTER_FOREIGN bumps within
 * XBOX_FRAME_COUNTER_WINDOW_MS, which a counter the title's DPC drives at
 * 60 Hz reaches within a few frames at any flip rate above 4 per second.
 * And it is the title's only while it keeps moving: one that has not moved
 * for XBOX_FRAME_COUNTER_IDLE_MS (15 vblanks) goes back to the toolkit, so
 * a wrong hand-over costs a quarter of a second, not the rest of the run. */
#define XBOX_FRAME_COUNTER_FOREIGN    3
#define XBOX_FRAME_COUNTER_WINDOW_MS  500u
#define XBOX_FRAME_COUNTER_IDLE_MS    250u

typedef struct {
    uint32_t foreign;   /* moves on its own in the current window */
    uint32_t first_ms;  /* when the window's first move was seen */
    int      owned;     /* the title's: not bumped */
    uint32_t last_seen; /* owned: the value last read */
    uint32_t moved_ms;  /* owned: when it last moved */
} XboxFrameCounterWatch;

int xbox_FrameCounterOwned(XboxFrameCounterWatch *w, int seeded,
                           uint32_t last_written, uint32_t cur,
                           uint32_t now_ms);

/* One bump of a registered counter word, the part of xbox_memory_layout.c's
 * frame_counter_bump after the guest pointers are resolved. pacing is
 * RECOMP_FLIP_PACING: off, every call adds one, as before the hold, and the
 * watch never runs. On, the watch decides. Returns what happened, for the
 * caller's log line. */
typedef struct {
    uint32_t last_written;
    int      seeded;
    XboxFrameCounterWatch watch;
} XboxFrameCounterBump;

enum {
    XBOX_FRAME_COUNTER_BUMPED,       /* the toolkit's: one added */
    XBOX_FRAME_COUNTER_TITLES,       /* the title's: left alone */
    XBOX_FRAME_COUNTER_HANDED_OVER,  /* became the title's at this call */
    XBOX_FRAME_COUNTER_HANDED_BACK   /* stopped moving: the toolkit's again */
};

int xbox_FrameCounterBump(XboxFrameCounterBump *b, volatile uint32_t *word,
                          int pacing, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_FLIP_HOLD_H */
