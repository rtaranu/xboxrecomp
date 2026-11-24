/* Where a frame's time goes (nv2a_perf.h). */
#include "nv2a_perf.h"
#include "platform/host_time.h"   /* xbox_HostNowNs */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <pthread.h>
#include <time.h>
#endif

volatile int nv2a_perf_phase_is = NV2A_PERF_TITLE;
volatile int nv2a_perf_part_is = NV2A_PERF_COMMANDS;
volatile int nv2a_perf_timing;

#if defined(_WIN32)
static SRWLOCK s_lock = SRWLOCK_INIT;
static void lock(void) { AcquireSRWLockExclusive(&s_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&s_lock); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
#endif

/* Written by the thread that carries out the commands, locked against the
 * ones that read: the frames and how many there are. */
#define KEPT NV2A_PERF_KEPT
static Nv2aPerfFrame s_rec[KEPT];
static uint32_t s_count;                /* frames closed; the last is count - 1 */
static uint32_t s_first;                /* the first since timing started */
static uint64_t s_frame_ns, s_phase_ns; /* when each began */
static uint64_t s_spent[NV2A_PERF_PHASES];
static float s_gpu;                     /* known already of the open frame */
static uint32_t s_shown[KEPT];          /* frames, by number, locked as above, */
static float s_shown_ms[KEPT];          /* that were shown, so long after one */
static uint64_t s_shown_ns;             /* the last was; its thread's */
static volatile int s_want;             /* nv2a_perf_set() */
static double s_log_ms;                 /* a line every so many; 0: none */
static double s_logged_ms;              /* since the last */

/* The sampling thread's, and what they were when the open frame began. */
static volatile uint32_t s_seen[NV2A_PERF_PARTS];
static uint32_t s_seen_was[NV2A_PERF_PARTS];

void nv2a_perf_sample(void)
{
    int part = nv2a_perf_part_is;

    if (nv2a_perf_phase_is == NV2A_PERF_WORK)
        s_seen[part] = s_seen[part] + 1;
}

#define SAMPLE_NS 500000
#if defined(NV2A_PERF_NO_SAMPLER)       /* its test takes the samples */
static void start_sampler(void) {}
#elif defined(_WIN32)
static DWORD WINAPI sampler(LPVOID unused)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);

    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);     /* it hardly runs */
    for (;;) {
        LARGE_INTEGER due;

        due.QuadPart = -SAMPLE_NS / 100;
        if (!nv2a_perf_timing)
            Sleep(50);
        else if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(1);
        nv2a_perf_sample();
    }
}

static void start_sampler(void)
{
    HANDLE th = CreateThread(NULL, 0, sampler, NULL, 0, NULL);

    if (th)
        CloseHandle(th);
}
#else
static void *sampler(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec ts = { 0, nv2a_perf_timing ? SAMPLE_NS : 50000000 };

        nanosleep(&ts, NULL);
        nv2a_perf_sample();
    }
    return NULL;
}

static void start_sampler(void)
{
    pthread_t th;

    if (pthread_create(&th, NULL, sampler, NULL) == 0)
        pthread_detach(th);
}
#endif

void nv2a_perf_set(int on) { s_want = on != 0; }

void nv2a_perf_phase_ends(void)
{
    uint64_t now = xbox_HostNowNs();

    s_spent[nv2a_perf_phase_is] += now - s_phase_ns;
    s_phase_ns = now;
}

static void frame_begins(uint64_t now)
{
    int k;

    s_frame_ns = now;
    memset(s_spent, 0, sizeof s_spent);
    for (k = 0; k < NV2A_PERF_PARTS; k++)
        s_seen_was[k] = s_seen[k];
    s_gpu = -1.0f;
}

