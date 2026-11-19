/*
 * nv2a_ack_stats.c - the ack thread's counters and their reporter
 * (nv2a_ack_stats.h).
 *
 * RECOMP_ACK_STATS=<file> (or 1 for ack-stats.log) turns them on. Every
 * ACK_REPORT_SECS a thread of its own appends what the last interval found,
 * so a run ended by taskkill or a closed window still leaves its numbers, and
 * the ack thread never writes a line. Read the interval at the spot.
 */
#include "nv2a_ack_stats.h"
#include "xbox_memory_layout.h"   /* windows.h, RECOMP_TLS */
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACK_REPORT_SECS = 5 };

/* The two bits nv2a_ack_stats_cleared calls a backstop. */
#define PCRTC_INTR_0       0x600100u
#define PCRTC_INTR_VBLANK  (1u << 0)

int g_ack_stats_on;
int g_ack_pass_work;
nv2a_ack_stats g_ack_stats;
volatile uint64_t g_ack_kick_ns;

static FILE *s_out;

/* ── The ack loop's call sites ── */

void nv2a_ack_stats_pass(volatile const uint32_t *regs)
{
    static uint64_t pass_ns;
    static uint32_t pcrtc_start;
    uint64_t now = xbox_HostNowNs();
    uint32_t start = *(volatile const uint32_t *)((const char *)regs + 0x600800);

    if (pass_ns)
        nv2a_ack_stats_hist(g_ack_stats.h_pass, now - pass_ns);
    pass_ns = now;
    g_ack_stats.passes++;
    if (g_ack_pass_work)
        g_ack_stats.passes_work++;
    g_ack_pass_work = 0;
    if (start != pcrtc_start) {
        pcrtc_start = start;
        g_ack_stats.flips++;
    }
}

void nv2a_ack_stats_kick_end(uint64_t t)
{
    if (t)
        InterlockedCompareExchange64((volatile LONG64 *)&g_ack_kick_ns, 0,
                                     (LONG64)t);
}

/* One kickoff-latency sample. t is the stamp as read before the flush was
 * cleared: once it is clear the title is free to stamp its next kickoff. */
static void kick_sample(uint64_t t)
{
    uint64_t now = xbox_HostNowNs();

    if (!t)
        return;
    if (now > t)
        nv2a_ack_stats_hist(g_ack_stats.h_kick, now - t);
    nv2a_ack_stats_kick_end(t);
}

void nv2a_ack_stats_cleared(size_t i, uint32_t offset, uint32_t bits,
                            int from_walk, uint64_t kick_ns)
{
    extern int xbox_VblankAckOwned(void);

    if (i == 0) {
        if (from_walk)
            ACK_COUNT(flush_walk);
        else
            ACK_COUNT(flush_pass);
        kick_sample(kick_ns);
        return;
    }
    ACK_COUNT(intr[i - 1]);
    g_ack_stats.intr_bits[i - 1] |= bits;
    if (offset == PCRTC_INTR_0 && (bits & PCRTC_INTR_VBLANK)
            && xbox_VblankAckOwned())
        ACK_COUNT(vbl_backstop);
}

void nv2a_ack_stats_flush(int from_walk, uint64_t kick_ns)
{
    if (from_walk)
        ACK_COUNT(flush_walk);
    else
        ACK_COUNT(flush_spin);
    kick_sample(kick_ns);
}

void nv2a_ack_stats_mirrors(int fence, int dsp, int poke, int counter)
{
    g_ack_stats.fence += (uint64_t)fence;
    g_ack_stats.dsp += (uint64_t)dsp;
    g_ack_stats.poke += (uint64_t)poke;
    g_ack_stats.counter += (uint64_t)counter;
    if (fence + dsp + poke + counter)
        g_ack_pass_work = 1;
}

/* The gaps say how many kickoffs a short spin would catch and how many
 * would find a sleeping worker. A hold's wait is in the gap. */
void nv2a_ack_stats_kick_announced(int stamp)
{
    static uint64_t last_ns;
    uint64_t now = xbox_HostNowNs();

    if (last_ns)
        nv2a_ack_stats_hist(g_ack_stats.h_gap, now - last_ns);
    last_ns = now;
    if (stamp)
        g_ack_kick_ns = now;
}

uint64_t nv2a_ack_stats_walk_begin(void)
{
    if (!g_ack_stats_on)
        return 0;
    ACK_COUNT(put_moved);
    return xbox_HostNowNs();
}

void nv2a_ack_stats_walk_end(uint64_t start_ns)
{
    if (start_ns)
        nv2a_ack_stats_hist(g_ack_stats.h_walk, xbox_HostNowNs() - start_ns);
}

/* ── RECOMP_ACK_WRITES ── */

