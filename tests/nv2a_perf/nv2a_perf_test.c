/* Where a frame's time goes (nv2a_perf.h), with a clock set by hand. */
#include "nv2a_perf.h"
#include <stdio.h>

static int s_bad;
#define CHECK(c) do { if (!(c)) { s_bad++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t s_now = 5000000000ull;
static unsigned s_asked;

uint64_t xbox_HostNowNs(void)
{
    s_asked++;
    return s_now;
}

static void pass(double ms) { s_now += (uint64_t)(ms * 1e6 + 0.5); }
static int near_to(double a, double b) { return a - b < 1e-3 && b - a < 1e-3; }

static void in_phase(int phase, double ms)
{
    int was = nv2a_perf_phase(phase);

    pass(ms);
    nv2a_perf_phase(was);
}

static void frame(double ms)
{
    in_phase(NV2A_PERF_WORK, ms);
    nv2a_perf_frame();
}

static Nv2aPerfStats stats(double ms)
{
    Nv2aPerfStats s;

    nv2a_perf_stats(ms, &s);
    return s;
}

static void test_off(void)
{
    int i;

    for (i = 0; i < 10; i++)
        frame(16.0);
    CHECK(s_asked == 0);                /* nothing is timed */
    CHECK(nv2a_perf_frame_open() == 0 && stats(1000).frames == 0);
    nv2a_perf_gpu(0, 1.0);
    nv2a_perf_gpu(1, 1.0);
}

static void test_phases(void)
{
    Nv2aPerfStats s;
    double sum = 0.0;
    int k, was;

    nv2a_perf_set(1);
    frame(50.0);                        /* the one that starts it is not one */
    CHECK(stats(1000).frames == 0 && stats(1000).avg == 0.0);

    /* One within another: each has its own time only. */
    in_phase(NV2A_PERF_TITLE, 3.0);
    in_phase(NV2A_PERF_HOLD, 2.0);
    was = nv2a_perf_phase(NV2A_PERF_WORK);
    CHECK(was == NV2A_PERF_TITLE);
    pass(4.0);
    was = nv2a_perf_phase(NV2A_PERF_BACK);
    CHECK(was == NV2A_PERF_WORK);
    pass(0.25);
    in_phase(NV2A_PERF_GPU, 1.5);
    pass(0.25);
    nv2a_perf_phase(was);
    in_phase(NV2A_PERF_PIPELINES, 0.75);
    in_phase(NV2A_PERF_NEW_TEXTURES, 0.5);
    pass(1.0);
    nv2a_perf_frame();                  /* within a phase, which goes on */
    s = stats(1000);
    CHECK(s.frames == 1 && near_to(s.avg, 13.25) && near_to(s.ms, 13.25));
    CHECK(near_to(s.phase[NV2A_PERF_TITLE], 3) && near_to(s.phase[NV2A_PERF_HOLD], 2));
    CHECK(near_to(s.phase[NV2A_PERF_WORK], 5) && near_to(s.phase[NV2A_PERF_BACK], 0.5));
    CHECK(near_to(s.phase[NV2A_PERF_GPU], 1.5));
    CHECK(near_to(s.phase[NV2A_PERF_PIPELINES], 0.75));
    CHECK(near_to(s.phase[NV2A_PERF_NEW_TEXTURES], 0.5));
    CHECK(near_to(s.once_worst, 1.25) && s.once_frames == 1);
    for (k = 0; k < NV2A_PERF_PHASES; k++)
        sum += s.phase[k];
    CHECK(near_to(sum, s.avg));

    pass(2.0);
    nv2a_perf_phase(NV2A_PERF_TITLE);
    pass(4.75);
    nv2a_perf_frame();
    s = stats(1000);
    CHECK(s.frames == 2 && near_to(s.avg, 10) && near_to(s.phase[NV2A_PERF_WORK], 3.5));
    CHECK(near_to(s.phase[NV2A_PERF_TITLE], 3.875));
    CHECK(near_to(s.lo, 6.75) && near_to(s.hi, 13.25) && near_to(s.jitter, 6.5));
    CHECK(s.once_frames == 1);
    CHECK(stats(6).frames == 1 && near_to(stats(6).avg, 6.75));     /* the latest */

    /* The same phase again reads no clock. */
    k = (int)s_asked;
    in_phase(NV2A_PERF_TITLE, 1.0);
    CHECK((int)s_asked == k);
    nv2a_perf_frame();
}

static void test_parts(void)
{
    Nv2aPerfStats s;
    int i, was;

    nv2a_perf_set(0);
    frame(1.0);
    nv2a_perf_set(1);
    frame(1.0);
    nv2a_perf_phase(NV2A_PERF_WORK);
    was = nv2a_perf_part(NV2A_PERF_DRAWS);
    CHECK(was == NV2A_PERF_COMMANDS);
    for (i = 0; i < 6; i++)
        nv2a_perf_sample();
    was = nv2a_perf_part(NV2A_PERF_TEXTURES);
    CHECK(was == NV2A_PERF_DRAWS);
    for (i = 0; i < 3; i++)
        nv2a_perf_sample();
    nv2a_perf_phase(NV2A_PERF_GPU);     /* a wait is no part of the work */
    for (i = 0; i < 100; i++)
        nv2a_perf_sample();
    nv2a_perf_phase(NV2A_PERF_WORK);
    nv2a_perf_part(NV2A_PERF_COMMANDS);
    nv2a_perf_sample();
    pass(20.0);
    nv2a_perf_frame();
    s = stats(1000);
    CHECK(s.frames == 1 && s.samples == 10 && near_to(s.phase[NV2A_PERF_WORK], 20));
    CHECK(near_to(s.part[NV2A_PERF_DRAWS], 12) && near_to(s.part[NV2A_PERF_TEXTURES], 6));
    CHECK(near_to(s.part[NV2A_PERF_COMMANDS], 2) && s.part[NV2A_PERF_VERTICES] == 0.0);
    /* Another, seen in one place only. */
    nv2a_perf_part(NV2A_PERF_VERTICES);
    for (i = 0; i < 10; i++)
        nv2a_perf_sample();
    pass(10.0);
    nv2a_perf_frame();
    nv2a_perf_part(NV2A_PERF_COMMANDS);
    s = stats(1000);
    CHECK(s.frames == 2 && s.samples == 20 && near_to(s.part[NV2A_PERF_VERTICES], 7.5));
    nv2a_perf_phase(NV2A_PERF_TITLE);
    nv2a_perf_set(0);
    nv2a_perf_frame();
}

