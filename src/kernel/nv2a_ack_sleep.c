/*
 * nv2a_ack_sleep.c - the NV2A ack thread sleeps between kickoffs
 * (nv2a_ack_sleep.h; docs/technical/nv2a-ack-thread.md, phase 3).
 *
 * RECOMP_ACK_SLEEP=1: after a pass with nothing to do the thread spins
 * RECOMP_ACK_SPIN_US more (default 32: Halo's kickoffs come 4-8 us apart in
 * bursts), then sleeps until a kickoff is announced or RECOMP_ACK_NET_US
 * passes (default 1000), the safety net that still finds what no
 * announcement reports. After an answered flush it also waits up to
 * ACK_PUT_WAIT_NS for the PUT that follows. While a flip is held the vblank
 * that may release it wakes the thread (xbox_Nv2aAckVblank);
 * RECOMP_ACK_HOLD_WAKE=0 sleeps RECOMP_ACK_HOLD_SLEEP_US a pass instead.
 */
#include "nv2a_ack_sleep.h"
#include "nv2a_ack_stats.h"
#include "xbox_memory_layout.h"
#include "kernel.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>

enum { ACK_PUT_WAIT_NS = 200000 };

extern volatile LONG g_nv2a_kick_waiting;
extern int nv2a_pb_exec_flip_held(void);
extern int nv2a_pb_exec_flip_poll(void);

static long ack_env_long(const char *name, long def, long lo, long hi)
{
    const char *e = getenv(name);
    long v = (e && *e) ? strtol(e, NULL, 10) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

int nv2a_ack_sleep_on(void)
{
    static volatile long on = -1;

    if (on < 0)
        on = ack_env_long("RECOMP_ACK_SLEEP", 0, 0, 1);
    return (int)on;
}

/* RECOMP_ACK_HOLD_WAKE=0: a held flip sleeps RECOMP_ACK_HOLD_SLEEP_US a
 * pass, as before the vblank woke the thread (an A/B switch). */
static int ack_hold_wake_on(void)
{
    static volatile long on = -1;

    if (on < 0)
        on = ack_env_long("RECOMP_ACK_HOLD_WAKE", 1, 0, 1);
    return (int)on;
}

static uint64_t ack_spin_ns(void)
{
    static volatile long us = -1;

    if (us < 0)
        us = ack_env_long("RECOMP_ACK_SPIN_US", 32, 0, 5000);
    return (uint64_t)us * 1000u;
}

static uint64_t ack_net_ns(void)
{
    static volatile long us = -1;

    if (us < 0)
        us = ack_env_long("RECOMP_ACK_NET_US", 1000, 100, 20000);
    return (uint64_t)us * 1000u;
}

/* RECOMP_MCPX_COUNTER_HZ: 0 adds 1 a pass (the old model, whose rate is the
 * loop's speed), N follows the clock at N a second. The default is 0, and
 * 48000 under RECOMP_ACK_SLEEP, where a sleeping loop would all but stop a
 * per-pass counter: the APU's sample rate, which the emulated APU's own
 * counter keeps. */
uint32_t nv2a_ack_mcpx_step(void)
{
    static long hz = -1;
    static uint64_t last_ns, frac;
    uint64_t now, n;

    if (hz < 0) {
        hz = ack_env_long("RECOMP_MCPX_COUNTER_HZ",
                          nv2a_ack_sleep_on() ? 48000 : 0, 0, 100000000);
        fprintf(stderr, "  [GPU] MCPX counter: %s%ld\n",
                hz ? "per second " : "per pass ", hz ? hz : 1L);
    }
    if (!hz)
        return 1;
    now = xbox_HostNowNs();
    if (!last_ns)
        last_ns = now;
    n = (now - last_ns) * (uint64_t)hz + frac;
    last_ns = now;
    frac = n % 1000000000u;
    return (uint32_t)(n / 1000000000u);
}

#if defined(_WIN32)

/* 1 while the ack thread is asleep or about to be. Whoever takes it back to
 * 0 signals the event (auto-reset), so a kickoff costs the title a system
 * call only when the thread is really asleep. */
static volatile LONG g_ack_sleeping;
static HANDLE g_ack_wake;
static volatile uint32_t *g_put_reg;

/* After g_nv2a_kick_waiting is set (an interlocked store, so a full fence:
 * the thread sets g_ack_sleeping before it reads g_nv2a_kick_waiting). */
void nv2a_ack_sleep_kick(void)
{
    if (g_ack_sleeping && InterlockedExchange(&g_ack_sleeping, 0))
        SetEvent(g_ack_wake);
}

/* The thread takes the flag back itself. If the title took it first, its
 * SetEvent is on the way: wait for it, or the next sleep ends on it. */
static void ack_awake(void)
{
    if (!InterlockedExchange(&g_ack_sleeping, 0))
        WaitForSingleObject(g_ack_wake, INFINITE);
}

/* From the timer thread, after each vblank is counted: the vblank may
 * release a held flip, which the ack thread polls for. Without this a
 * sleeping thread saw the release only at its safety net's timeout. */
void xbox_Nv2aAckVblank(void)
{
    if (g_ack_wake && nv2a_pb_exec_flip_held())
        nv2a_ack_sleep_kick();
}

void nv2a_ack_sleep_init(volatile uint32_t *put_reg, int kick_wakes)
{
    if (!nv2a_ack_sleep_on())
        return;
    g_put_reg = put_reg;
    g_ack_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    fprintf(stderr, "  [GPU] ack thread sleeps: spin %llu us, safety net %llu us%s\n",
            (unsigned long long)(ack_spin_ns() / 1000),
            (unsigned long long)(ack_net_ns() / 1000),
            kick_wakes ? "" : " (RECOMP_KICK_SPIN_US=0: no kickoff wakes it)");
}

/* 1 if the event ended it (a kickoff, or a vblank while held), 0 the
 * timeout, -1 no sleep (work came). held: a flip is held, so the walk
 * waits and a new PUT is no reason to stay up; its release is. */
static int ack_sleep(uint64_t ns, uint32_t seen_put, int held)
{
    static HANDLE timer;
    LARGE_INTEGER due;
    HANDLE hs[2];
    DWORD r;

    if (!g_ack_wake)
        return -1;
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       0x00000002 /* HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
    InterlockedExchange(&g_ack_sleeping, 1);
    /* A kickoff announced, PUT written or the flip released since the pass
     * looked. The timer thread counts the vblank before it reads the flag,
     * this thread sets the flag before it polls: one of them sees the other. */
    if (g_nv2a_kick_waiting
            || (held ? !nv2a_pb_exec_flip_poll()
                     : (g_put_reg && *g_put_reg != seen_put))) {
        ack_awake();
        return -1;
    }
    if (held)
        ACK_ADD(hold_sleeps);
    else
        ACK_ADD(sleeps);
    due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        hs[0] = g_ack_wake;
        hs[1] = timer;
        r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
    } else {
        r = WaitForSingleObject(g_ack_wake, (DWORD)((ns + 999999u) / 1000000u));
    }
    if (r == WAIT_OBJECT_0) {
        if (held)
            ACK_ADD(hold_woken);
        else
            ACK_ADD(wake_kick);
        return 1;
    }
    ack_awake();
    if (!held)
        ACK_ADD(wake_net);
    return 0;
}

