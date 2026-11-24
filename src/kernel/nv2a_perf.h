/*
 * nv2a_perf.h - where a frame's time goes, seen from the thread that carries
 * out the title's commands. A frame ends at each flip.
 *
 * Phases are timed, and add up to the frame: they change a few times a frame.
 * Parts split NV2A_PERF_WORK; they change thousands of times a frame, so the
 * code says which one it is in (a store) and another thread looks every half
 * millisecond. One frame's parts are rough, a second's are not.
 *
 * Off, nothing is timed and no thread runs. RECOMP_PERF_LOG=<s> keeps it on
 * and writes a [PERF] line every s seconds.
 */
#ifndef NV2A_PERF_H
#define NV2A_PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NV2A_PERF_TITLE = 0,        /* nothing to do: the title sent no more */
    NV2A_PERF_HOLD,             /* a flip is held for its vblank */
    NV2A_PERF_WORK,
    NV2A_PERF_PIPELINES,        /* work to be done once and not in a frame: */
    NV2A_PERF_NEW_TEXTURES,     /* making a pipeline, making a texture */
    NV2A_PERF_GPU,              /* waiting for it */
    NV2A_PERF_BACK,             /* copying pictures to guest memory */
    NV2A_PERF_PHASES
} Nv2aPerfPhase;

typedef enum {
    NV2A_PERF_COMMANDS = 0,     /* reading them, and keeping their state */
    NV2A_PERF_DRAWS,
    NV2A_PERF_VERTICES,
    NV2A_PERF_TEXTURES,
    NV2A_PERF_PARTS
} Nv2aPerfPart;

/* Private to the functions below. */
extern volatile int nv2a_perf_phase_is, nv2a_perf_part_is, nv2a_perf_timing;
void nv2a_perf_phase_ends(void);

/* Both from the thread that carries out the commands only, and both return
 * what it was, to be put back. */
static inline int nv2a_perf_phase(int phase)
{
    int was = nv2a_perf_phase_is;

    if (was != phase) {
        if (nv2a_perf_timing)
            nv2a_perf_phase_ends();
        nv2a_perf_phase_is = phase;
    }
    return was;
}

static inline int nv2a_perf_part(int part)
{
    int was = nv2a_perf_part_is;

    nv2a_perf_part_is = part;
    return was;
}

/* At each flip, on that thread. */
void nv2a_perf_frame(void);

/* The frame being timed, to tag GPU work with; 0: none. On that thread. */
uint32_t nv2a_perf_frame_open(void);

/* GPU time of a frame, once known, on that thread; it adds up. */
void nv2a_perf_gpu(uint32_t frame, double ms);

/* On or off, from the next flip; the log keeps it on. Any thread. */
void nv2a_perf_set(int on);

/* Looks where the thread is. The sampling thread's. */
void nv2a_perf_sample(void);

typedef struct {
    float ms, phase[NV2A_PERF_PHASES];
    float gpu;                          /* below 0: not known yet */
    uint32_t part[NV2A_PERF_PARTS];     /* samples */
} Nv2aPerfFrame;

/* The latest frames, the last one last: at most `room` and NV2A_PERF_KEPT of
 * them. Returns how many. Any thread. */
#define NV2A_PERF_KEPT 2048u
uint32_t nv2a_perf_frames(Nv2aPerfFrame *out, uint32_t room);

typedef struct {
    uint32_t frames;            /* 0: none yet, and nothing below is set */
    double ms;                  /* they took */
    double avg, lo, hi;         /* ms a frame */
    double jitter;              /* how much a frame differs from the one before */
    double target;              /* what most take: whole vblanks if near them */
    uint32_t hitches;           /* took half as long again, or more */
    double phase[NV2A_PERF_PHASES];     /* ms a frame */
    double part[NV2A_PERF_PARTS];       /* the same: they add up to WORK */
    uint32_t samples;           /* that the parts rest on */
    double gpu;                 /* ms a frame */
    uint32_t gpu_frames;        /* it is known for */
    double once_worst;          /* the most once-only work in a frame, ms */
    uint32_t once_frames;       /* with over 1 ms of it */
} Nv2aPerfStats;

/* Over the latest of these that took `ms` together. Any thread. */
void nv2a_perf_sum(const Nv2aPerfFrame *f, uint32_t n, double ms, Nv2aPerfStats *out);

/* The same of the frames that are kept. One thread at a time. */
void nv2a_perf_stats(double ms, Nv2aPerfStats *out);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_PERF_H */