/* Under the write trap's lock. */
static void writes_count(uint32_t offset, uint32_t rva, int toolkit)
{
    static RECOMP_TLS int last = -1;
    nv2a_ack_stats *s = &g_ack_stats;
    int i;

    if (toolkit) {
        s->w_toolkit++;
    } else {
        int kind = offset == 0x100410u ? ACKW_FLUSH
                 : offset == 0x800040u ? ACKW_PUT : ACKW_OTHER;
        s->w_title++;
        if (last >= 0)
            s->w_order[last][kind]++;
        last = kind;
    }
    for (i = 0; i < s->w_count; i++)
        if (s->w[i].offset == offset && s->w[i].rva == rva)
            break;
    if (i == s->w_count) {
        if (i == ACK_WRITERS) {
            s->w_lost++;
            return;
        }
        s->w[i].offset = offset;
        s->w[i].rva = rva;
        s->w[i].toolkit = toolkit;
        s->w_count = i + 1;
    }
    s->w[i].n++;
}

/* RECOMP_ACK_WRITES=1, with RECOMP_ACK_STATS: count every write to the PFB
 * page (the flush) and the PFIFO USER page (DMA_PUT, DMA_GET), with the
 * writing instruction and, for the title, the order of a kickoff's writes
 * (docs/technical/nv2a-ack-thread.md, Writes per page). The pages are
 * read-only and each write is single-stepped, as with the PGRAPH trap: a
 * measurement that slows every write, never a default. A write another
 * thread makes while the page is open for one step is not seen. */
#if defined(_WIN32)
static const uint32_t ACKW_PAGE[] = { 0x100000u, 0x800000u };
static CRITICAL_SECTION g_ackw_cs;
static char *g_ackw_regs;
static DWORD g_ackw_ack_tid;
static uintptr_t g_ackw_base;
static RECOMP_TLS int s_ackw_step = -1;

