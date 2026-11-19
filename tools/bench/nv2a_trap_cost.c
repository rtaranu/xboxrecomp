/*
 * nv2a_trap_cost.c - what one trapped register write costs, and how long a
 * sleeping thread takes to wake on an event.
 *
 * The two standalone measurements in docs/technical/nv2a-ack-thread.md
 * (Measurements: Trap cost, Wake latency). Windows x64 only. From a Visual
 * Studio x64 prompt:
 *
 *     cl /O2 /W3 nv2a_trap_cost.c && nv2a_trap_cost
 *
 * Trap cost, three ways of writing one word:
 *   plain    a store to writable memory, the baseline;
 *   step     the store faults on a read-only page; the handler opens the
 *            page, single-steps the store and closes it again (two
 *            exceptions, two VirtualProtect calls: the PGRAPH trap today);
 *   emulate  the store faults on a read-only view of a section; the handler
 *            does the store itself through a writable view of the same
 *            section and skips the instruction (one exception, no
 *            protection change: phase 4's aperture).
 *
 * Wake latency: one thread sets an event at random gaps, another sleeps on
 * it, with and without a short spin on a flag first.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define WRITES 200000
#define WAKES  20000

static LARGE_INTEGER s_freq;

static uint64_t ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

static double to_us(uint64_t d)
{
    return (double)d * 1e6 / (double)s_freq.QuadPart;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *d, int n, uint64_t total)
{
    qsort(d, n, sizeof d[0], cmp_u64);
    printf("  %-14s mean %7.2f us  median %7.2f  p90 %7.2f  p99 %7.2f  max %8.1f\n",
           name, to_us(total) / n, to_us(d[n / 2]), to_us(d[n * 9 / 10]),
           to_us(d[n * 99 / 100]), to_us(d[n - 1]));
}

/* ---- trap cost ---------------------------------------------------------- */

typedef void (*store_fn)(volatile uint32_t *p, uint32_t v);
static store_fn s_store;            /* mov [rcx], edx; ret */
static char *s_guestview, *s_hostview;      /* read-only view, writable view */
static int s_mode;                  /* 1 step, 2 emulate */
static __declspec(thread) int t_stepping;

static LONG CALLBACK trap_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode, old;
    CONTEXT *c = ep->ContextRecord;

    if (code == EXCEPTION_SINGLE_STEP && t_stepping) {
        t_stepping = 0;
        VirtualProtect(s_guestview, 0x1000, PAGE_READONLY, &old);
        c->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t f = ep->ExceptionRecord->ExceptionInformation[1];
        const uint8_t *ip = (const uint8_t *)c->Rip;

        if (f < (uintptr_t)s_guestview || f >= (uintptr_t)s_guestview + 0x1000)
            return EXCEPTION_CONTINUE_SEARCH;
        if (s_mode == 1) {
            if (!VirtualProtect(s_guestview, 0x1000, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            t_stepping = 1;
            c->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (ip[0] == 0x89 && ip[1] == 0x11) {          /* mov [rcx], edx */
            *(volatile uint32_t *)(s_hostview + (f - (uintptr_t)s_guestview)) = (uint32_t)c->Rdx;
            c->Rip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int run_writes(const char *name, volatile uint32_t *p, uint64_t *d)
{
    uint64_t total = 0;

    for (int i = 0; i < WRITES; i++) {
        uint64_t t0 = ticks();
        s_store(p, (uint32_t)i);
        d[i] = ticks() - t0;
        total += d[i];
    }
    if (*(volatile uint32_t *)(s_hostview + 0x410) != (uint32_t)(WRITES - 1)
            && p != (volatile uint32_t *)(s_hostview + 0x410)) {
        printf("  %s: the last write did not land\n", name);
        return 1;
    }
    report(name, d, WRITES, total);
    return 0;
}

static int trap_cost(void)
{
    static const uint8_t code[] = { 0x89, 0x11, 0xC3 };
    uint64_t *d = malloc(sizeof(uint64_t) * WRITES);
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, 0x1000, NULL);
    DWORD old;

    s_store = (store_fn)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!d || !sec || !s_store)
        return 1;
    memcpy((void *)s_store, code, sizeof code);
    s_guestview = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 0x1000);
    s_hostview = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, 0x1000);
    if (!s_guestview || !s_hostview || !AddVectoredExceptionHandler(1, trap_veh))
        return 1;

    printf("trapped write, %d writes each (per-write times include one QPC pair)\n",
           WRITES);
    run_writes("plain", (volatile uint32_t *)(s_hostview + 0x410), d);
    VirtualProtect(s_guestview, 0x1000, PAGE_READONLY, &old);
    s_mode = 1;
    run_writes("step", (volatile uint32_t *)(s_guestview + 0x410), d);
    s_mode = 2;
    run_writes("emulate", (volatile uint32_t *)(s_guestview + 0x410), d);
    free(d);
    return 0;
}

/* ---- wake latency ------------------------------------------------------- */

static HANDLE s_event;
static volatile LONG s_flag, s_done, s_quit;
static volatile uint64_t s_sent;
static uint64_t s_spin_ticks;
static uint64_t *s_wake;
static volatile LONG s_n;

static DWORD WINAPI sleeper(LPVOID unused)
{
    (void)unused;
    while (!s_quit) {
        uint64_t start = ticks();
        int woke = 0;

        while (ticks() - start < s_spin_ticks) {
            if (InterlockedExchange(&s_flag, 0)) {
                woke = 1;
                break;
            }
            YieldProcessor();
        }
        if (!woke) {
            WaitForSingleObject(s_event, INFINITE);
            if (!InterlockedExchange(&s_flag, 0))
                continue;               /* a stale signal */
        }
        if (s_quit)
            break;
        s_wake[s_n] = ticks() - s_sent;
        InterlockedIncrement(&s_n);
        InterlockedExchange(&s_done, 1);
    }
    return 0;
}

static void wake_latency(double spin_us)
{
    uint64_t total = 0;
    HANDLE t;

    s_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_spin_ticks = (uint64_t)(spin_us * (double)s_freq.QuadPart / 1e6);
    s_n = 0;
    s_quit = 0;
    s_flag = 0;
    t = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
    srand(1);
    for (int i = 0; i < WAKES; i++) {
        /* Gaps of 0-266 us: Halo's ~250 kickoffs a frame at 30 fps. */
        uint64_t gap = (uint64_t)((rand() % 267) * (double)s_freq.QuadPart / 1e6);
        uint64_t g0 = ticks();
        while (ticks() - g0 < gap)
            YieldProcessor();
        s_done = 0;
        s_sent = ticks();
        InterlockedExchange(&s_flag, 1);
        SetEvent(s_event);
        while (!s_done)
            YieldProcessor();
    }
    s_quit = 1;
    InterlockedExchange(&s_flag, 1);
    SetEvent(s_event);
    WaitForSingleObject(t, 1000);
    CloseHandle(t);
    CloseHandle(s_event);
    for (int i = 0; i < s_n; i++)
        total += s_wake[i];
    {
        char name[32];
        snprintf(name, sizeof name, "spin %.0f us", spin_us);
        report(name, s_wake, s_n, total);
    }
}

int main(void)
{
    QueryPerformanceFrequency(&s_freq);
    if (trap_cost())
        return 1;
    s_wake = malloc(sizeof(uint64_t) * WAKES);
    printf("\nwake latency, %d wakes each, gaps 0-266 us\n", WAKES);
    wake_latency(0);
    wake_latency(20);
    wake_latency(50);
    wake_latency(200);
    return 0;
}
