/*
 * NV2A render back ends: batches as the title sent them (optional).
 *
 * Nv2aBackend (nv2a_backend.h) hands over triangles the executor has already
 * transformed and clipped, with one colour and one texture. A back end that
 * transforms vertices itself, or needs every texture stage, the combiners or
 * the surface's format, registers this one as well and gets each batch before
 * any of that: where the vertices are, the indices, and the state as the
 * title programmed it.
 *
 * The two work together. A batch or clear this back end refuses (by returning
 * 0) takes the path it would have taken without it: Nv2aBackend's callbacks
 * if one is registered, the executor's own drawing otherwise. flip and
 * present_sync stay with Nv2aBackend.
 *
 * Its callbacks run on the executor's thread, one call at a time, and the
 * next command waits for them. Every pointer is valid for the call only. Once
 * it returns, the title may be told the GPU is past the batch and reuse its
 * vertices and textures, so a back end that records for later copies what it
 * has not read yet.
 */
#ifndef NV2A_BACKEND_BATCH_H
#define NV2A_BACKEND_BATCH_H

#include <stdint.h>
#include "nv2a_backend.h"
#include "nv2a_combiner.h"
#include "nv2a_vsh_interp.h"

/* Goes up with every change to the structures below. */
#define NV2A_BATCH_VERSION 3u

/* What is being drawn into. Unlike Nv2aSurface's, the fields needed to find a
 * pixel in memory are all here. */
typedef struct {
    uint32_t color_va, zeta_va;         /* guest addresses; 0: none bound */
    uint32_t color_pitch, zeta_pitch;   /* bytes per row */
    uint32_t format;                    /* SET_SURFACE_FORMAT as sent: colour
                                         * 0-3, zeta 4-7, type 8-11 (2 is
                                         * swizzled), anti-aliasing 12-15,
                                         * log2 width 16-23, height 24-31 */
    uint32_t bytes_per_pixel;           /* of the colour surface */
    uint32_t clip_x, clip_y, clip_w, clip_h;   /* real (anti-aliased) pixels */
    uint32_t aa_sx, aa_sy;              /* 1 or 2 each */
} Nv2aTarget;

/* One vertex attribute. size 0: no array, every vertex reads `current`. */
typedef struct {
    uint32_t offset;                    /* of element 0: a guest address, or
                                         * bytes into inline_data */
    uint32_t type, size, stride;        /* SET_VERTEX_DATA_ARRAY_FORMAT's:
                                         * type 0 D3DCOLOR, 1 s16 normalised,
                                         * 2 float, 4 u8 normalised, 5 s16,
                                         * 6 packed 11:11:10 */
    float    current[4];
} Nv2aBatchAttr;

typedef struct {
    Nv2aTexture t;                      /* offset, size, format, wrap, levels */
    uint32_t    palette;                /* guest address of the CLUT */
    uint32_t    border;                 /* SET_TEXTURE_BORDER_COLOR */
    int         border_color;           /* BORDER reads `border` */
    int         cube;
    int         valid;                  /* 0: nothing the stage could sample */
} Nv2aBatchTexture;

typedef enum {
    NV2A_TRANSFORM_PROGRAM = 0,         /* the vertex program below */
    NV2A_TRANSFORM_FIXED,               /* fixed function: matrices and
                                         * lighting are in `regs` */
    NV2A_TRANSFORM_NONE                 /* positions are logical pixels */
} Nv2aTransform;