static void test_gpu(void)
{
    uint32_t a, b;
    Nv2aPerfStats s;

    nv2a_perf_set(1);
    frame(1.0);
    a = nv2a_perf_frame_open();
    frame(10.0);
    b = nv2a_perf_frame_open();
    CHECK(a && b == a + 1);
    s = stats(1000);
    CHECK(s.gpu_frames == 0 && s.gpu == 0.0);
    nv2a_perf_gpu(a, 3.0);
    nv2a_perf_gpu(a, 1.0);              /* in two pieces */
    nv2a_perf_gpu(b, 2.0);              /* before the frame is over */
    s = stats(1000);
    CHECK(s.gpu_frames == 1 && near_to(s.gpu, 4));
    frame(10.0);
    nv2a_perf_gpu(b, 4.0);
    nv2a_perf_gpu(0, 100.0);
    nv2a_perf_gpu(b + 2, 100.0);        /* no such frame yet */
    nv2a_perf_gpu(a - 1, 100.0);        /* from before */
    frame(10.0);                        /* and one that stays unknown */
    s = stats(1000);
    CHECK(s.frames == 3 && s.gpu_frames == 2 && near_to(s.gpu, 5));
    nv2a_perf_set(0);
    frame(1.0);
    nv2a_perf_gpu(a, 100.0);
    CHECK(nv2a_perf_frame_open() == 0);
    nv2a_perf_set(1);
    frame(1.0);
    frame(10.0);
    nv2a_perf_gpu(a, 100.0);            /* timed, then not, then again */
    s = stats(1000);
    CHECK(s.frames == 1 && s.gpu_frames == 0);
}

static void test_pace(void)
{
    Nv2aPerfStats s;
    int i;

    nv2a_perf_set(0);
    frame(1.0);
    nv2a_perf_set(1);
    frame(1.0);
    for (i = 0; i < 30; i++)
        frame(i == 7 ? 33.3 : i & 1 ? 16.2 : 17.1);
    s = stats(10000);
    CHECK(near_to(s.target, 1000.0 / 60) && s.hitches == 1 && near_to(s.hi, 33.3));
    for (i = 0; i < 100; i++)
        frame(i == 50 ? 49.0 : 33.0);
    s = stats(10000);
    CHECK(near_to(s.target, 2000.0 / 60) && s.hitches == 0);
    for (i = 0; i < 200; i++)
        frame(i == 50 ? 7.6 : 5.0);     /* not paced: what it is */
    s = stats(900);
    CHECK(near_to(s.target, 5.0) && s.hitches == 1);

    /* More than are kept. */
    for (i = 0; i < 5000; i++)
        frame(i < 4000 ? 4.0 : 2.0);
    s = stats(1e9);
    CHECK(s.frames == 2048 && near_to(s.avg, (1000 * 2.0 + 1048 * 4.0) / 2048));
    {
        static Nv2aPerfFrame f[3000];
        Nv2aPerfStats t;

        CHECK(nv2a_perf_frames(f, 3000) == 2048);
        CHECK(near_to(f[2047].ms, 2) && near_to(f[1048].ms, 2) && near_to(f[1047].ms, 4));
        nv2a_perf_sum(f, 2048, 1e9, &t);
        CHECK(t.frames == s.frames && t.avg == s.avg && t.target == s.target);
        CHECK(nv2a_perf_frames(f, 1001) == 1001);       /* the latest, oldest first */
        CHECK(near_to(f[0].ms, 4) && near_to(f[1].ms, 2) && near_to(f[1000].ms, 2));
    }
    nv2a_perf_gpu(nv2a_perf_frame_open() - 2049, 100.0);
    CHECK(stats(1e9).gpu_frames == 0);
    nv2a_perf_gpu(nv2a_perf_frame_open() - 2048, 1.0);
    CHECK(stats(1e9).gpu_frames == 1 && near_to(stats(1e9).gpu, 1.0));
}

/* RECOMP_PERF_LOG: timed whether or not anything else asks. */
static int test_log(void)
{
    nv2a_perf_set(0);
    frame(1.0);
    frame(10.0);
    nv2a_perf_set(0);
    frame(10.0);
    CHECK(stats(1000).frames == 2 && nv2a_perf_frame_open());
    return s_bad != 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
        return test_log();
    test_off();
    test_phases();
    test_parts();
    test_gpu();
    test_pace();
    printf("%s\n", s_bad ? "failed" : "passed");
    return s_bad != 0;
}
