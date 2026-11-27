/* Texture coordinates of a quad the near plane cut, on the software
 * rasteriser. A floor that reaches behind the eye is clipped at corners about
 * 2e5 pixels off the screen with a small w, while its far corners lie on the
 * screen at a w 500 times larger. Each texel of a linear texture holds its
 * own column, so every pixel says which texel it sampled; that is checked
 * against the perspective-correct coordinate worked out in doubles.
 *
 * With the edge functions in floats, the products of such coordinates round
 * by more than the off-screen corners' weight at a visible pixel, and their
 * large 1/w turns that into texture coordinates many texels off: a regular
 * grid of wrong texels over the floor. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 640
#define H 480
#define TEX_W 1024
#define COLOR_AT  0x10000u
#define TEX_AT    0x140000u
#define POS_AT    0x142000u
#define DIFF_AT   0x142100u
#define UV_AT     0x142200u
#define MEM_BYTES 0x150000u
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

int main(void)
{
    /* x, y and w out as they come in, w = the input's z. */
    static const float M[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 1, 0 };
    /* (screen x, screen y, w, u in texels): the quad as a clipper fans it,
     * two corners it made far off the screen and two far corners on it. */
    static const float c[4][4] = {
        { 192066.0f, 47587.8f, 1023.85f, 20884.5f },
        { 539.087f,  142.645f, 469713.0f, 21767.6f },
        { 403.551f,  127.629f, 549297.0f, 22672.7f },
        { 189616.0f, 47589.3f, 1023.81f, 20930.7f },
    };
    static const uint16_t idx[6] = { 0, 1, 2,  0, 2, 3 };
    const uint32_t white = 0xFFFFFFFFu;
    uint32_t i, t, p, checked = 0, bad = 0;
    int x, y, worst = 0;

#ifdef _WIN32
    _putenv("RECOMP_RASTER_THREADS=1");
#else
    setenv("RECOMP_RASTER_THREADS", "1", 1);
#endif

    /* Texel u: its column, low byte in red, high bits in green. */
    for (i = 0; i < TEX_W; i++) {
        uint32_t texel = 0xFF000000u | ((i & 0xFFu) << 16) | ((i >> 8) << 8);
        memcpy(s_mem + TEX_AT + 4u * i, &texel, 4);
    }

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
    /* Register combiners passing stage 0 through: the final combiner's D is
     * T0, alpha 1, as the games sample. */
    m(0x1E60, 0);                                  /* COMBINER_CONTROL: no general stages */
    m(0x1E70, 1);                                  /* stage 0: 2D texture */
    m(0x0288, 8u);                                 /* final: D = T0 */
    m(0x028C, 0x20u << 8);                         /* final alpha: 1 */
    m(0x1B00, TEX_AT);                             /* TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16));          /* FORMAT: LIN_A8R8G8B8, 1 level */
    m(0x1B08, 1u | (1u << 8));                     /* ADDRESS: wrap u and v */
    m(0x1B10, (TEX_W * 4u) << 16);                 /* CONTROL1: pitch */
    m(0x1B1C, ((uint32_t)TEX_W << 16) | 1u);       /* IMAGE_RECT: TEX_W x 1 */
    m(0x1E94, 0);                                  /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
    m(0x1744, UV_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));        /* attr 9: float2 */
    for (i = 0; i < 4; i++) {
        float pos[3] = { c[i][0] * c[i][2], c[i][1] * c[i][2], c[i][2] };
        float uv[2] = { c[i][3], 0.0f };
        memcpy(s_mem + POS_AT + 12u * i, pos, 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
        memcpy(s_mem + UV_AT + 8u * i, uv, 8);
    }
    m(0x17FC, 5);                                  /* BEGIN_END: triangles */
    for (i = 0; i < 6; i += 2)
        m(0x1800, idx[i] | ((uint32_t)idx[i + 1] << 16));  /* ARRAY_ELEMENT16 */
    m(0x17FC, 0);

    for (t = 0; t < 2; t++) {
        const float *v[3] = { c[idx[3 * t]], c[idx[3 * t + 1]], c[idx[3 * t + 2]] };
        double area = ((double)v[1][0] - v[0][0]) * ((double)v[2][1] - v[0][1])
                    - ((double)v[1][1] - v[0][1]) * ((double)v[2][0] - v[0][0]);

        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                double px = x + 0.5, py = y + 0.5, l[3], mw[3], u;
                int got, want, d;

                l[0] = (((double)v[2][0] - v[1][0]) * (py - v[1][1])
                      - ((double)v[2][1] - v[1][1]) * (px - v[1][0])) / area;
                l[1] = (((double)v[0][0] - v[2][0]) * (py - v[2][1])
                      - ((double)v[0][1] - v[2][1]) * (px - v[2][0])) / area;
                l[2] = 1.0 - l[0] - l[1];
                /* Inside, and clear of the edges, where a pixel may go to
                 * either triangle. */
                if (l[0] < 1e-6 || l[1] < 1e-4 || l[2] < 1e-4)
                    continue;
                for (i = 0; i < 3; i++)
                    mw[i] = l[i] / v[i][2];
                u = (mw[0] * v[0][3] + mw[1] * v[1][3] + mw[2] * v[2][3])
                  / (mw[0] + mw[1] + mw[2]);
                want = (int)floor(u) % TEX_W;
                memcpy(&p, s_mem + COLOR_AT + 4u * (y * W + x), 4);
                got = (int)(((p >> 16) & 0xFF) | (((p >> 8) & 0xFF) << 8));
                d = abs(got - want);
                if (d > TEX_W / 2)
                    d = TEX_W - d;                 /* across the wrap */
                checked++;
                if (d > 2)
                    bad++;
                if (d > worst)
                    worst = d;
            }
    }
    printf("%u pixels checked, %u more than 2 texels off, worst %d\n",
           checked, bad, worst);
    CHECK(checked > 100, "only %u pixels inside", checked);
    CHECK(bad == 0, "%u pixels more than 2 texels off (worst %d)", bad, worst);

    if (!s_fail)
        printf("nv2a far corner: ok\n");
    return s_fail ? 1 : 0;
}