typedef struct {
    uint32_t        prim;               /* SET_BEGIN_END: 1 points .. 10
                                         * polygon */
    const uint32_t *indices;
    uint32_t        index_count;        /* 1 or more */
    const Nv2aBatchAttr *attrs;         /* 16 */
    const void     *inline_data;        /* NULL: attribute offsets are guest
                                         * addresses */
    uint32_t        inline_bytes;

    Nv2aTransform   transform;
    /* Vertex program memory and constants, whole. A revision changes whenever
     * the contents may have; equal revisions mean equal contents. */
    const uint32_t (*program)[4];       /* NV2A_VSH_SLOTS */
    uint32_t        program_start;
    uint32_t        program_revision;
    const float   (*constants)[4];      /* NV2A_VSH_CONSTANTS */
    uint32_t        constants_revision;

    const Nv2aBatchTexture *textures;   /* 4 */
    const Nv2aCombiner     *combiner;   /* NULL: the title never set them up */
    const Nv2aRenderState  *state;
    const Nv2aTarget       *target;

    /* The 3D class's methods 0x0000-0x1FFC, the last value sent to each,
     * indexed by method / 4: whatever has no field above. */
    const uint32_t *regs;
} Nv2aRawBatch;

typedef struct {
    uint32_t flags;                     /* CLEAR_SURFACE: Z 0x1, stencil 0x2,
                                         * colour 0xF0 */
    uint32_t argb, zstencil;            /* the clear values as sent */
    int      rect_set;                  /* the rectangle below was sent */
    uint32_t rect_h, rect_v;            /* SET_CLEAR_RECT_*: first | last << 16,
                                         * inclusive, as sent */
    const uint32_t *regs;               /* Nv2aRawBatch's */
} Nv2aRawClear;

/* A visibility test's answer, as the title asked for it (GET_REPORT). */
typedef struct {
    uint32_t va;                        /* 16 bytes of guest memory */
    uint64_t time;
    uint32_t pixels;                    /* counted by the executor's own drawing
                                         * since the last CLEAR_REPORT_VALUE */
} Nv2aReport;

typedef struct {
    uint32_t version;                   /* NV2A_BATCH_VERSION */
    /* Each may be NULL. Nonzero: done. */
    int (*draw)(const Nv2aRawBatch *b);
    int (*clear)(const Nv2aTarget *t, const Nv2aRenderState *rs,
                 const Nv2aRawClear *c);
    /* Visibility tests: pixels that pass every test of batches drawn while
     * regs[0x17CC / 4] (SET_ZPASS_PIXEL_COUNT_ENABLE) is not 0. Clears do not
     * count.
     * report_clear: the count starts at 0 again.
     * report: nonzero if the back end answers, with nv2a_batch_report() on any
     * thread, once it has the count: pixels plus its own. Until then the title
     * sees the test unfinished, and it may wait. 0: answered by the executor,
     * with pixels.
     * idle: a report was taken, and every command the title sent so far has
     * been carried out. It may be waiting for the answer. */
    void (*report_clear)(void);
    int (*report)(const Nv2aReport *r);
    void (*idle)(void);
    /* The title freed contiguous memory: whatever the back end keeps by these
     * guest addresses (textures, surfaces) is no longer what is there. Called
     * on the thread that freed it, any thread, while others may be drawing. */
    void (*memory_freed)(uint32_t va, uint32_t bytes);
} Nv2aBatchBackend;

/* Register (or, with NULL, remove) the back end; as nv2a_backend_register, to
 * be called before the title submits work. Returns 0, changing nothing, for
 * another version. */
int nv2a_backend_register_batches(const Nv2aBatchBackend *backend);

/* For draw() only: transform vertices on the CPU, as the executor does for its
 * own drawing. out[i] is vertex first + i of the batch's index list: position
 * in real surface pixels with the clip w left in pos[3], unclipped (w may be
 * negative), colours and texture coordinates as the vertex stage put them out.
 * Returns 0 if a vertex could not be transformed. */
int nv2a_batch_transform(uint32_t first, uint32_t count, Nv2aVshOutput *out);

/* As above, for a fixed-function batch's positions before the transform. */
int nv2a_batch_positions(uint32_t first, uint32_t count, float (*out)[4]);

/* Writes a report: `to` is the host's address of it. */
void nv2a_batch_report(void *to, uint64_t time, uint32_t pixels);

#endif /* NV2A_BACKEND_BATCH_H */
