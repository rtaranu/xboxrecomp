/* Face culling on the software rasteriser (SET_CULL_FACE_ENABLE, _CULL_FACE,
 * _FRONT_FACE), with fixed-function triangles on a 16x16 surface:
 *
 *   - a clockwise triangle is drawn and the same one counter-clockwise is
 *     culled, with BACK culled and CW the front;
 *   - with FRONT culled, the other way round;
 *   - a strip's second triangle, whose corners come in the other order, is a
 *     front face too;
 *   - a triangle with a corner behind the eye: that corner projects to the
 *     same point as one in front of it, but the part in front of the eye
 *     winds the other way, so the facing comes from clip space, not from the
 *     three projected points.
 *
 * Without culling, back faces drew too: a title that draws closed meshes into
 * a shadow map got the far sides of thin slats as well, and thicker shadows. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 16
#define H 16
#define COLOR_AT  0x10000u
#define POS_AT    0x40000u
#define DIFF_AT   0x42000u
#define MEM_BYTES 0x50000u
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
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Corners as (screen x, screen y, w): the composite below makes clip space
 * (x w, y w, w, w), so a corner with w < 0 is behind the eye and projects to
 * (x, y) all the same. */
static void draw(uint32_t prim, const float (*c)[3], uint32_t n)
{
    uint32_t i, white = 0xFFFFFFFFu;

    for (i = 0; i < n; i++) {
        float p[3] = { c[i][0] * c[i][2], c[i][1] * c[i][2], c[i][2] };
        memcpy(s_mem + POS_AT + 12u * i, p, 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
    }
    m(0x17FC, prim);                               /* BEGIN_END */
    m(0x1810, (n - 1) << 24);                      /* DRAW_ARRAYS: n from 0 */
    m(0x17FC, 0);
}

static int lit(int x, int y)
{
    uint32_t p;
    memcpy(&p, s_mem + COLOR_AT + 4u * (uint32_t)(y * W + x), 4);
    return p == 0xFFFFFFFFu;
}

static void clear(void) { memset(s_mem + COLOR_AT, 0, 4u * W * H); }

int main(void)
{
    /* x, y and w out as they come in, w = the input's z. */
    static const float M[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 1, 0 };
    /* Clockwise on the screen, y down: left to right along the top, then down. */
    static const float cw[3][3]  = { {2, 4, 1}, {14, 4, 1}, {8, 14, 1} };
    static const float ccw[3][3] = { {2, 4, 1}, {8, 14, 1}, {14, 4, 1} };
    /* A strip over the whole surface: (0,1,2) clockwise, (1,2,3) the other
     * order, the same face. */
    static const float strip[4][3] = { {0, 0, 1}, {16, 0, 1}, {0, 16, 1}, {16, 16, 1} };
    /* cw with its third corner behind the eye: the same projected points, but
     * what is in front of the eye lies above the top edge, wound the other way. */
    static const float behind[3][3]  = { {2, 4, 1}, {14, 4, 1}, {8, 14, -1} };
    static const float behind2[3][3] = { {2, 4, 1}, {8, 14, -1}, {14, 4, 1} };
    uint32_t i;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=1");
#else
    setenv("RECOMP_RASTER_THREADS", "1", 1);
#endif

    m(0x0208, 0x0028);                             /* SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    m(0x0200, (uint32_t)W << 16);                  /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);                  /* CLIP_VERTICAL */
    m(0x020C, ((W * 4u) << 16) | (W * 4u));        /* SURFACE_PITCH */
    m(0x0210, COLOR_AT);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x0358, 0x01010101u);                        /* SET_COLOR_MASK: all */
    m(0x030C, 0);                                  /* DEPTH_TEST_ENABLE: off */
    m(0x035C, 0);                                  /* DEPTH_MASK: off */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);                  /* composite */
    m(0x1E94, 0);                                  /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
    m(0x0308, 1);                                  /* SET_CULL_FACE_ENABLE */
    m(0x03A0, 0x900);                              /* SET_FRONT_FACE: CW */
    m(0x039C, 0x405);                              /* SET_CULL_FACE: BACK */

    clear(); draw(5, cw, 3);
    CHECK(lit(8, 8), "a clockwise front face is drawn");
    clear(); draw(5, ccw, 3);
    CHECK(!lit(8, 8), "a counter-clockwise back face is culled");

    m(0x039C, 0x404);                              /* SET_CULL_FACE: FRONT */
    clear(); draw(5, cw, 3);
    CHECK(!lit(8, 8), "FRONT culls the clockwise one");
    clear(); draw(5, ccw, 3);
    CHECK(lit(8, 8), "FRONT keeps the counter-clockwise one");
    m(0x039C, 0x405);

    clear(); draw(6, strip, 4);
    CHECK(lit(2, 2) && lit(13, 13), "both triangles of a strip are front faces: %d %d",
          lit(2, 2), lit(13, 13));

    clear(); draw(5, behind, 3);
    CHECK(!lit(8, 1), "a corner behind the eye turns the face: culled");
    clear(); draw(5, behind2, 3);
    CHECK(lit(8, 1), "and the other order is drawn, above the top edge");
    CHECK(!lit(8, 8), "where the projected corners would put it, nothing");

    m(0x0308, 0);                                  /* culling off: both drawn */
    clear(); draw(5, ccw, 3);
    CHECK(lit(8, 8), "with culling off a back face is drawn");

    if (!s_fail)
        printf("nv2a cull: ok\n");
    return s_fail ? 1 : 0;
}