static void log_line(void)
{
    Nv2aPerfStats s;

    nv2a_perf_stats(s_log_ms, &s);
    if (!s.frames)
        return;
    fprintf(stderr, "[PERF] %.1f fps over %.1f s: %.2f ms [%.2f %.2f], jitter %.2f, %u over"
            " 1.5 x %.2f; work %.2f: commands %.2f, draws %.2f, vertices %.2f, textures %.2f;"
            " once only: pipelines %.2f, new textures %.2f, worst %.2f, %u frames over 1 ms;"
            " waits: title %.2f, flip hold %.2f, gpu %.2f, copying back %.2f; gpu %.2f",
            1000.0 / s.avg, s.ms / 1000.0, s.avg, s.lo, s.hi, s.jitter, s.hitches, s.target,
            s.phase[NV2A_PERF_WORK], s.part[NV2A_PERF_COMMANDS], s.part[NV2A_PERF_DRAWS],
            s.part[NV2A_PERF_VERTICES], s.part[NV2A_PERF_TEXTURES],
            s.phase[NV2A_PERF_PIPELINES], s.phase[NV2A_PERF_NEW_TEXTURES], s.once_worst,
            s.once_frames, s.phase[NV2A_PERF_TITLE], s.phase[NV2A_PERF_HOLD],
            s.phase[NV2A_PERF_GPU], s.phase[NV2A_PERF_BACK], s.gpu);
    if (s.shown.n)
        fprintf(stderr, "; shown: %.2f ms [%.2f %.2f], jitter %.2f, %u over 1.5 x %.2f,"
                " %u skipped", s.shown.avg, s.shown.lo, s.shown.hi, s.shown.jitter,
                s.shown.hitches, s.shown.target, s.skipped);
    fprintf(stderr, "\n");
}

void nv2a_perf_frame(void)
{
    static int asked, sampling;
    Nv2aPerfFrame *r;
    int k;

    if (!asked) {
        const char *e = getenv("RECOMP_PERF_LOG");

        asked = 1;
        s_log_ms = e ? atof(e) * 1000.0 : 0.0;
    }
    if (!s_want && !(s_log_ms > 0.0)) {
        nv2a_perf_timing = 0;
        return;
    }
    if (!nv2a_perf_timing) {            /* the first frame begins here */
        if (!sampling)
            start_sampler();
        sampling = 1;
        lock();
        s_first = s_count;
        s_shown[s_count % KEPT] = 0;    /* of the frame that was left open */
        unlock();
        s_logged_ms = 0.0;
        s_phase_ns = xbox_HostNowNs();
        frame_begins(s_phase_ns);
        nv2a_perf_timing = 1;
        return;
    }
    nv2a_perf_phase_ends();             /* and goes on in the next frame */
    lock();
    r = &s_rec[s_count % KEPT];
    r->ms = (float)((double)(s_phase_ns - s_frame_ns) / 1e6);
    for (k = 0; k < NV2A_PERF_PHASES; k++)
        r->phase[k] = (float)((double)s_spent[k] / 1e6);
    for (k = 0; k < NV2A_PERF_PARTS; k++)
        r->part[k] = s_seen[k] - s_seen_was[k];
    r->gpu = s_gpu;
    s_count++;
    unlock();
    frame_begins(s_phase_ns);
    s_logged_ms += r->ms;
    if (s_log_ms > 0.0 && s_logged_ms >= s_log_ms) {
        s_logged_ms = 0.0;
        log_line();
    }
}

uint32_t nv2a_perf_frame_open(void) { return nv2a_perf_timing ? s_count + 1 : 0; }

void nv2a_perf_gpu(uint32_t frame, double ms)
{
    float *to;

    if (frame <= s_first || s_count + 1 - frame > KEPT)     /* or still to come */
        return;
    lock();
    to = frame == s_count + 1 ? &s_gpu : &s_rec[(frame - 1) % KEPT].gpu;
    *to = (*to < 0.0f ? 0.0f : *to) + (float)ms;
    unlock();
}

void nv2a_perf_shown(uint32_t frame)
{
    uint64_t now;

    if (!frame) {
        s_shown_ns = 0;
        return;
    }
    now = xbox_HostNowNs();
    if (s_shown_ns) {
        lock();
        s_shown[(frame - 1) % KEPT] = frame;
        s_shown_ms[(frame - 1) % KEPT] = (float)((double)(now - s_shown_ns) / 1e6);
        unlock();
    }
    s_shown_ns = now;
}

