/* Vertices transformed on the raster pool must give the picture one thread
 * gives. Two batches of 1020 vertices (340 triangles in a 64x64 surface):
 *
 *   A  mov oPos, v0 ; mov oD0, v3            every vertex on its own
 *   B  mov oPos, v0 ; add c[10], c[10], v4 ; mov oD0, c[10]
 *      with constant writes on: each vertex's colour is the sum of all the
 *      vertices before it, so it holds only if B stays in order on one
 *      thread.
 *
 *   nv2a-xf-threads-test write <file>   with RECOMP_RASTER_THREADS=1
 *   nv2a-xf-threads-test check <file>   with a pool: same pixels */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 64
#define H 64
#define COLOR_AT  0x10000u
#define POS_AT    0x20000u
#define DIFF_AT   0x40000u
#define STEP_AT   0x48000u
#define MEM_BYTES 0x60000u
#define VERTS     1020u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
/* Memory model 2 is off here. */
int xbox_mem2_phys_to_guest(uint32_t pa, uint32_t *va) { (void)pa; (void)va; return 0; }
int xbox_mem2_arena_has(uint32_t va, uint32_t bytes) { (void)va; (void)bytes; return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
void xbox_RegisterTitleEvent(uint32_t guest_va) { (void)guest_va; }
void xbox_SetTitleEvent(uint32_t guest_va) { (void)guest_va; }
int xbox_Nv2aPgraphRaise(uint32_t addr, uint32_t data) { (void)addr; (void)data; return 0; }
int xbox_Nv2aPgraphWaitAck(uint32_t timeout_ms) { (void)timeout_ms; return 1; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_raster_pool_size(void);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ---- vertex program words ---------------------------------------------- */
enum { T = 1, V = 2, C = 3 };             /* source mux */
#define XYZW 0x1B
enum { MOV = 1, ADD = 3 };

static void src(uint32_t w[4], int which, int mux, int swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= (uint32_t)mux << 26; }
    else if (which == 1) { w[2] |= ((uint32_t)swz << 17) | ((uint32_t)mux << 11); }
    else { w[2] |= (uint32_t)swz << 2; w[3] |= (uint32_t)mux << 28; }
}

/* op a [, c] -> o[out] (to_o) or c[out]; input v[vin], constant c[cin]. */
static void ins(uint32_t w[4], int op, int amux, int cmux, int vin, int cin,
                int to_o, int out, int final)
{
    memset(w, 0, 16);
    w[1] = ((uint32_t)op << 21) | ((uint32_t)vin << 9) | ((uint32_t)cin << 13);
    src(w, 0, amux, XYZW);
    if (cmux)
        src(w, 2, cmux, XYZW);
    w[3] |= (0xFu << 12) | ((uint32_t)to_o << 11) | ((uint32_t)out << 3)
          | (uint32_t)final;
}

static void load_program(const uint32_t (*p)[4], int n)
{
    int i, k;
    m(0x1E9C, 0);                                  /* _PROGRAM_LOAD: slot 0 */
    for (i = 0; i < n; i++)
        for (k = 0; k < 4; k++)
            m(0x0B00 + 4u * (uint32_t)k, p[i][k]);
    m(0x1EA0, 0);                                  /* _PROGRAM_START: slot 0 */
}

static void draw(void)
{
    uint32_t first;

    memset(s_mem + COLOR_AT, 0, W * H * 4);
    m(0x17FC, 5);                                  /* BEGIN_END: triangles */
    for (first = 0; first < VERTS; first += 255)   /* DRAW_ARRAYS: 255 each */
        m(0x1810, (254u << 24) | first);
    m(0x17FC, 0);
}

int main(int argc, char **argv)
{
    uint32_t prog_a[2][4], prog_b[3][4], i, lit = 0;
    uint32_t seed = 12345;
    static uint8_t picture[2][W * H * 4];
    int writing, k;

    if (argc != 3 || (strcmp(argv[1], "write") && strcmp(argv[1], "check"))) {
        fprintf(stderr, "usage: nv2a-xf-threads-test write|check <file>\n");
        return 2;
    }
    writing = !strcmp(argv[1], "write");
    if (!writing && nv2a_raster_pool_size() < 2) {
        printf("FAIL: check needs a pool (RECOMP_RASTER_THREADS > 1)\n");
        return 1;
    }

    for (i = 0; i < VERTS; i++) {
        float pos[4], step[4];
        uint32_t col;
        seed = seed * 1103515245u + 12345u; pos[0] = (float)((seed >> 8) % (W * 16)) / 16.0f;
        seed = seed * 1103515245u + 12345u; pos[1] = (float)((seed >> 8) % (H * 16)) / 16.0f;
        pos[2] = 0.0f; pos[3] = 1.0f;
        seed = seed * 1103515245u + 12345u; col = seed | 0xFF000000u;
        step[0] = 0.0007f * (float)(i % 5); step[1] = 0.0005f * (float)(i % 7);
        step[2] = 0.0003f * (float)(i % 11); step[3] = 0.0f;
        memcpy(s_mem + POS_AT + 16u * i, pos, 16);
        memcpy(s_mem + DIFF_AT + 4u * i, &col, 4);
        memcpy(s_mem + STEP_AT + 16u * i, step, 16);
    }

    m(0x0208, 0x0028);                             /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    m(0x0200, (uint32_t)W << 16);                  /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);                  /* CLIP_VERTICAL */
    m(0x020C, ((W * 4u) << 16) | (W * 4u));        /* SURFACE_PITCH */
    m(0x0210, COLOR_AT);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x0358, 0x01010101u);                        /* SET_COLOR_MASK: all */
    m(0x030C, 0);                                  /* DEPTH_TEST_ENABLE: off */
    m(0x035C, 0);                                  /* DEPTH_MASK: off */
    m(0x1E94, 2);                                  /* TRANSFORM_EXECUTION_MODE: program */
    m(0x1720, POS_AT);  m(0x1760, 2 | (4 << 4) | (16 << 8));       /* v0: float4 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* v3: D3DCOLOR */
    m(0x1730, STEP_AT); m(0x1770, 2 | (4 << 4) | (16 << 8));       /* v4: float4 */

    /* A: every vertex on its own. */
    ins(prog_a[0], MOV, V, 0, 0, 0, 1, 0, 0);       /* mov o[0], v0 */
    ins(prog_a[1], MOV, V, 0, 3, 0, 1, 3, 1);       /* mov o[3], v3 ; final */
    m(0x1E98, 0);                                  /* _CXT_WRITE_EN: off */
    load_program((const uint32_t (*)[4])prog_a, 2);
    draw();
    memcpy(picture[0], s_mem + COLOR_AT, sizeof picture[0]);

    /* B: each vertex adds to c[10], which later vertices read. */
    ins(prog_b[0], MOV, V, 0, 0, 0, 1, 0, 0);       /* mov o[0], v0 */
    ins(prog_b[1], ADD, C, V, 4, 10, 0, 10, 0);     /* add c[10], c[10], v4 */
    ins(prog_b[2], MOV, C, 0, 0, 10, 1, 3, 1);      /* mov o[3], c[10] ; final */
    m(0x1EA4, 10);                                 /* _CONSTANT_LOAD: c[10] */
    for (k = 0; k < 4; k++)
        m(0x0B80 + 4u * (uint32_t)k, f2u(0.0f));
    m(0x1E98, 1);                                  /* _CXT_WRITE_EN: on */
    load_program((const uint32_t (*)[4])prog_b, 3);
    draw();
    m(0x1E98, 0);
    memcpy(picture[1], s_mem + COLOR_AT, sizeof picture[1]);

    for (k = 0; k < 2; k++)
        for (i = 0; i < W * H; i++)
            lit += (picture[k][4 * i] | picture[k][4 * i + 1] | picture[k][4 * i + 2]) != 0;
    if (lit < W * H) {
        printf("FAIL: only %u of %u pixels drawn\n", lit, 2 * W * H);
        return 1;
    }

    if (writing) {
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(picture, sizeof picture, 1, f) != 1) {
            printf("FAIL: cannot write %s\n", argv[2]);
            return 1;
        }
        fclose(f);
        printf("nv2a transform on one thread: written\n");
        return 0;
    } else {
        static uint8_t want[2][W * H * 4];
        FILE *f = fopen(argv[2], "rb");
        uint32_t bad[2] = {0, 0};
        if (!f || fread(want, sizeof want, 1, f) != 1) {
            printf("FAIL: cannot read %s\n", argv[2]);
            return 1;
        }
        fclose(f);
        for (k = 0; k < 2; k++)
            for (i = 0; i < W * H; i++)
                bad[k] += memcmp(&want[k][4 * i], &picture[k][4 * i], 4) != 0;
        if (bad[0] || bad[1]) {
            printf("FAIL: %u (plain) and %u (constant writes) pixels differ"
                   " on %d threads\n", bad[0], bad[1], nv2a_raster_pool_size());
            return 1;
        }
        printf("nv2a transform on %d threads: ok\n", nv2a_raster_pool_size());
        return 0;
    }
}
