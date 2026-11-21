/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * Register-combiner state and texture stages 1-3 are not passed on: a back end
 * sees stage 0 and the diffuse colour. One that needs them, or the batch before
 * transform, registers an Nv2aBatchBackend too (nv2a_backend_batch.h).
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_flip_hold.h"   /* XboxFlipIntervalPolicy */

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
} Nv2aBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* What a flip hands a back end that shows the picture itself. The executor
 * fills it and may add fields at the end; it is good for the call only. */
typedef struct Nv2aPresent {
    uint32_t va;        /* the surface to show; 0: none */
    uint32_t pitch;
    uint32_t aa_sy;     /* its vertical anti-aliasing factor, 1 or 2 */
    /* RECOMP_FLIP_PACING: the vblank (xbox_VblankCount) the flip is due on,
     * see xbox_FlipHoldDue. The picture belongs on that vblank, or at once
     * when the count has reached it. has_due 0: show it at once (pacing off,
     * the first flip, IMMEDIATE, the edge rule). */
    int      has_due;
    uint32_t due;
} Nv2aPresent;

/* For a back end that has the picture itself and does not wait for the GPU at
 * a flip. `present` is called at each flip instead of present_sync. Nonzero:
 * the back end shows the picture itself (xbox_FramebufferWindowSetExternal),
 * and guest memory may lack it. 0: guest memory has it, and the window takes
 * its copy there. Called on the executor thread, like present_sync, and
 * returns without waiting for the vblank: the wait is the back end's own.
 * present_sync is then called before a framebuffer dump instead, to put what
 * was drawn into guest memory. NULL removes it, and so does registering. */
void nv2a_backend_set_present(int (*present)(const Nv2aPresent *p));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* The KEVENT XDK D3D waits on after patching software method 5 into the
 * pushbuffer (its fence wait when the GPU is far behind): the miniport's event
 * at miniport + 0x1A4, which D3D's interrupt handler sets for that method.
 * The executor sets it when it executes the method. The title supplies the
 * address, as for the semaphore. */
void nv2a_pb_set_wake_event(uint32_t guest_va);

/* Nonzero: draw nothing, for a host renderer that draws from the title's
 * API calls. The pushbuffer is still walked (fences, semaphores, notifiers,
 * flips); clears, batches and the flip's copy to the window are skipped. */
void nv2a_pb_set_draws_off(int off);

/* For a hook in front of D3D's KickOff: sleep while a flip is held
 * (RECOMP_FLIP_PACING), at most XBOX_FLIP_HOLD_MAX_NS. A held flip leaves
 * the PFB flush KickOff spins on unacknowledged until the vblank, so a
 * title entering KickOff then would spin a host core for the rest of the
 * frame; waiting here first ends at the same release. Returns at once with
 * no hold, or with RECOMP_KICK_HOLD_WAIT_US=0 (the sleep step, default
 * 200 us).
 *
 * It then announces the kickoff, so the GPU side answers its PFB flush as
 * soon as KickOff sets it instead of on its next pass (RECOMP_KICK_SPIN_US,
 * default 200; 0 turns this off). */
void nv2a_pb_flip_hold_wait(void);

/* For a backend that presents on the title's thread (in its Swap): wait
 * until the vblank the frame is due on (RECOMP_FLIP_PACING; see
 * xbox_FlipHoldDue), at most XBOX_FLIP_HOLD_MAX_NS, then present. Without
 * it the present lands whenever the title reaches its Swap, which the hold
 * leaves anywhere in the frame after the vblank. Returns 1 if it waited, 0
 * at once with pacing off, no flip to measure from, IMMEDIATE, or the
 * vblank already reached. */
int nv2a_pb_flip_present_wait(void);

/* The title's say in how long a flip is held (RECOMP_FLIP_PACING; see
 * docs/technical/flip-interval.md), for game code that knows when its frames
 * can run faster than its swaps ask (gameplay timed on the clock) and when
 * not (movies and logic stepped per frame).
 *
 * xbox_FlipIntervalMaxSet: hold a swap asking for more than max vblanks
 * (0-3, more reads as 3) for max; 0 holds each as asked. Any thread, from
 * the next flip.
 * xbox_FlipIntervalMaxGet: the cap in force, the forced one included; 0
 * with flip pacing off.
 * xbox_FlipIntervalPolicySet: fn decides each frame instead (NULL clears);
 * see XboxFlipIntervalPolicy for what it may do.
 * Both setters return 0 and change nothing with flip pacing off or with
 * RECOMP_FLIP_INTERVAL_MAX set, which overrides the title. They may be
 * called every frame: only a change is logged. A forced cap keeps what they
 * set, which decides again once it is released. */
int     xbox_FlipIntervalMaxSet(unsigned max);
unsigned xbox_FlipIntervalMaxGet(void);
int      xbox_FlipIntervalPolicySet(XboxFlipIntervalPolicy fn);

/* A debug switch for the forced cap while running (a settings panel), the
 * same as RECOMP_FLIP_INTERVAL_MAX: max 0-3 (more reads as 3) forces it,
 * below 0 hands the cap back to the title. Any thread, from the next flip.
 * Returns 0, changing nothing, with flip pacing off.
 * xbox_FlipIntervalMaxForced: the forced cap; -1 when the title decides, -2
 * with flip pacing off. */
int      xbox_FlipIntervalMaxForce(int max);
int      xbox_FlipIntervalMaxForced(void);

#endif /* NV2A_BACKEND_H */