int nv2a_ack_sleep_pass_end(const nv2a_ack_pass *p)
{
    /* When the thread last had work, how long it waits for the PUT after an
     * answered flush, and whether the safety net woke this pass. One
     * caller: the ack thread. */
    static uint64_t last_work_ns, put_wait_until;
    static int net_woken;
    uint64_t now;

    if (!nv2a_ack_sleep_on())
        return 0;
    now = xbox_HostNowNs();
    if (p->kicked) {
        last_work_ns = now;
        if (!p->put_moved)
            put_wait_until = now + ACK_PUT_WAIT_NS;
    }
    if (p->put_moved || p->mirrored) {
        last_work_ns = now;
        put_wait_until = 0;
        if (net_woken && !p->kicked)
            ACK_ADD(net_work);
    }
    net_woken = 0;

    /* Asked again: a hold this pass began may have ended meanwhile. */
    if (p->flip_held && ack_hold_wake_on()) {
        ack_sleep(ack_net_ns(), p->seen_put, 1);
    } else if (p->flip_held && p->hold_sleep_ns && nv2a_pb_exec_flip_poll()) {
        xbox_HostSleepNs(p->hold_sleep_ns);
    } else if (g_nv2a_kick_waiting) {
        ACK_ADD(up_kick_wait);   /* straight to the next pass */
    } else if (now - last_work_ns < ack_spin_ns()) {
        ACK_ADD(up_spin);
        YieldProcessor();
    } else if (now < put_wait_until) {
        ACK_ADD(up_put_wait);
        YieldProcessor();
    } else {
        int r = ack_sleep(ack_net_ns(), p->seen_put, 0);

        net_woken = r == 0;
        if (r < 0)
            ACK_ADD(up_refused);
    }
    return 1;
}

#else  /* not built for other hosts yet: RECOMP_ACK_SLEEP does nothing */

void nv2a_ack_sleep_kick(void) {}
void xbox_Nv2aAckVblank(void) {}
void nv2a_ack_sleep_init(volatile uint32_t *put_reg, int kick_wakes)
{
    (void)put_reg;
    (void)kick_wakes;
}
int nv2a_ack_sleep_pass_end(const nv2a_ack_pass *p)
{
    (void)p;
    return 0;
}

#endif