static int by_size(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

#define VBLANK_MS (1000.0 / 60.0)

uint32_t nv2a_perf_frames(Nv2aPerfFrame *out, uint32_t room)
{
    uint32_t n, i;

    lock();
    n = s_count - s_first;
    if (n > KEPT)
        n = KEPT;
    if (n > room)
        n = room;
    for (i = 0; i < n; i++) {
        uint32_t k = (s_count - n + i) % KEPT;

        out[i] = s_rec[k];
        out[i].shown = s_shown[k] == s_count - n + i + 1 ? s_shown_ms[k] : -1.0f;
    }
    unlock();
    return n;
}

void nv2a_perf_stats(double ms, Nv2aPerfStats *out)
{
    static Nv2aPerfFrame f[KEPT];

    nv2a_perf_sum(f, nv2a_perf_frames(f, KEPT), ms, out);
}

/* Of `n` times, the latest first, at least one; left in order of size. */
static void pace(float *t, uint32_t n, Nv2aPerfPace *out)
{
    double sum = 0.0, step = 0.0;
    uint32_t i;

    out->n = n;
    out->lo = out->hi = t[0];
    for (i = 0; i < n; i++) {
        sum += t[i];
        if (t[i] < out->lo)
            out->lo = t[i];
        if (t[i] > out->hi)
            out->hi = t[i];
        if (i)
            step += t[i] > t[i - 1] ? t[i] - t[i - 1] : t[i - 1] - t[i];
    }
    out->avg = sum / n;
    out->jitter = n > 1 ? step / (n - 1) : 0.0;
    qsort(t, n, sizeof t[0], by_size);
    out->target = t[n / 2];
    if (out->target > VBLANK_MS * 0.75)
        out->target = VBLANK_MS * (double)(uint32_t)(out->target / VBLANK_MS + 0.5);
    out->hitches = 0;
    for (i = 0; i < n; i++)
        out->hitches += t[i] >= out->target * 1.5;
}

void nv2a_perf_sum(const Nv2aPerfFrame *f, uint32_t have, double ms, Nv2aPerfStats *out)
{
    float took[KEPT], shown[KEPT];
    uint32_t n = 0, seen = 0, unseen = 0;
    uint32_t part[NV2A_PERF_PARTS] = {0};
    Nv2aPerfPace all;
    double gpu = 0.0;
    int k;

    memset(out, 0, sizeof *out);
    if (have > KEPT)
        have = KEPT;
    while (n < have && out->ms < ms) {
        const Nv2aPerfFrame *r = &f[have - 1 - n];
        double once = r->phase[NV2A_PERF_PIPELINES] + r->phase[NV2A_PERF_NEW_TEXTURES];

        took[n++] = r->ms;
        if (r->shown >= 0.0f) {
            out->skipped += unseen;
            unseen = 0;
            shown[seen++] = r->shown;
        } else if (seen) {              /* not those still to be */
            unseen++;
        }
        out->ms += r->ms;
        for (k = 0; k < NV2A_PERF_PHASES; k++)
            out->phase[k] += r->phase[k];
        for (k = 0; k < NV2A_PERF_PARTS; k++) {
            part[k] += r->part[k];
            out->samples += r->part[k];
        }
        if (r->gpu >= 0.0f) {
            gpu += r->gpu;
            out->gpu_frames++;
        }
        if (once > out->once_worst)
            out->once_worst = once;
        out->once_frames += once > 1.0;
    }
    if (!n || out->ms <= 0.0) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->frames = n;
    pace(took, n, &all);
    out->avg = all.avg;
    out->lo = all.lo;
    out->hi = all.hi;
    out->jitter = all.jitter;
    out->target = all.target;
    out->hitches = all.hitches;
    if (seen)
        pace(shown, seen, &out->shown);
    for (k = 0; k < NV2A_PERF_PHASES; k++)
        out->phase[k] /= n;
    for (k = 0; k < NV2A_PERF_PARTS && out->samples; k++)
        out->part[k] = out->phase[NV2A_PERF_WORK] * part[k] / out->samples;
    out->gpu = out->gpu_frames ? gpu / out->gpu_frames : 0.0;
}