static LONG CALLBACK ackw_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode, old;

    if (code == EXCEPTION_SINGLE_STEP && s_ackw_step >= 0) {
        VirtualProtect(g_ackw_regs + ACKW_PAGE[s_ackw_step], 0x1000,
                       PAGE_READONLY, &old);
        s_ackw_step = -1;
        LeaveCriticalSection(&g_ackw_cs);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t off = ep->ExceptionRecord->ExceptionInformation[1]
                        - (uintptr_t)g_ackw_regs;

        for (int p = 0; p < (int)(sizeof ACKW_PAGE / sizeof ACKW_PAGE[0]); p++) {
            if (off - ACKW_PAGE[p] >= 0x1000)
                continue;
            EnterCriticalSection(&g_ackw_cs);
            if (!VirtualProtect(g_ackw_regs + ACKW_PAGE[p], 0x1000,
                                PAGE_READWRITE, &old)) {
                LeaveCriticalSection(&g_ackw_cs);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            writes_count((uint32_t)off & ~3u,
                         (uint32_t)(ep->ContextRecord->Rip - g_ackw_base),
                         GetCurrentThreadId() == g_ackw_ack_tid);
            s_ackw_step = p;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void nv2a_ack_stats_writes_init(void *nv2a_memory, unsigned long ack_tid)
{
    const char *e = getenv("RECOMP_ACK_WRITES");
    DWORD old;
    int on = 1;

    if (!e || *e != '1' || !g_ack_stats_on || !nv2a_memory)
        return;
    InitializeCriticalSection(&g_ackw_cs);
    g_ackw_regs = (char *)nv2a_memory;
    g_ackw_ack_tid = (DWORD)ack_tid;
    g_ackw_base = (uintptr_t)GetModuleHandleW(NULL);
    if (!AddVectoredExceptionHandler(1, ackw_veh))
        on = 0;
    for (size_t p = 0; on && p < sizeof ACKW_PAGE / sizeof ACKW_PAGE[0]; p++)
        on = VirtualProtect(g_ackw_regs + ACKW_PAGE[p], 0x1000,
                            PAGE_READONLY, &old) != 0;
    fprintf(stderr, "  [GPU] ack write count %s (PFB and PFIFO USER pages)\n",
            on ? "on" : "could not be armed");
}
#else
void nv2a_ack_stats_writes_init(void *nv2a_memory, unsigned long ack_tid)
{
    (void)nv2a_memory;
    (void)ack_tid;
    (void)writes_count;
}
#endif

void nv2a_ack_stats_hist(uint64_t *h, uint64_t ns)
{
    uint64_t us = ns / 1000u;
    int b = 0;

    while (us && b < ACK_HIST_BUCKETS - 1) {
        us >>= 1;
        b++;
    }
    h[b]++;
}

static void put_hist(const char *name, const uint64_t *now, const uint64_t *was)
{
    int any = 0;

    for (int b = 0; b < ACK_HIST_BUCKETS; b++) {
        uint64_t n = now[b] - was[b];
        if (!n)
            continue;
        if (!any)
            fprintf(s_out, "  %-5s us:", name);
        any = 1;
        /* The last bucket has no upper bound. */
        if (b == ACK_HIST_BUCKETS - 1)
            fprintf(s_out, " >=%llu:%llu", 1ull << (b - 1), (unsigned long long)n);
        else
            fprintf(s_out, " <%llu:%llu", 1ull << b, (unsigned long long)n);
    }
    if (any)
        fputc('\n', s_out);
}

#define D(f) ((unsigned long long)(s.f - p.f))

static void report(double t, const nv2a_ack_stats *cur, const nv2a_ack_stats *prev)
{
    const nv2a_ack_stats s = *cur, p = *prev;

    fprintf(s_out, "t=%.0f passes %llu work %llu flips %llu put %llu\n", t,
            D(passes), D(passes_work), D(flips), D(put_moved));
    fprintf(s_out, "  flush pass %llu walk %llu spin %llu spin-timeout %llu\n",
            D(flush_pass), D(flush_walk), D(flush_spin), D(kick_spin_timeout));
    fprintf(s_out, "  intr PMC %llu PBUS %llu PFIFO %llu PGRAPH %llu PCRTC %llu"
            " (bits ever %08X %08X %08X %08X %08X) backstop %llu\n",
            D(intr[0]), D(intr[1]), D(intr[2]), D(intr[3]), D(intr[4]),
            s.intr_bits[0], s.intr_bits[1], s.intr_bits[2], s.intr_bits[3],
            s.intr_bits[4], D(vbl_backstop));
    fprintf(s_out, "  idle %llu %llu  mirrors fence %llu dsp %llu poke %llu"
            " counter %llu (no PUT %llu)  frame-tick %llu\n",
            D(idle[0]), D(idle[1]), D(fence), D(dsp), D(poke), D(counter),
            D(mirror_noput), D(frame_tick));
    if (s.sleeps)
        fprintf(s_out, "  sleeps %llu woken by kickoff %llu timeout %llu"
                " (found work %llu)\n",
                D(sleeps), D(wake_kick), D(wake_net), D(net_work));
    if (s.hold_sleeps)
        fprintf(s_out, "  held-flip sleeps %llu woken %llu\n",
                D(hold_sleeps), D(hold_woken));
    if (s.sleeps || s.hold_sleeps)
        fprintf(s_out, "  awake: kick spin %llu, kick waiting %llu, spin %llu,"
                " PUT wait %llu, sleep refused %llu\n",
                D(up_kick_spin), D(up_kick_wait), D(up_spin), D(up_put_wait),
                D(up_refused));
    put_hist("pass", s.h_pass, p.h_pass);
    put_hist("kick", s.h_kick, p.h_kick);
    put_hist("walk", s.h_walk, p.h_walk);
    put_hist("gap", s.h_gap, p.h_gap);
    if (s.w_title + s.w_toolkit) {
        static const char *kind[ACKW_KINDS] = { "other", "flush", "PUT" };

        fprintf(s_out, "  writes title %llu toolkit %llu (table full %llu)\n",
                D(w_title), D(w_toolkit), D(w_lost));
        fprintf(s_out, "  order (previous -> this, title):");
        for (int a = 0; a < ACKW_KINDS; a++)
            for (int b = 0; b < ACKW_KINDS; b++)
                if (D(w_order[a][b]))
                    fprintf(s_out, " %s->%s %llu", kind[a], kind[b], D(w_order[a][b]));
        fputc('\n', s_out);
        for (int i = 0; i < s.w_count; i++) {
            uint64_t n = s.w[i].n - (i < p.w_count ? p.w[i].n : 0);
            if (n)
                fprintf(s_out, "    %06X by rva %08X%s: %llu\n", s.w[i].offset,
                        s.w[i].rva, s.w[i].toolkit ? " (toolkit)" : "",
                        (unsigned long long)n);
        }
    }
    fflush(s_out);
}

#ifdef _WIN32
static DWORD WINAPI reporter(LPVOID unused)
{
    static nv2a_ack_stats prev, cur;
    uint64_t t0 = xbox_HostNowNs();

    (void)unused;
    for (;;) {
        Sleep(ACK_REPORT_SECS * 1000);
        memcpy(&cur, (const void *)&g_ack_stats, sizeof cur);
        report((double)(xbox_HostNowNs() - t0) / 1e9, &cur, &prev);
        prev = cur;
    }
    return 0;
}
#endif

void nv2a_ack_stats_init(void)
{
    const char *e = getenv("RECOMP_ACK_STATS");

    if (!e || !*e || !strcmp(e, "0"))
        return;
    s_out = fopen(strcmp(e, "1") ? e : "ack-stats.log", "a");
    if (!s_out) {
        fprintf(stderr, "  [GPU] ack stats: cannot open %s\n", e);
        return;
    }
    fprintf(s_out, "--- ack stats, every %d s (counts per interval)\n",
            ACK_REPORT_SECS);
    fflush(s_out);
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, reporter, NULL, 0, NULL));
#endif
    g_ack_stats_on = 1;
    fprintf(stderr, "  [GPU] ack stats -> %s\n", strcmp(e, "1") ? e : "ack-stats.log");
}
