/*
 * The Vulkan rasteriser through the executor: methods in, guest memory out.
 * Each check is a picture worked out by hand, except the combiners' and the
 * vertex programs', which are compared with the CPU's (nv2a_combiner.h,
 * nv2a_vsh_interp.h) over random programs.
 * Exit 77: no Vulkan here.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend_batch.h"
#include "nv2a_capture.h"
#include "nv2a_perf.h"
#include "nv2a_vk.h"
#include "nv2a_vk_pipeline_cache.h"
#include "../d3d/d3d8_swizzle.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#define Sleep(ms) usleep((ms) * 1000)
#endif

#define MEM_BYTES 0x200000u
#define COLOR_AT  0x10000u
#define ZETA_AT   0x20000u
#define SMALL_AT  0x30000u
#define TEX_AT    0x40000u
#define POS_AT    0x50000u
#define UV_AT     0x51000u
#define BIG_AT    0xF0000u
#define PB_FMT_F(n, stride) (2u | (n) << 4 | (stride) << 8)
#define W 64
#define H 32

/* The GPU's addresses: the window's below MEM_BYTES, low memory's at LOW_AT. */
#define WINDOW_BYTES 0x4000000u
#define LOW_AT    0x200000u
#define LOW_BYTES 0x10000u
#define GUEST(at) (0x80000000u + (at))

static uint8_t *s_base, *s_mem;                 /* guest address 0; the window */
static void *s_tiled, *s_spare;
static int s_touch;                             /* all tests with TOUCH */
static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_base; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
/* What the GPU's addresses mean (nv2a_pb_exec_set_resolver). */
static int offset_va(uint32_t pa, uint32_t *va)
{
    if (!pa || pa >= MEM_BYTES)
        return 0;
    *va = GUEST(pa);
    return 1;
}
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static unsigned s_windowed;                      /* from guest memory */
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    (void)va; (void)pitch;
    s_windowed++;
}
void recomp_exit_hook_run(const char *why) { (void)why; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
static volatile unsigned s_vblanks;
uint32_t xbox_VblankCount(void) { return s_vblanks; }
void xbox_HostSleepNs(uint64_t ns) { (void)ns; Sleep(1); }
static uint64_t s_clock;
uint64_t xbox_HostNowNs(void) { return s_clock += 1000; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
void xbox_RegisterTitleEvent(uint32_t guest_va) { (void)guest_va; }
void xbox_SetTitleEvent(uint32_t guest_va) { (void)guest_va; }
int xbox_Nv2aPgraphRaise(uint32_t addr, uint32_t data) { (void)addr; (void)data; return 0; }
int xbox_Nv2aPgraphWaitAck(uint32_t timeout_ms) { (void)timeout_ms; return 1; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_run(uint32_t method, const uint32_t *p, uint32_t n);

static void method(uint32_t number, uint32_t value)
{
    nv2a_pb_exec_method(0, number, value);
}

static void method_float(uint32_t number, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    method(number, bits);
}

static void put_float(uint32_t address, float value)
{
    memcpy(s_mem + address, &value, sizeof value);
}

static uint32_t word(uint32_t at)
{
    uint32_t v;
    memcpy(&v, s_mem + at, 4);
    return v;
}

static uint32_t pixel(uint32_t x, uint32_t y) { return word(COLOR_AT + y * W * 4 + x * 4); }

/* Each channel within `by`. */
static int close_to(uint32_t a, uint32_t b, int by)
{
    unsigned i;

    for (i = 0; i < 32; i += 8)
        if (abs((int)((a >> i) & 0xFF) - (int)((b >> i) & 0xFF)) > by)
            return 0;
    return 1;
}

static void surface(uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format)
{
    method(0x0200, w << 16);
    method(0x0204, h << 16);
    method(0x0208, format);
    method(0x020C, pitch | pitch << 16);
    method(0x0210, offset);
    method(0x0214, ZETA_AT);
}

static void main_surface(void)
{
    surface(COLOR_AT, W, H, W * 4, 0x0128);      /* A8R8G8B8, Z24S8, pitch */
}

static void clear(uint32_t argb)
{
    method(0x1D90, argb);
    method(0x1D8C, 0xFFFFFF00u);
    method(0x1D94, 0xF3);
}

static void flip(void) { method(0x0130, 0); }

/* A rectangle in surface pixels, corners in D3D's clockwise order. */
static float s_quad_w = 1.0f;

static void quad(float x0, float y0, float x1, float y1, float z, float u1, float v1)
{
    const float w = s_quad_w;
    const float p[16] = { x0, y0, z, w,  x1, y0, z, w,  x1, y1, z, w,  x0, y1, z, w };
    const float t[8]  = { 0, 0,  u1, 0,  u1, v1,  0, v1 };
    unsigned i;

    for (i = 0; i < 16; i++)
        put_float(POS_AT + 4 * i, p[i]);
    for (i = 0; i < 8; i++)
        put_float(UV_AT + 4 * i, t[i]);
    method(0x17FC, 8);
    method(0x1810, 3u << 24);
    method(0x17FC, 0);
}

static void diffuse(uint32_t argb)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        method_float(0x1A00 + 16 * 3 + 4 * i,
                     (float)((argb >> (i == 3 ? 24 : 16 - 8 * i)) & 0xFF) / 255.0f);
}

static void rect(float x0, float y0, float x1, float y1, uint32_t argb)
{
    diffuse(argb);
    quad(x0, y0, x1, y1, 0, 1, 1);
}

/* The final combiner alone: the primary colour, or texture 0. */
static void pass(uint32_t reg)
{
    method(0x1E60, 1);
    method(0x0260, 0);                           /* ALPHA_ICW 0 */
    method(0x0AC0, 0);
    method(0x0AA0, 0);
    method(0x1E40, 0);
    method(0x0288, reg);                         /* D: the register's RGB */
    method(0x028C, (reg | 0x10u) << 8);          /* G: its alpha */
}

static void setup(void)
{
    static const uint32_t attr[4] = { 0, 3, 4, 9 };
    unsigned i;

    main_surface();
    /* A vertex program that hands every attribute on: positions are surface
     * pixels. (The executor's fixed function has no secondary colour.) */
    method(0x1E94, 2);
    method(0x1E9C, 0);
    for (i = 0; i < 4; i++) {
        method(0x0B00, 0);
        method(0x0B04, 1u << 21 | 0x1Bu | attr[i] << 9);     /* MOV o[], v[] */
        method(0x0B08, 2u << 26);
        method(0x0B0C, 0xFu << 12 | 1u << 11 | attr[i] << 3 | (i == 3));
    }
    method(0x1EA0, 0);
    method_float(0x0394, 0.0f);
    method_float(0x0398, 16777215.0f);
    method(0x1720, POS_AT);
    method(0x1760, PB_FMT_F(4u, 16u));
    method(0x1720 + 4 * 9, UV_AT);
    method(0x1760 + 4 * 9, PB_FMT_F(2u, 8u));
    method(0x0358, 0x01010101u);
    method(0x0344, 1);
    method(0x0348, 0);
    method(0x0350, 0x8006);
    method(0x0354, 0x207);
    method(0x1E70, 0);
    method(0x03B8, 1);                           /* specular colours, alpha and all */
    method(0x0294, 0x20001u);
    pass(4);
}

static void test_clear(void)
{
    clear(0xFF102030u);
    flip();
    CHECK(pixel(0, 0) == 0xFF102030u && pixel(W - 1, H - 1) == 0xFF102030u);
    method(0x1D90, 0x80FFEEDDu);
    method(0x1D94, 0x20);                        /* green alone */
    flip();
    CHECK(pixel(5, 5) == 0xFF10EE30u);
    method(0x1D98, 8u | 15u << 16);              /* columns 8-15, rows 4-5 */
    method(0x1D9C, 4u | 5u << 16);
    method(0x1D90, 0x01020304u);
    method(0x1D94, 0xF0);
    method(0x1D98, (W - 1u) << 16);
    method(0x1D9C, (H - 1u) << 16);
    flip();
    CHECK(pixel(8, 4) == 0x01020304u && pixel(15, 5) == 0x01020304u);
    CHECK(pixel(7, 4) == 0xFF10EE30u && pixel(16, 5) == 0xFF10EE30u);
    CHECK(pixel(8, 3) == 0xFF10EE30u && pixel(8, 6) == 0xFF10EE30u);
}

/* Exactly the pixels whose centres are inside. */
static void test_coverage(void)
{
    uint32_t x, y, wrong = 0;

    clear(0xFF000000u);
    rect(4, 3, 12, 9, 0xFFFF8000u);
    rect(20.4f, 3, 24.6f, 9, 0xFF00FF00u);       /* centres 20.5 .. 24.5 */
    flip();
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t want = y >= 3 && y < 9 ? x >= 4 && x < 12 ? 0xFFFF8000u
                          : x >= 20 && x < 25 ? 0xFF00FF00u : 0xFF000000u : 0xFF000000u;
            wrong += pixel(x, y) != want;
        }
    CHECK(wrong == 0);
}

static void test_depth(void)
{
    clear(0xFF000000u);
    method(0x030C, 1);
    method(0x0354, 0x201);                       /* less */
    method(0x035C, 1);
    diffuse(0xFFFF0000u);
    quad(0, 0, 8, 8, 8000000.0f, 1, 1);
    diffuse(0xFF00FF00u);
    quad(4, 0, 12, 8, 9000000.0f, 1, 1);         /* behind */
    diffuse(0xFF0000FFu);
    quad(0, 4, 8, 12, 7000000.0f, 1, 1);         /* in front */
    method(0x035C, 0);
    diffuse(0xFFFFFFFFu);
    quad(30, 0, 34, 4, 10.0f, 1, 1);             /* drawn, not written */
    diffuse(0xFF00FFFFu);
    quad(30, 0, 34, 4, 20.0f, 1, 1);
    method(0x030C, 0);
    flip();
    CHECK(pixel(2, 2) == 0xFFFF0000u && pixel(6, 2) == 0xFFFF0000u);
    CHECK(pixel(10, 2) == 0xFF00FF00u);
    CHECK(pixel(2, 6) == 0xFF0000FFu && pixel(2, 10) == 0xFF0000FFu);
    CHECK(pixel(31, 1) == 0xFF00FFFFu);
    method(0x0354, 0x207);
}

/* Z24 and Z16 as floating point (SET_CONTROL0): what is nearer, and the clear
 * value's meaning. */
static void test_float_depth(void)
{
    static const struct { uint32_t format, clear; float near_to, far_from; } k[] = {
        /* 2^100 and 1.5 * 2^64 as 8 and 16 bits; 2 + 2 * 0x555 / 0x1000 as 4 and 12. */
        { 0x0128, 0xE30000FFu, 1.2e30f, 1.3e30f },
        { 0x0128, 0xBF8000FFu, 2.74e19f, 2.79e19f },
        { 0x0118, 0x8555,      2.66f,   2.67f },
        { 0x0118, 0xFFFF,      511.9f,  512.0f },
    };
    unsigned i;

    method(0x0290, 0x1000);
    method(0x0354, 0x201);                       /* less */
    method(0x035C, 1);
    method(0x030C, 1);
    for (i = 0; i < 4; i++) {
        surface(COLOR_AT, W, H, W * 4, k[i].format);
        method(0x1D90, 0xFF000000u);
        method(0x1D8C, k[i].clear);
        method(0x1D94, 0xF3);
        diffuse(0xFFFF0000u);
        quad(0, 0, 8, 8, k[i].near_to, 1, 1);
        diffuse(0xFF00FF00u);
        quad(10, 0, 18, 8, k[i].far_from, 1, 1);
        diffuse(0xFF0000FFu);
        quad(4, 0, 8, 8, k[i].near_to * 0.5f, 1, 1);
        diffuse(0xFFFFFFFFu);
        quad(0, 0, 8, 8, k[i].near_to * 0.75f, 1, 1);
        flip();
        CHECK(pixel(2, 2) == 0xFFFFFFFFu && pixel(6, 2) == 0xFF0000FFu);
        CHECK(pixel(12, 2) == 0xFF000000u);
    }
    method(0x030C, 0);
    method(0x035C, 0);
    method(0x0354, 0x207);
    method(0x0290, 0);
    main_surface();
}

static void test_stencil(void)
{
    clear(0xFF000000u);
    method(0x032C, 1);
    method(0x0360, 0xFF);
    method(0x0364, 0x207);                       /* always, replace by 5 */
    method(0x0368, 5);
    method(0x036C, 0xFF);
    method(0x0370, 0x1E00);
    method(0x0374, 0x1E00);
    method(0x0378, 0x1E01);
    method(0x0358, 0);                           /* no colour */
    rect(0, 0, 8, 8, 0xFFFFFFFFu);
    method(0x0358, 0x01010101u);
    method(0x0364, 0x202);                       /* equal */
    method(0x0378, 0x1E00);
    rect(4, 0, 12, 8, 0xFFFF0000u);
    method(0x0368, 0x15);
    method(0x036C, 0x0F);                        /* 5 and 0x15 agree here */
    rect(0, 4, 4, 8, 0xFF00FF00u);
    method(0x032C, 0);
    flip();
    CHECK(pixel(2, 2) == 0xFF000000u && pixel(6, 2) == 0xFFFF0000u);
    CHECK(pixel(10, 2) == 0xFF000000u && pixel(2, 6) == 0xFF00FF00u);
}

static void triangle(int clockwise, float x)
{
    const float p[3][2] = { { x, 0 }, { x + 8, 0 }, { x, 8 } };
    unsigned i;

    for (i = 0; i < 3; i++) {
        const float *v = p[clockwise ? i : 2 - i];
        put_float(POS_AT + 16 * i, v[0]);
        put_float(POS_AT + 16 * i + 4, v[1]);
        put_float(POS_AT + 16 * i + 8, 0);
        put_float(POS_AT + 16 * i + 12, 1);
    }
    method(0x17FC, 5);
    method(0x1810, 2u << 24);
    method(0x17FC, 0);
}

/* Clockwise on screen (y down) is GL_CW. */
static void test_cull(void)
{
    diffuse(0xFFFFFFFFu);
    clear(0xFF000000u);
    method(0x0308, 1);
    method(0x039C, 0x405);                       /* back */
    method(0x03A0, 0x900);                       /* clockwise is front */
    triangle(1, 0);
    triangle(0, 10);
    method(0x03A0, 0x901);
    triangle(1, 20);
    triangle(0, 30);
    method(0x039C, 0x404);                       /* front */
    triangle(1, 40);
    triangle(0, 50);
    method(0x0308, 0);
    flip();
    CHECK(pixel(1, 1) == 0xFFFFFFFFu && pixel(11, 1) == 0xFF000000u);
    CHECK(pixel(21, 1) == 0xFF000000u && pixel(31, 1) == 0xFFFFFFFFu);
    CHECK(pixel(41, 1) == 0xFFFFFFFFu && pixel(51, 1) == 0xFF000000u);
}

static void test_blend(void)
{
    clear(0xFF204060u);
    method(0x0304, 1);
    method(0x0344, 0x302);                       /* src alpha, 1 - src alpha */
    method(0x0348, 0x303);
    rect(0, 0, 8, 8, 0x80FF0000u);
    method(0x0344, 1);                           /* one, one */
    method(0x0348, 1);
    rect(10, 0, 18, 8, 0x00102030u);
    method(0x0350, 0x800B);                      /* dst - src */
    rect(20, 0, 28, 8, 0x00102030u);
    method(0x0350, 0x8006);
    method(0x0344, 0x8001);                      /* constant colour, zero */
    method(0x0348, 0);
    method(0x034C, 0xFF804020u);
    rect(30, 0, 38, 8, 0xFFFFFFFFu);
    method(0x0304, 0);
    method(0x0358, 0x00010000u);                 /* red alone */
    rect(40, 0, 48, 8, 0x00FFFFFFu);
    method(0x0358, 0x01010101u);
    method(0x0344, 1);
    flip();
    CHECK(close_to(pixel(2, 2), 0xBF90202Fu, 2));
    CHECK(close_to(pixel(12, 2), 0xFF306090u, 1));
    CHECK(close_to(pixel(22, 2), 0xFF102030u, 1));
    CHECK(close_to(pixel(32, 2), 0xFF804020u, 1));
    CHECK(pixel(42, 2) == 0xFFFF4060u);
}

/* SET_LOGIC_OP: all sixteen, of 0xC and 0xA in every digit; what is blended
 * does not count. */
static void test_logic(void)
{
    /* From CLEAR: clear, and, and reverse, copy, and inverted, noop, xor, or,
     * nor, equiv, invert, or reverse, copy inverted, or inverted, nand, set. */
    static const uint32_t want[16] = { 0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
                                       0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF };
    unsigned i;

    for (i = 0; i < 16; i++) {
        clear(0xAAAAAAAAu);
        method(0x0304, 1);
        method(0x0344, 0);
        method(0x0348, 1);
        method(0x17BC, 1);
        method(0x17C0, 0x1500 + i);
        rect(0, 0, 8, 8, 0xCCCCCCCCu);
        method(0x17BC, 0);
        method(0x0304, 0);
        method(0x0344, 1);
        method(0x0348, 0);
        rect(10, 0, 18, 8, 0xCCCCCCCCu);
        flip();
        CHECK(pixel(2, 2) == want[i] * 0x11111111u && pixel(12, 2) == 0xCCCCCCCCu);
    }
}

static void test_alpha(void)
{
    clear(0xFF000000u);
    method(0x0300, 1);
    method(0x033C, 0x204);                       /* greater than 0x80 */
    method(0x0340, 0x80);
    rect(0, 0, 8, 8, 0x80FFFFFFu);
    rect(10, 0, 18, 8, 0x81FFFFFFu);
    method(0x0300, 0);
    flip();
    CHECK(pixel(2, 2) == 0xFF000000u && pixel(12, 2) == 0x81FFFFFFu);
}

/* ---- visibility tests ---- */

#define REPORT_AT 0x60000u

extern void nv2a_pb_exec_idle(void);

static void count_on(void)
{
    method(0x17C8, 1);
    method(0x17CC, 1);
}

static void ask(uint32_t n)
{
    memset(s_mem + REPORT_AT + 16 * n, 0xFF, 16);
    method(0x17CC, 0);
    method(0x17D0, 1u << 24 | (REPORT_AT + 16 * n));
}

static int answered(uint32_t n) { return word(REPORT_AT + 16 * n + 12) == 0; }

/* As a title waits for it: what it asked is all that was sent. */
static uint32_t answer(uint32_t n)
{
    unsigned ms;

    nv2a_pb_exec_idle();
    for (ms = 0; ms < 2000 && !answered(n); ms++)
        Sleep(1);
    CHECK(answered(n));
    return word(REPORT_AT + 16 * n + 8);
}

static void test_reports(void)
{
    Nv2aVkStats was, st;
    uint32_t i, wrong = 0;

    flip();
    clear(0xFF000000u);
    nv2a_vk_stats(&was);
    count_on();
    rect(0, 0, 10, 10, 0xFFFFFFFFu);
    ask(0);
    CHECK(!answered(0));                         /* nothing was submitted */
    nv2a_pb_exec_idle();
    nv2a_pb_exec_idle();                         /* told once */
    CHECK(answer(0) == 100);
    nv2a_vk_stats(&st);
    CHECK(st.reports == was.reports + 1 && st.early == was.early + 1);
    CHECK(st.flushes == was.flushes);

    /* The frame goes on, and its picture has both halves. */
    rect(20, 0, 24, 4, 0xFF00FF00u);             /* not counted */
    count_on();
    rect(30, 0, 34, 4, 0xFF0000FFu);
    rect(30, 8, 34, 10, 0xFF0000FFu);
    ask(1);
    method(0x17CC, 1);                           /* on, without a clear */
    rect(30, 12, 31, 13, 0xFF0000FFu);
    ask(2);
    CHECK(answer(2) == 25 && answer(1) == 24);
    CHECK(word(REPORT_AT + 16) < word(REPORT_AT + 32));  /* the times */
    count_on();
    ask(3);
    CHECK(answer(3) == 0);
    flip();
    CHECK(pixel(5, 5) == 0xFFFFFFFFu && pixel(22, 2) == 0xFF00FF00u);
    CHECK(pixel(32, 2) == 0xFF0000FFu && pixel(32, 9) == 0xFF0000FFu);

    /* Pixels that fail a test do not count, nor do clears; ones with no
     * colour written do. */
    clear(0xFF000000u);
    method(0x030C, 1);
    method(0x0354, 0x201);                       /* less */
    method(0x035C, 1);
    quad(0, 0, 8, 8, 8000000.0f, 1, 1);
    method(0x0358, 0);
    count_on();
    quad(4, 0, 12, 8, 9000000.0f, 1, 1);         /* half of it behind */
    ask(0);
    method(0x0358, 0x01010101u);
    method(0x030C, 0);
    method(0x0354, 0x207);
    count_on();
    method(0x0300, 1);
    method(0x033C, 0x204);
    method(0x0340, 0x80);
    rect(0, 16, 8, 24, 0x80FFFFFFu);             /* killed */
    rect(10, 16, 13, 24, 0x81FFFFFFu);
    method(0x0300, 0);
    method(0x1D90, 0x11223344u);
    method(0x1D94, 0x20);                        /* green alone: drawn */
    method(0x1D94, 0xF0);
    ask(1);
    CHECK(answer(1) == 24 && answer(0) == 32);

    /* Over a change of target, and over a flip. */
    count_on();
    rect(0, 0, 3, 3, 0xFFFFFFFFu);
    surface(SMALL_AT, 16, 16, 64, 0x0128);
    rect(0, 0, 4, 4, 0xFFFFFFFFu);
    main_surface();
    rect(0, 0, 5, 5, 0xFFFFFFFFu);
    flip();
    rect(0, 0, 6, 6, 0xFFFFFFFFu);
    ask(0);
    CHECK(answer(0) == 9 + 16 + 25 + 36);

    /* Turned off and on again, and cleared while on. */
    count_on();
    rect(0, 0, 3, 1, 0xFFFFFFFFu);
    method(0x17CC, 0);
    rect(0, 0, 9, 1, 0xFFFFFFFFu);
    method(0x17CC, 1);
    rect(0, 0, 4, 1, 0xFFFFFFFFu);
    ask(0);
    count_on();
    rect(0, 0, 9, 1, 0xFFFFFFFFu);
    method(0x17C8, 1);
    rect(0, 0, 5, 1, 0xFFFFFFFFu);
    ask(1);
    CHECK(answer(1) == 5 && answer(0) == 7);

    /* Not waited for: the frame goes on while the GPU has its first parts. */
    clear(0xFF000000u);
    for (i = 0; i < 8; i++) {
        count_on();
        rect(0, (float)i, (float)(i + 1), (float)(i + 1), 0xFFFFFFFFu);
        ask(i);
        nv2a_pb_exec_idle();
    }
    flip();
    for (i = 0; i < 8; i++)
        CHECK(answer(i) == i + 1 && pixel(i, i) == 0xFFFFFFFFu && pixel(i + 1, i) == 0xFF000000u);

    /* Many, each waited for in one frame: more than it has submissions. */
    for (i = 0; i < 40; i++) {
        count_on();
        rect(0, 0, (float)(i + 1), 2, 0xFFFFFFFFu);
        ask(0);
        wrong += answer(0) != 2 * (i + 1);
    }
    flip();
    /* Many at once: more than are kept track of, and more queries. */
    for (i = 0; i < 3000; i++) {
        count_on();
        rect(0, 0, 1 + (float)(i % 60), 1, 0xFFFFFFFFu);
        ask(i);
        if (i % 700 == 0)
            flip();
    }
    for (i = 0; i < 3000; i++)
        wrong += answer(i) != 1 + i % 60;
    count_on();
    for (i = 0; i < 5000; i++) {
        surface(i & 1 ? SMALL_AT : COLOR_AT, 16, 16, 64, 0x0128);
        rect(0, 0, i < 4096 ? 1.0f : 2.0f, 1, 0xFFFFFFFFu);
    }
    main_surface();
    ask(0);
    CHECK(answer(0) == 4096 + 904 * 2);
    CHECK(wrong == 0);

    /* Asked, and the memory freed before the answer. */
    count_on();
    rect(0, 0, 1, 1, 0xFFFFFFFFu);
    ask(0);
    ask(1);
    nv2a_vk_memory_freed(GUEST(REPORT_AT), 16);
    CHECK(answer(1) == 1 && !answered(0));
    flip();
    memset(s_mem + REPORT_AT, 0, 16 * 3000);
}

/* Other layouts of the colour surface, and what reading them back gives. */
static void test_formats(void)
{
    uint32_t mx, my;
    uint16_t v;

    surface(SMALL_AT, 16, 16, 32, 0x0123);       /* R5G6B5 */
    clear(0xFF000000u);
    rect(2, 1, 4, 3, 0xFFFF8040u);
    flip();
    memcpy(&v, s_mem + SMALL_AT + 1 * 32 + 2 * 2, 2);
    CHECK(v == 0xFC08);
    memcpy(&v, s_mem + SMALL_AT + 1 * 32 + 4 * 2, 2);
    CHECK(v == 0);

    /* A8R8G8B8, swizzled, 16x16 */
    surface(SMALL_AT, 16, 16, 64, 0x0228u | 4u << 16 | 4u << 24);
    clear(0xFF000000u);
    rect(5, 9, 6, 10, 0xFF123456u);
    flip();
    xbox_swizzle_masks(16, 16, &mx, &my);
    CHECK(word(SMALL_AT + 4 * (swizzle_deposit(5, mx) | swizzle_deposit(9, my))) == 0xFF123456u);
    CHECK(word(SMALL_AT + 4 * (swizzle_deposit(9, mx) | swizzle_deposit(5, my))) == 0xFF000000u);
    main_surface();
}

/* One draw of one colour, dithered, into a surface of 2 bytes a pixel: each
 * channel of a 4x4 block has the two nearest values, as many of each as the
 * colour asks for. The channels' shifts and largest values are given. */
static void dithered(uint32_t format, const uint32_t shift[3], const uint32_t most[3])
{
    uint32_t level, c, x, y, wrong = 0, mixed = 0;

    surface(SMALL_AT, 16, 16, 32, format);
    for (level = 0; level < 256; level += 5) {
        rect(0, 0, 16, 16, 0xFF000000u | level * 0x010101u);
        flip();
        for (c = 0; c < 3; c++) {
            uint32_t low = level * most[c] / 255, total, sum = 0;

            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x++) {
                    uint16_t v, first;
                    uint32_t n;

                    memcpy(&v, s_mem + SMALL_AT + y * 32 + x * 2, 2);
                    memcpy(&first, s_mem + SMALL_AT + (y & 3) * 32 + (x & 3) * 2, 2);
                    n = (v >> shift[c]) & most[c];
                    wrong += n != low && n != low + 1;
                    wrong += v != first;
                    sum += n - low;
                }
            total = (level * most[c] % 255) * 64;       /* of 255 */
            wrong += abs((int)(sum * 255) - (int)total) > 2 * 255 + 2;
            wrong += total == 0 && sum != 0;
            mixed += sum != 0 && sum != 64;
        }
    }
    CHECK(wrong == 0 && mixed > 100);
    main_surface();
}

static void test_dither(void)
{
    static const uint32_t s565[3] = { 11, 5, 0 }, m565[3] = { 31, 63, 31 };
    static const uint32_t s555[3] = { 10, 5, 0 }, m555[3] = { 31, 31, 31 };
    uint32_t x, wrong = 0, v;

    method(0x0310, 1);
    dithered(0x0123, s565, m565);
    dithered(0x0121, s555, m555);

    /* Added to what is there: the sum is what is dithered. */
    surface(SMALL_AT, 16, 16, 32, 0x0123);
    clear(0xFF808080u);                          /* 16, 32, 16 */
    method(0x0304, 1);
    method(0x0344, 1);
    method(0x0348, 1);
    rect(0, 0, 16, 16, 0xFF040404u);             /* 0.486 of a step of blue */
    method(0x0304, 0);
    flip();
    for (x = 0, v = 0; x < 16; x++) {
        uint16_t p;

        memcpy(&p, s_mem + SMALL_AT + (x >> 2) * 32 + (x & 3) * 2, 2);
        wrong += (p & 31u) != 16 && (p & 31u) != 17;
        v += (p & 31u) - 16;
    }
    CHECK(wrong == 0 && v >= 7 && v <= 9);

    /* Eight bits have nothing to dither; off, one value. */
    main_surface();
    clear(0xFF000000u);
    rect(0, 0, 16, 16, 0xFF818283u);
    method(0x0310, 0);
    surface(SMALL_AT, 16, 16, 32, 0x0123);
    rect(0, 0, 16, 16, 0xFF141414u);             /* 2.43, 4.94, 2.43 */
    main_surface();
    flip();
    for (x = 0, wrong = 0; x < 16; x++) {
        uint16_t p;

        memcpy(&p, s_mem + SMALL_AT + (x >> 2) * 32 + (x & 3) * 2, 2);
        wrong += p != (2u << 11 | 5u << 5 | 2u);
        wrong += pixel(x & 3, x >> 2) != 0xFF818283u;
    }
    CHECK(wrong == 0);
}

static void bind_texture(uint32_t at, uint32_t color, uint32_t w_log2, uint32_t h_log2)
{
    method(0x1B00, at);
    method(0x1B04, 0x28u | color << 8 | 1u << 16 | w_log2 << 20 | h_log2 << 24);
    method(0x1B08, 0x00030303u);                 /* clamp */
    method(0x1B0C, 0x4003FFC0u);
    method(0x1B14, 0x01012000u);                 /* nearest */
    method(0x1E70, 1);
}

/* ---- where the time goes ---- */

/* The GPU's ms for a frame in two submissions. */
static double two_submissions(int first, int second)
{
    Nv2aVkStats was, st;

    flip();
    nv2a_vk_wait();
    nv2a_vk_stats(&was);
    count_on();
    while (first--)
        rect(0, 0, W, H, 0xFFFFFFFFu);
    ask(0);
    nv2a_pb_exec_idle();
    while (second--)
        rect(0, 0, W, H, 0xFFFFFFFFu);
    flip();
    nv2a_vk_wait();
    nv2a_vk_stats(&st);
    CHECK(st.early == was.early + 1);
    return st.gpu_ms - was.gpu_ms;
}

static void test_perf(void)
{
    Nv2aVkStats was, st;
    Nv2aPerfStats s;
    double sum = 0.0, all;
    int k;

    nv2a_perf_phase(NV2A_PERF_WORK);
    nv2a_perf_set(1);
    rect(0, 0, 8, 8, 0xFFFFFFFFu);
    flip();                                      /* starts it; not one of them */
    nv2a_vk_wait();
    nv2a_vk_stats(&was);
    for (k = 0; k < 4; k++) {
        clear(0xFF000000u);
        count_on();
        rect(0, 0, 8, 8, 0xFFFFFFFFu);
        ask(0);
        nv2a_pb_exec_idle();                     /* a submission of its own */
        rect(8, 0, 16, 8, 0xFFFFFFFFu);
        if (k == 1) {                            /* used nowhere else */
            method(0x0304, 1);
            method(0x0344, 0x307);
            method(0x0348, 0x305);
            rect(0, 0, 1, 1, 0xFFFFFFFFu);
            method(0x0304, 0);
            method(0x0344, 1);
            method(0x0348, 0);
        }
        if (k == 2) {
            pass(8);
            bind_texture(TEX_AT + 0x4000, 0x06, 5, 2);
            rect(0, 0, 1, 1, 0xFFFFFFFFu);
            method(0x1E70, 0);
            pass(4);
        }
        flip();
    }
    nv2a_vk_wait();
    nv2a_vk_stats(&st);
    nv2a_perf_stats(1e9, &s);
    CHECK(s.frames == 4 && s.gpu_frames == 4 && st.early == was.early + 4);
    all = st.gpu_ms - was.gpu_ms;
    CHECK(all > 0.0 && s.gpu * 4 > all * 0.999 && s.gpu * 4 < all * 1.001);
    for (k = 0; k < NV2A_PERF_PHASES; k++)
        sum += s.phase[k];
    CHECK(sum > s.avg * 0.999 && sum < s.avg * 1.001);
    CHECK(s.phase[NV2A_PERF_WORK] > 0.0);
    CHECK(s.phase[NV2A_PERF_HOLD] == 0.0 && s.phase[NV2A_PERF_TITLE] == 0.0);
    CHECK(s.phase[NV2A_PERF_GPU] > 0.0);
    if (!s_touch) {
        CHECK(s.phase[NV2A_PERF_PIPELINES] > 0.0 && s.phase[NV2A_PERF_NEW_TEXTURES] > 0.0);
        CHECK(s.phase[NV2A_PERF_BACK] > 0.0);
    } else {
        CHECK(s.phase[NV2A_PERF_PIPELINES] == 0.0);  /* the first time made it */
        CHECK(s.phase[NV2A_PERF_BACK] == 0.0);
    }
    CHECK(nv2a_perf_phase_is == NV2A_PERF_WORK && nv2a_perf_part_is == NV2A_PERF_COMMANDS);
    nv2a_perf_set(0);
    flip();
    nv2a_perf_phase(NV2A_PERF_TITLE);
    (void)pixel(0, 0);

    for (k = 0; k < 5; k++) {                   /* others use the GPU too */
        all = two_submissions(2000, 1);
        sum = two_submissions(1, 2000);
        if (sum > all / 4 && all > sum / 4)
            break;
    }
    CHECK(k < 5);
}

/* Texel for pixel: swizzled, and a linear surface drawn just before. */
static void test_texture(void)
{
    uint32_t x, y, mx, my, wrong = 0;

    xbox_swizzle_masks(8, 8, &mx, &my);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            uint32_t v = 0xFF000000u | x << 20 | y << 12 | 0x55u;
            memcpy(s_mem + TEX_AT + 4 * (swizzle_deposit(x, mx) | swizzle_deposit(y, my)), &v, 4);
        }
    clear(0xFF000000u);
    pass(8);
    bind_texture(TEX_AT, 0x06, 3, 3);
    quad(0, 0, 8, 8, 0, 1, 1);

    /* A small target, sampled in the same frame. */
    method(0x1E70, 0);
    pass(4);
    surface(SMALL_AT, 16, 16, 64, 0x0128);
    method(0x1D90, 0xFF00FF00u);
    method(0x1D94, 0xF0);
    rect(0, 0, 4, 16, 0xFF0000FFu);
    main_surface();
    pass(8);
    bind_texture(SMALL_AT, 0x12, 0, 0);
    method(0x1B10, 64u << 16);
    method(0x1B1C, 16u << 16 | 16u);
    quad(20, 0, 36, 16, 0, 16, 16);
    flip();
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            wrong += pixel(x, y) != (0xFF000000u | x << 20 | y << 12 | 0x55u);
    CHECK(wrong == 0);
    CHECK(pixel(21, 3) == 0xFF0000FFu && pixel(25, 3) == 0xFF00FF00u);
    CHECK(pixel(37, 3) == 0xFF000000u);

    /* The title rewrites the texture between two frames. */
    for (x = 0; x < 64; x++) {
        uint32_t v = 0xFFABCDEFu;
        memcpy(s_mem + TEX_AT + 4 * x, &v, 4);
    }
    bind_texture(TEX_AT, 0x06, 3, 3);
    quad(0, 0, 8, 8, 0, 1, 1);
    flip();
    CHECK(pixel(3, 3) == 0xFFABCDEFu);
    method(0x1E70, 0);
    pass(4);
}

/* What the title writes into a surface after a flip stays, under what is
 * drawn next. */
static void test_cpu_writes(void)
{
    uint32_t v = 0xFF777777u;

    clear(0xFF000000u);
    rect(0, 0, 8, 8, 0xFFFF0000u);
    flip();
    memcpy(s_mem + COLOR_AT + 20 * 4, &v, 4);
    rect(10, 0, 12, 8, 0xFF00FF00u);
    flip();
    CHECK(pixel(2, 2) == 0xFFFF0000u && pixel(11, 2) == 0xFF00FF00u);
    CHECK(pixel(20, 0) == v);
}

static void test_lines_and_points(void)
{
    uint32_t x, n = 0;

    clear(0xFF000000u);
    diffuse(0xFFFFFFFFu);
    put_float(POS_AT, 4.5f); put_float(POS_AT + 4, 6.5f);
    put_float(POS_AT + 16, 20.5f); put_float(POS_AT + 20, 6.5f);
    method(0x17FC, 2);
    method(0x1810, 1u << 24);
    method(0x17FC, 0);
    put_float(POS_AT, 40.5f); put_float(POS_AT + 4, 10.5f);
    method(0x17FC, 1);
    method(0x1810, 0);
    method(0x17FC, 0);
    flip();
    for (x = 0; x < W; x++)
        n += pixel(x, 6) == 0xFFFFFFFFu;
    CHECK(n >= 15 && n <= 17);
    CHECK(pixel(40, 10) == 0xFFFFFFFFu);
}

/* How many of w by h pixels from (x, y) are not `back`. */
static unsigned painted(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t back)
{
    uint32_t i, n = 0;

    for (i = 0; i < w * h; i++)
        n += pixel(x + i % w, y + i / w) != back;
    return n;
}

static void corner(unsigned i, float x, float y)
{
    put_float(POS_AT + 16 * i, x);
    put_float(POS_AT + 16 * i + 4, y);
    put_float(POS_AT + 16 * i + 8, 0.0f);
    put_float(POS_AT + 16 * i + 12, 1.0f);
}

static void drawn_as(uint32_t prim, unsigned n)
{
    method(0x17FC, prim);
    method(0x1810, (n - 1) << 24);
    method(0x17FC, 0);
}

static void point_at(float x, float y)
{
    corner(0, x, y);
    drawn_as(1, 1);
}

static void test_sizes(void)
{
    uint32_t x, y, wrong = 0;

    clear(0xFF000000u);
    diffuse(0xFFFFFFFFu);
    method(0x043C, 4 * 8);
    point_at(8, 8);
    method(0x043C, 0x2F0);                       /* too large to be a size */
    point_at(20, 8);
    method(0x0380, 4 * 8);                       /* lines */
    corner(0, 32, 16);
    corner(1, 48, 16);
    drawn_as(2, 2);
    method(0x0380, 8);
    flip();
    CHECK(painted(0, 0, 15, 20, 0xFF000000u) == 16);
    CHECK(pixel(6, 6) == 0xFFFFFFFFu && pixel(9, 9) == 0xFFFFFFFFu);
    CHECK(painted(15, 0, 10, 20, 0xFF000000u) == 1);
    for (y = 0; y < H; y++)
        wrong += pixel(40, y) != (y >= 14 && y < 18 ? 0xFFFFFFFFu : 0xFF000000u);
    CHECK(wrong == 0);

    /* Smooth: what covers a pixel in part is blended in part. */
    for (x = 0; x < 3; x++) {
        clear(0xFF000000u);
        method(0x0304, 1);
        method(0x0344, 0x302);
        method(0x0348, 0x303);
        method(0x0320, x != 0);
        method(0x1D7C, x == 2 ? 0xFFFF0001u : 0xFFFF0000u);
        corner(0, 2, 2);
        corner(1, 50, 20);
        drawn_as(2, 2);
        method(0x0304, 0);
        method(0x0344, 1);
        method(0x0348, 0);
        method(0x0320, 0);
        flip();
        for (wrong = 0, y = 0; y < W * H; y++)
            wrong += pixel(y % W, y / W) != 0xFF000000u && pixel(y % W, y / W) != 0xFFFFFFFFu;
        CHECK((wrong > 30) == (x == 1) && painted(0, 0, W, H, 0xFF000000u) > 40);
    }

    /* Sprites: stage 3 reads the place in the point. */
    xbox_swizzle_masks(4, 4, &x, &y);
    for (wrong = 0; wrong < 16; wrong++) {
        uint32_t v = 0xFF000000u | (wrong & 3) << 20 | (wrong >> 2) << 12 | 0x55u;
        memcpy(s_mem + TEX_AT + 4 * (swizzle_deposit(wrong & 3, x) | swizzle_deposit(wrong >> 2, y)),
               &v, 4);
    }
    clear(0xFF000000u);
    bind_texture(TEX_AT, 0x06, 2, 2);
    for (x = 0x00; x < 0x40; x += 4)
        if (x != 0x24 && x != 0x1C)
            method(0x1BC0 + x, x == 0 ? TEX_AT : x == 4 ? 0x28u | 6u << 8 | 1u << 16 | 2u << 20 | 2u << 24
                   : x == 8 ? 0x00030303u : x == 0xC ? 0x4003FFC0u : x == 0x14 ? 0x01012000u : 0);
    method(0x1E70, 1u << 15);
    pass(11);
    method(0x031C, 1);
    method(0x043C, 8 * 8);
    point_at(10, 10);
    method(0x031C, 0);
    point_at(30, 10);                            /* not a sprite: texel 0, 0 */
    flip();
    for (wrong = 0, y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            wrong += pixel(6 + x, 6 + y) != (0xFF000000u | (x / 2) << 20 | (y / 2) << 12 | 0x55u);
            wrong += pixel(26 + x, 6 + y) != 0xFF000055u;
        }
    CHECK(wrong == 0);
    method(0x1E70, 0);
    method(0x043C, 8);
    pass(4);
}

/* SET_FRONT_POLYGON_MODE and SET_BACK_POLYGON_MODE. */
static void test_fill_modes(void)
{
    const uint32_t black = 0xFF000000u, white = 0xFFFFFFFFu;
    unsigned cull;

    diffuse(white);
    method(0x03A0, 0x900);                       /* clockwise is front */
    for (cull = 0; cull < 2; cull++) {
        clear(black);
        method(0x0308, cull);
        method(0x039C, 0x405);
        method(0x038C, 0x1B01);
        method(0x0390, 0x1B01);
        quad(2.5f, 2.5f, 12.5f, 12.5f, 0, 1, 1);
        corner(0, 22.5f, 2.5f);
        corner(1, 32.5f, 2.5f);
        corner(2, 22.5f, 12.5f);
        corner(3, 32.5f, 12.5f);
        drawn_as(9, 4);
        corner(0, 42.5f, 2.5f);
        corner(1, 52.5f, 2.5f);
        corner(2, 47.5f, 12.5f);
        drawn_as(10, 3);
        flip();
        CHECK(pixel(7, 2) == white && pixel(2, 7) == white && pixel(12, 7) == white
              && pixel(7, 12) == white);
        CHECK((pixel(7, 7) == white) == (cull == 1));   /* the diagonal */
        CHECK(pixel(27, 2) == white && pixel(22, 7) == white && pixel(32, 7) == white
              && pixel(27, 12) == white);
        CHECK((pixel(27, 7) == white) == (cull == 1));
        CHECK(pixel(47, 2) == white && pixel(47, 7) == black);
    }
    /* Corners only. */
    clear(black);
    method(0x0308, 0);
    method(0x038C, 0x1B00);
    method(0x0390, 0x1B00);
    quad(2, 2, 12, 12, 0, 1, 1);
    flip();
    CHECK(painted(0, 0, 20, 20, black) >= 4 && painted(0, 0, 20, 20, black) <= 8);
    CHECK(painted(5, 0, 5, 20, black) == 0 && painted(0, 5, 20, 5, black) == 0);
    /* Front faces filled, back faces as lines, and the other way round. */
    clear(black);
    method(0x038C, 0x1B02);
    method(0x0390, 0x1B01);
    triangle(1, 0);
    triangle(0, 10);
    method(0x038C, 0x1B01);
    method(0x0390, 0x1B02);
    triangle(1, 20);
    triangle(0, 30);
    method(0x0308, 1);                           /* what is culled is not drawn */
    triangle(0, 40);
    method(0x0308, 0);
    flip();
    CHECK(pixel(2, 2) == white && pixel(12, 2) == black && painted(9, 0, 9, 8, black) > 10);
    CHECK(pixel(22, 2) == black && painted(19, 0, 9, 8, black) > 10 && pixel(32, 2) == white);
    CHECK(painted(40, 0, 20, 20, black) == 0);
    /* Lines have an offset of their own to turn on. */
    method(0x038C, 0x1B01);
    method(0x0390, 0x1B01);
    method(0x030C, 1);
    method(0x035C, 1);
    method(0x0354, 0x201);
    method(0x0384, 0);
    method_float(0x0388, -100.0f);
    for (cull = 0; cull < 3; cull++) {
        clear(black);
        method(0x1D8C, 0);                       /* depth 0: only less passes */
        method(0x0338, cull == 1);
        method(0x0334, cull == 2);
        method_float(0x0388, cull ? -100.0f : 0.0f);
        corner(0, 2.5f, 2.5f);
        corner(1, 12.5f, 2.5f);
        corner(2, 2.5f, 12.5f);
        method(0x1D8C, 0x00000A00u);
        method(0x1D94, 0x01);
        put_float(POS_AT + 8, 10.0f);
        put_float(POS_AT + 24, 10.0f);
        put_float(POS_AT + 40, 10.0f);
        drawn_as(5, 3);
        flip();
        CHECK((pixel(7, 2) == white) == (cull == 2));
    }
    method(0x030C, 0);
    method(0x0334, 0);
    method(0x0338, 0);
    method(0x0388, 0);
    method(0x0354, 0x207);
    method(0x038C, 0x1B02);
    method(0x0390, 0x1B02);
    method(0x03A0, 0x901);
}

/* A texture with border texels of its own: 4 of them around 4 by 4, in 16 by
 * 16. */
static void test_border(void)
{
    uint32_t x, y, mx, my, wrong = 0;

    xbox_swizzle_masks(16, 16, &mx, &my);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            uint32_t v = 0xFF000000u | x << 20 | y << 12 | 0x55u;
            memcpy(s_mem + TEX_AT + 4 * (swizzle_deposit(x, mx) | swizzle_deposit(y, my)), &v, 4);
        }
    clear(0xFF000000u);
    pass(8);
    bind_texture(TEX_AT, 0x06, 2, 2);
    method(0x1B04, 0x20u | 6u << 8 | 1u << 16 | 2u << 20 | 2u << 24);
    method(0x1B08, 0x00040404u);
    quad(0, 0, 12, 12, 0, 1, 1);
    {
        const float t[8] = { -1, -1,  2, -1,  2, 2,  -1, 2 };
        for (x = 0; x < 8; x++)
            put_float(UV_AT + 4 * x, t[x]);
        drawn_as(8, 4);
    }
    method(0x1B08, 0x00010101u);                 /* repeat: none of the border */
    quad(20, 0, 28, 8, 0, 2, 2);
    flip();
    for (y = 0; y < 12; y++)
        for (x = 0; x < 12; x++)
            wrong += pixel(x, y) != (0xFF000000u | x << 20 | y << 12 | 0x55u);
    CHECK(wrong == 0);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            wrong += pixel(20 + x, y) != (0xFF000000u | (4 + x % 4) << 20 | (4 + y % 4) << 12 | 0x55u);
    CHECK(wrong == 0);
    method(0x1E70, 0);
    pass(4);
}

static uint32_t s_seed = 1;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 8 ^ s_seed << 16;
}

/* An input word: registers that exist, any mapping and component. */
static uint32_t random_inputs(void)
{
    static const uint8_t reg[] = { 0, 1, 2, 4, 5, 12, 13 };
    uint32_t v = 0, i;

    for (i = 0; i < 4; i++)
        v = v << 8 | reg[rnd() % sizeof reg] | (rnd() & 0xF0u);
    return v;
}

/* Outputs to three different registers, or nowhere. */
static uint32_t random_outputs(int color)
{
    static const uint8_t reg[] = { 4, 5, 12, 13 };
    static const uint32_t scale[] = { 0, 0x8000, 0x10000, 0x18000, 0x20000, 0x30000 };
    uint32_t first = rnd() % 4, v;

    v = reg[first] << 8;
    if (rnd() & 1) v |= reg[(first + 1) % 4] << 4;
    if (rnd() & 1) v |= reg[(first + 2) % 4];
    v |= scale[rnd() % 6];
    if (color && (rnd() & 3) == 0) {             /* a dot product has no sum */
        v = (v & ~0xF00u) | 0x2000u;
        if (rnd() & 1) v |= 0x1000u;
    }
    return v;
}

/* The same random programs through the CPU evaluator. Inputs are constants and
 * vertex colours in whole bytes. */
static void test_combiners(void)
{
    unsigned round, i, k, bad = 0;

    for (round = 0; round < 400; round++) {
        Nv2aCombiner rc;
        uint32_t c0 = rnd(), c1 = rnd(), got, want;
        float v0[4], v1[4], fog[4] = { 0, 0, 0, 1 }, t[4][4] = { { 0 } }, out[4];

        memset(&rc, 0, sizeof rc);
        for (i = 0; i < 4; i++)
            t[i][3] = 1.0f;
        rc.control = (1 + rnd() % 4) | 0x11000u | (rnd() & 0x100u);
        for (i = 0; i < 8; i++) {
            rc.color_icw[i] = random_inputs();
            rc.alpha_icw[i] = random_inputs();
            rc.color_ocw[i] = random_outputs(1);
            rc.alpha_ocw[i] = random_outputs(0);
            rc.factor0[i] = rnd();
            rc.factor1[i] = rnd();
            method(0x0AC0 + 4 * i, rc.color_icw[i]);
            method(0x0260 + 4 * i, rc.alpha_icw[i]);
            method(0x1E40 + 4 * i, rc.color_ocw[i]);
            method(0x0AA0 + 4 * i, rc.alpha_ocw[i]);
            method(0x0A60 + 4 * i, rc.factor0[i]);
            method(0x0A80 + 4 * i, rc.factor1[i]);
        }
        rc.final0 = (random_inputs() & 0x1F1F1F1Fu) | 0x20000000u * (rnd() & 1);
        if (rnd() & 1)
            rc.final0 = (rc.final0 & ~0x1F00u) | 0x0F00u;    /* C: E * F */
        rc.final1 = (random_inputs() & 0x1F1F1F00u) | (rnd() & 0xE0u);
        rc.final_c0 = rnd();
        rc.final_c1 = rnd();
        method(0x1E60, rc.control);
        method(0x0288, rc.final0);
        method(0x028C, rc.final1);
        method(0x1E20, rc.final_c0);
        method(0x1E24, rc.final_c1);
        diffuse(c0);
        for (i = 0; i < 4; i++)
            method_float(0x1A00 + 16 * 4 + 4 * i,
                         (float)((c1 >> (i == 3 ? 24 : 16 - 8 * i)) & 0xFF) / 255.0f);
        clear(0);
        quad(0, 0, 8, 8, 0, 1, 1);
        flip();
        got = pixel(3, 3);
        nv2a_rc_unpack(c0, v0);
        nv2a_rc_unpack(c1, v1);
        nv2a_rc_eval(&rc, v0, v1, fog, (const float (*)[4])t, out);
        want = 0;
        for (i = 0; i < 4; i++)
            want |= (uint32_t)(out[i] * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);
        if (!close_to(got, want, 2) && bad++ < 8) {
            fprintf(stderr, "FAIL combiners %u: %08X, the CPU says %08X (control %X, final"
                    " %08X %08X, v0 %08X v1 %08X)\n", round, got, want, rc.control,
                    rc.final0, rc.final1, c0, c1);
            for (k = 0; k < (rc.control & 15); k++)
                fprintf(stderr, "  %u: %08X %08X -> %08X %08X, %08X %08X\n", k, rc.color_icw[k],
                        rc.alpha_icw[k], rc.color_ocw[k], rc.alpha_ocw[k], rc.factor0[k],
                        rc.factor1[k]);
        }
    }
    if (bad) {
        fprintf(stderr, "%u of %u programs differ\n", bad, round);
        s_fail = 1;
    }
    for (i = 0; i < 4; i++)
        method_float(0x1A00 + 16 * 4 + 4 * i, 0);
    pass(4);
}

/* Register 14, spare0 plus secondary. NV_register_combiners maps both with
 * UNSIGNED_IDENTITY first, so a negative spare0 adds nothing (the CPU
 * evaluator adds it as it is, which is why the programs above leave it out). */
static void test_color_sum(void)
{
    unsigned i;

    pass(14);
    for (i = 0; i < 4; i++)
        method_float(0x1A00 + 16 * 4 + 4 * i, 64.0f / 255.0f);
    method(0x0A60, 0xFF000000u);
    diffuse(0xFF102030u);
    method(0x1E40, 12u << 4);                    /* spare0 = A * B */
    method(0x028C, 0x00001400u);
    clear(0);
    method(0x0AC0, 0x04200000u);                 /* diffuse * 1 */
    quad(0, 0, 8, 8, 0, 1, 1);
    method(0x0AC0, 0xE4200000u);                 /* -diffuse * 1 */
    quad(10, 0, 18, 8, 0, 1, 1);
    method(0x0AC0, 0x04200000u);
    method(0x028C, 0x00001440u);                 /* 1 - secondary */
    quad(20, 0, 28, 8, 0, 1, 1);
    method(0x028C, 0x00001480u);                 /* and a sum that clamps */
    method(0x1E40, 12u << 4 | 0x20000u);
    quad(30, 0, 38, 8, 0, 1, 1);
    flip();
    CHECK(close_to(pixel(3, 3), 0xFF506070u, 1));
    CHECK(close_to(pixel(13, 3), 0xFF404040u, 1));
    CHECK(close_to(pixel(23, 3), 0xFFCFDFEFu, 1));
    CHECK(close_to(pixel(33, 3), 0xFF80C0FFu, 1));
    for (i = 0; i < 4; i++)
        method_float(0x1A00 + 16 * 4 + 4 * i, 0);
    pass(4);
}

/* ---- vertex programs on the GPU ----------------------------------------- */

typedef struct { uint32_t mux, reg, swz, neg; } Src;     /* 1 R, 2 v, 3 c */

#define XYZW 0x1Bu
#define XXXX 0x00u
#define YYYY 0x55u

/* Instruction words, nv2a_vsh_interp.c's table. */
typedef struct {
    uint32_t mac, ilu, input, constant, relative, tdst, tmask, imask, oaddr, omask,
             from_ilu, to_c, final;
    Src a, b, c;
} Ins;

static void program_begin(void)
{
    method(0x1E9C, 0);
}

static void put_ins(const Ins *i)
{
    method(0x0B00, 0);
    method(0x0B04, i->ilu << 25 | i->mac << 21 | i->constant << 13 | i->input << 9
                 | i->a.neg << 8 | i->a.swz);
    method(0x0B08, i->a.reg << 28 | i->a.mux << 26 | i->b.neg << 25 | i->b.swz << 17
                 | i->b.reg << 13 | i->b.mux << 11 | i->c.neg << 10 | i->c.swz << 2
                 | i->c.reg >> 2);
    method(0x0B0C, (i->c.reg & 3) << 30 | i->c.mux << 28 | i->tmask << 24 | i->tdst << 20
                 | i->imask << 16 | i->omask << 12 | (i->to_c ? 0 : 1u << 11)
                 | i->oaddr << 3 | i->from_ilu << 2 | i->relative << 1 | i->final);
}

/* MOV o[to], v[from] */
static void pass_attribute(uint32_t to, uint32_t from, int final)
{
    Ins i = { 0 };

    i.mac = 1;
    i.input = from;
    i.a.mux = 2;
    i.a.swz = XYZW;
    i.oaddr = to;
    i.omask = 15;
    i.final = final != 0;
    put_ins(&i);
}

static void constant(uint32_t n, float x, float y, float z, float w)
{
    method(0x1EA4, n);
    method_float(0x0B80, x);
    method_float(0x0B84, y);
    method_float(0x0B88, z);
    method_float(0x0B8C, w);
}

static void current(uint32_t attr, float x, float y, float z, float w)
{
    method_float(0x1A00 + 16 * attr, x);
    method_float(0x1A04 + 16 * attr, y);
    method_float(0x1A08 + 16 * attr, z);
    method_float(0x1A0C + 16 * attr, w);
}

/* Colours of a back face, the specular switches and a program's point size. */
static void test_two_sided(void)
{
    unsigned i, cpu, flat;

    program_begin();
    pass_attribute(0, 0, 0);
    pass_attribute(3, 3, 0);
    pass_attribute(4, 4, 0);
    pass_attribute(6, 6, 0);
    pass_attribute(7, 7, 0);
    pass_attribute(8, 8, 1);
    current(3, 1, 0, 0, 1);
    current(4, 0, 1, 0, 0.25f);
    current(7, 0, 0, 1, 1);
    current(8, 1, 1, 0, 0.25f);
    method(0x03A0, 0x900);                       /* clockwise is front */
    for (cpu = 0; cpu < 2; cpu++)
        for (flat = 0; flat < 2; flat++) {
            nv2a_vk_cpu_vertices((int)cpu);
            method(0x037C, flat ? 0x1D00 : 0x1D01);
            clear(0xFF000000u);
            for (i = 0; i < 2; i++) {
                method(0x17C4, i);
                pass(4);
                triangle(1, 0.0f + 20.0f * (float)i);
                triangle(0, 10.0f + 20.0f * (float)i);
            }
            pass(5);
            triangle(1, 40);
            triangle(0, 50);
            flip();
            CHECK(pixel(1, 1) == 0xFFFF0000u && pixel(11, 1) == 0xFFFF0000u);
            CHECK(pixel(21, 1) == 0xFFFF0000u && pixel(31, 1) == 0xFF0000FFu);
            CHECK(pixel(41, 1) == 0x4000FF00u && pixel(51, 1) == 0x40FFFF00u);

            clear(0xFF000000u);
            for (i = 0; i < 3; i++) {            /* off; no alpha of its own; all */
                method(0x03B8, i != 0);
                method(0x0294, i == 1 ? 1u : 0x20001u);
                triangle(1, 0.0f + 20.0f * (float)i);
                triangle(0, 10.0f + 20.0f * (float)i);
            }
            method(0x17C4, 0);
            flip();
            CHECK(pixel(1, 1) == 0xFF000000u && pixel(11, 1) == 0xFF000000u);
            CHECK(pixel(21, 1) == 0xFF00FF00u && pixel(31, 1) == 0xFFFFFF00u);
            CHECK(pixel(41, 1) == 0x4000FF00u && pixel(51, 1) == 0x40FFFF00u);

            pass(4);
            current(6, 6, 0, 0, 0);
            clear(0x00000000u);
            point_at(8, 8);                      /* the title's, 1 */
            method(0x0318, 1);
            point_at(24, 8);
            method(0x0318, 0);
            flip();
            CHECK(painted(0, 0, 16, 16, 0) == 1);
            CHECK(painted(16, 0, 16, 16, 0) == 36);
        }
    nv2a_vk_cpu_vertices(0);
    method(0x037C, 0x1D01);
    method(0x03A0, 0x901);
    method(0x0294, 0x20001u);
    current(4, 0, 0, 0, 0);
    setup();
}

static uint32_t to_argb(const float c[4])
{
    uint32_t v = 0, i;

    for (i = 0; i < 4; i++) {
        float f = c[i] > 1.0f ? 1.0f : c[i] >= 0.0f ? c[i] : 0.0f;
        v |= (uint32_t)(f * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);
    }
    return v;
}

static float small_float(void) { return (float)((int)(rnd() % 4001) - 2000) / 1000.0f; }

static Src random_src(void)
{
    Src s;

    s.mux = 1 + rnd() % 3;
    s.reg = rnd() % 12;
    s.swz = rnd() & 0xFF;
    s.neg = rnd() & 1;
    return s;
}

static int sane(const float *f, unsigned n)
{
    while (n--)
        if (!(f[n] > -1e5f && f[n] < 1e5f))
            return 0;
    return 1;
}

/* Random programs against the CPU interpreter, which the executor has fed the
 * same words. The result is scaled into a colour. What overflows on the way is
 * left out: the interpreter has no "0 times anything is 0". A step may also
 * fall either way for two roundings, so a few may differ. */
/* The texture stages' programs past the bump maps: stage 0 reads C8884828 all
 * over and the rest is worked out by hand. */
#define CUBE_AT   0x60000u
#define VOLUME_AT 0x68000u
static void bind_stage(unsigned st, uint32_t at, uint32_t format, uint32_t log2)
{
    uint32_t to = 0x1B00 + 0x40 * st;

    method(to, at);
    method(to + 0x04, 0x08u | format | 1u << 16 | log2 << 20 | log2 << 24);
    method(to + 0x08, 0x00030303u);
    method(to + 0x0C, 0x4003FFC0u);
    method(to + 0x14, 0x01012000u);
}

static uint32_t staged(unsigned st, uint32_t modes)
{
    method(0x1E70, modes);
    pass(8 + st);
    clear(0x12345678u);
    quad(0, 0, 8, 8, 0, 1, 1);
    flip();
    return pixel(3, 3);
}

static void test_stages(void)
{
    static const struct { uint32_t mapping; float s[3], t[3]; } k[] = {
        { 0x00, { 1, 0, 0 },     { 0, 1, 0 } },
        { 0x11, { 0, -1.2f, 0 }, { 0, 0, -0.4f } },
        { 0x22, { 0, 0.93f, 0 }, { -0.3f, 0, 0 } },
        { 0x33, { -0.56f, 0, 0 }, { 0, 0, 0.88f } },
        { 0x44, { 0.68f, 0, 0 }, { 0, 1, 0 } },
        { 0x40, { 1, 0, 0 },     { 0, 0, 0.28f } },
        { 0x01, { 8.9f, 0, 0 },  { 0, 1, 0 } },
    };
    static const uint32_t colors[6] = { 0xFFFF0000u, 0xFF800000u, 0xFF00FF00u, 0xFF008000u,
                                        0xFF0000FFu, 0xFF000080u };
    static const uint32_t attr[7] = { 0, 3, 4, 9, 10, 11, 12 };
    uint32_t x, y, z, mx, my, v;
    unsigned round, i;

    program_begin();
    for (i = 0; i < 7; i++)
        pass_attribute(attr[i], attr[i], i == 6);
    xbox_swizzle_masks(16, 16, &mx, &my);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            v = 0xFF000000u | x << 4 | y << 12;
            memcpy(s_mem + TEX_AT + 4 * (swizzle_deposit(x, mx) | swizzle_deposit(y, my)), &v, 4);
        }
    v = 0xC8884828u;
    memcpy(s_mem + TEX_AT + 0x1000, &v, 4);
    for (i = 0; i < 6 * 256; i++)
        memcpy(s_mem + CUBE_AT + 4 * i, &colors[i / 256], 4);
    for (z = 0; z < 4; z++)
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                v = 0xFF000000u | x << 4 | y << 12 | z << 20;
                memcpy(s_mem + VOLUME_AT + 4 * xbox_swizzle_offset_3d(x, y, z, 4, 4, 4), &v, 4);
            }
    bind_stage(0, TEX_AT + 0x1000, 0x0620, 0);

    bind_stage(2, TEX_AT, 0x0620, 4);            /* two dot products */
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        method(0x1E74, k[i].mapping);
        current(10, k[i].s[0], k[i].s[1], k[i].s[2], 1);
        current(11, k[i].t[0], k[i].t[1], k[i].t[2], 1);
        CHECK(staged(2, 1 | 17 << 5 | 9 << 10) == 0xFF004080u);
    }
    method(0x1E74, 0);
    CHECK(staged(1, 1 | 17 << 5 | 9 << 10) == 0);       /* a dot product's own colour */
    bind_stage(1, TEX_AT, 0x0620, 4);            /* at a texel */
    CHECK(staged(1, 1 | 15 << 5) == 0xFF0080C0u);
    CHECK(staged(1, 1 | 16 << 5) == 0xFF002040u);

    current(10, 1, 0, 0, 1);                     /* three: a volume */
    current(11, 0, 1, 0, 1);
    current(12, 1, 0, 2.1f, 1);
    bind_stage(3, VOLUME_AT, 0x0630 | 2u << 28, 2);
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 13 << 15) == 0xFF301020u);
    for (i = 0; i < 4; i++) {                    /* of blocks, each of one colour */
        static const uint32_t block[4][2] = { { 0xF800F800u, 0 }, { 0x07E007E0u, 0 },
                                              { 0x001F001Fu, 0 }, { 0xFFFFFFFFu, 0 } };

        memcpy(s_mem + VOLUME_AT + 0x1000 + 8 * i, block[i], 8);
    }
    bind_stage(3, VOLUME_AT + 0x1000, 0x0C30 | 2u << 28, 2);
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 13 << 15) == 0xFFFFFFFFu);
    current(12, 1, 0, 0.5f, 1);                  /* .61 */
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 13 << 15) == 0xFF0000FFu);
    current(12, 1, 0, 2.1f, 1);
    bind_stage(3, CUBE_AT, 0x0624, 4);           /* a cube map */
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 14 << 15) == colors[4]);
    current(12, 1, 0, -10, 1);
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 14 << 15) == colors[5]);
    current(10, 0, 0, 0, 1);                     /* (1, 0.2, 0.3) at (0, 0, n) */
    current(11, 0, 0, 0, 0.2f);
    current(12, 1, 0, 0, 0.3f);
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 12 << 15) == colors[1]);
    method_float(0x181C, 0.1f);
    method_float(0x1820, 1.0f);
    method_float(0x1824, 0.2f);
    CHECK(staged(3, 1 | 17 << 5 | 17 << 10 | 18 << 15) == colors[3]);
    bind_stage(2, CUBE_AT, 0x0624, 4);           /* with the stage after's */
    current(12, -1, 0, 0, 0.3f);
    CHECK(staged(2, 1 | 17 << 5 | 11 << 10 | 17 << 15) == colors[5]);

    /* z / w as the depth, against one halfway; in a w-buffer too. */
    for (round = 0; round < 2; round++) {
        method(0x1E70, 1 | 17 << 5 | 10 << 10);
        pass(4);
        method(0x030C, 1);
        method(0x035C, 1);
        method(0x0354, 0x201);
        method(0x1D90, 0xFF000000u);
        method(0x1D8C, 0x80000000u);
        method(0x1D94, 0xF3);
        if (round) {
            method(0x0290, 0x10000);
            method(0x1E70, 0);
            s_quad_w = 5000.0f;
            quad(60, 28, 64, 32, 0, 1, 1);
            s_quad_w = 1.0f;
            method(0x0290, 0);
            method(0x1E70, 1 | 17 << 5 | 10 << 10);
        }
        diffuse(0xFFFF0000u);
        current(11, 0, 7.082f, 0, 1);                /* w is 2 */
        current(10, 1.6e7f, 0, 0, 1);
        quad(0, 0, 8, 8, 16000000.0f, 1, 1);         /* 4.3 million */
        current(10, 3.2e7f, 0, 0, 1);
        quad(10, 0, 18, 8, 0.0f, 1, 1);              /* 8.5: behind */
        method(0x0354, 0x207);
        current(10, 8e7f, 0, 0, 1);
        quad(20, 0, 28, 8, 0.0f, 1, 1);              /* beyond the buffer's range */
        method(0x1D78, 0x10);
        quad(30, 0, 38, 8, 0.0f, 1, 1);              /* and held within it */
        method(0x1D78, 0);
        method(0x0354, 0x201);
        diffuse(0xFF00FF00u);                        /* what was written */
        method(0x1E70, 0);
        quad(0, 0, 4, 8, 4200000.0f, 1, 1);
        quad(4, 0, 8, 8, 4400000.0f, 1, 1);
        quad(30, 0, 34, 8, 16700000.0f, 1, 1);
        flip();
        CHECK(pixel(2, 2) == 0xFF00FF00u && pixel(6, 2) == 0xFFFF0000u);
        CHECK(pixel(12, 2) == 0xFF000000u && pixel(22, 2) == 0xFF000000u);
        CHECK(pixel(32, 2) == 0xFF00FF00u && pixel(36, 2) == 0xFFFF0000u);
    }
    method(0x030C, 0);
    method(0x035C, 0);
    setup();
}

static void test_programs(void)
{
    unsigned round, i, bad = 0, done = 0;
    Nv2aVkStats was, st;

    nv2a_vk_stats(&was);
    constant(100, 0.125f, 0.5f, 0, 0);
    for (i = 1; i < 16; i++)                     /* current values */
        method(0x1760 + 4 * i, 0);
    for (round = 0; round < 1000; round++) {
        float in[NV2A_VSH_INPUTS][4] = { { 1, 1, 1, 1 } };
        unsigned count = 2 + rnd() % 10;
        Nv2aVshOutput out;
        uint32_t got, want, k = rnd() % 12;
        Ins s;

        for (i = 1; i < 16; i++) {
            unsigned j;
            for (j = 0; j < 4; j++)
                in[i][j] = small_float();
            current(i, in[i][0], in[i][1], in[i][2], in[i][3]);
        }
        for (i = 0; i < 16; i++)
            constant(i, small_float(), small_float(), small_float(), small_float());
        program_begin();
        pass_attribute(0, 0, 0);
        for (i = 0; i < count; i++) {
            memset(&s, 0, sizeof s);
            s.mac = rnd() % 14;
            s.ilu = rnd() & 1 ? rnd() % 8 : 0;
            s.input = 1 + rnd() % 15;
            s.constant = rnd() % 16;
            s.relative = (rnd() & 7) == 0;
            s.a = random_src();
            s.b = random_src();
            s.c = random_src();
            s.tdst = rnd() % 12;
            s.tmask = rnd() & 15;
            s.imask = rnd() & 15;
            put_ins(&s);
        }
        memset(&s, 0, sizeof s);
        s.mac = 1;                               /* MOV o[9], Rk: unscaled */
        s.a.mux = 1;
        s.a.reg = k;
        s.a.swz = XYZW;
        s.oaddr = 9;
        s.omask = 15;
        put_ins(&s);
        s.mac = 4;                               /* MAD o[3], Rk, 1/8, 1/2 */
        s.constant = 100;
        s.b.mux = 3;
        s.b.swz = XXXX;
        s.c.mux = 3;
        s.c.swz = YYYY;
        s.oaddr = 3;
        s.final = 1;
        put_ins(&s);
        method(0x1EA0, 0);
        if (!nv2a_vsh_run((const float (*)[4])in, &out) || !sane(out.tex[0], 4))
            continue;
        done++;
        clear(0);
        quad(0, 0, 8, 8, 0, 1, 1);
        flip();
        got = pixel(3, 3);
        want = to_argb(out.d0);
        if (!close_to(got, want, 1) && bad++ < 8)
            fprintf(stderr, "program %u: %08X, the CPU says %08X (R%u %g %g %g %g)\n", round,
                    got, want, k, out.tex[0][0], out.tex[0][1], out.tex[0][2], out.tex[0][3]);
    }
    nv2a_vk_stats(&st);
    CHECK(done > 500 && bad * 100 <= done);
    CHECK(st.programs - was.programs == done);
    printf("programs: %u compared, %u differ\n", done, bad);
}

/* One operation on the ILU, times `scale`, plus `add`. */
static uint32_t ilu_plus(uint32_t op, float x, float y, float w, float scale, float add)
{
    Ins s = { 0 };

    current(5, x, y, 0, w);
    constant(101, scale, add, 0, 0);
    program_begin();
    pass_attribute(0, 0, 0);
    s.ilu = op;
    s.input = 5;
    s.c.mux = 2;
    s.c.swz = XYZW;
    s.tdst = 2;
    s.imask = 15;
    put_ins(&s);
    memset(&s, 0, sizeof s);
    s.mac = 4;                                   /* MAD o[3], R2, scale, add */
    s.constant = 101;
    s.a.mux = 1;
    s.a.reg = 2;
    s.a.swz = XYZW;
    s.b.mux = 3;
    s.b.swz = XXXX;
    s.c.mux = 3;
    s.c.swz = YYYY;
    s.oaddr = 3;
    s.omask = 15;
    s.final = 1;
    put_ins(&s);
    method(0x1EA0, 0);
    clear(0);
    quad(0, 0, 8, 8, 0, 1, 1);
    flip();
    return pixel(3, 3);
}

static uint32_t ilu_result(uint32_t op, float x, float y, float w, float scale)
{
    return ilu_plus(op, x, y, w, scale, 0);
}

/* A position whose x and y alone are written has 0 and 1 for the others. */
static void position_in_part(void)
{
    Ins i = { 0 };

    program_begin();
    i.mac = 1;                                  /* MOV oPos.xy, v0 */
    i.a.mux = 2;
    i.a.swz = XYZW;
    i.omask = 12;
    put_ins(&i);
    i.a.mux = 1;                                /* MOV oD0, R12 */
    i.a.reg = 12;
    i.a.swz = 0xAF;                             /* zzww */
    i.oaddr = 3;
    i.omask = 15;
    i.final = 1;
    put_ins(&i);
    method(0x1EA0, 0);
    clear(0xFF808080u);
    quad(0, 0, 8, 8, 0.5f, 1, 1);
    flip();
    CHECK(pixel(3, 3) == 0xFF0000FFu);
}

/* What NV_vertex_program says of the corners. */
static void test_program_corners(void)
{
    /* 0 times infinity is 0; infinity itself is drawn white. */
    CHECK(ilu_plus(2, 0.0f, 0, 0, 0.0f, 0.25f) == 0x40404040u);
    CHECK(ilu_result(2, 0.0f, 0, 0, 1.0f) == 0xFFFFFFFFu);
    CHECK(ilu_result(2, -0.0f, 0, 0, 1.0f) == 0x00000000u);     /* -infinity */
    CHECK(ilu_result(2, 4.0f, 0, 0, 1.0f) == 0x40404040u);
    /* RCC stops at 2^64, RSQ takes the absolute value. */
    CHECK(ilu_result(3, 0.0f, 0, 0, 5.421010862427522e-20f * 0.25f) == 0x40404040u);
    CHECK(ilu_result(4, -16.0f, 0, 0, 1.0f) == 0x40404040u);
    /* EXP 2.5: 4, 0.5, 5.657, 1. LOG 0.75: -1, 1.5, -0.415, 1. */
    CHECK(close_to(ilu_result(5, 2.5f, 0, 0, 0.125f), 0x208010B4u, 1));
    CHECK(close_to(ilu_result(6, -0.75f, 0, 0, -0.5f), 0x00800035u, 1));
    CHECK(close_to(ilu_result(6, 0.75f, 0, 0, 0.5f), 0x8000BF00u, 1));
    /* LIT: 1, n.l, n.h ^ power, 1; none of the last with the light behind. */
    CHECK(close_to(ilu_result(7, 0.5f, 0.5f, 2.0f, 1.0f), 0xFFFF8040u, 1));
    CHECK(close_to(ilu_result(7, -0.5f, 0.5f, 2.0f, 1.0f), 0xFFFF0000u, 1));
    CHECK(close_to(ilu_result(7, 0.5f, 0.0f, 0.0f, 1.0f), 0xFFFF80FFu, 1));
    position_in_part();
}

/* Attribute 5 in every format, at `at`, shown as the diffuse colour. */
static uint32_t format_result(uint32_t format, uint32_t at, const void *data, uint32_t bytes)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        memcpy(s_mem + at + i * 32, data, bytes);
    method(0x1720 + 4 * 5, at);
    method(0x1760 + 4 * 5, format | 32u << 8);
    clear(0);
    quad(0, 0, 8, 8, 0, 1, 1);
    flip();
    return pixel(3, 3);
}

static void test_vertex_formats(void)
{
    const float f[4] = { 0.25f, 0.5f, 0.75f, 0.125f };
    const uint8_t u[4] = { 0x10, 0x20, 0x30, 0x40 };
    const int16_t s[4] = { 8192, 16384, 24575, 4096 }, w[4] = { 1, 0, 1, 1 };
    const uint32_t c = 0x80FF4020u, cmp = 511u | 1023u << 11 | 255u << 22;
    const uint32_t at = 0x60000u;
    unsigned off;
    Nv2aVkStats was, st;

    program_begin();
    pass_attribute(0, 0, 0);
    pass_attribute(3, 5, 1);
    method(0x1EA0, 0);
    nv2a_vk_stats(&was);
    for (off = 0; off < 4; off++) {              /* any alignment */
        CHECK(close_to(format_result(0x42, at + off, f, 16), 0x204080BFu, 1));
        CHECK(close_to(format_result(0x32, at + off, f, 12), 0xFF4080BFu, 1));
        CHECK(close_to(format_result(0x22, at + off, f, 8), 0xFF408000u, 1));
        CHECK(close_to(format_result(0x12, at + off, f, 4), 0xFF400000u, 1));
        CHECK(format_result(0x44, at + off, u, 4) == 0x40102030u);
        CHECK(format_result(0x34, at + off, u, 3) == 0xFF102030u);
        CHECK(format_result(0x24, at + off, u, 2) == 0xFF102000u);
        CHECK(format_result(0x40, at + off, &c, 4) == c);
        CHECK(close_to(format_result(0x41, at + off, s, 8), 0x204080BFu, 1));
        CHECK(close_to(format_result(0x31, at + off, s, 6), 0xFF4080BFu, 1));
        CHECK(close_to(format_result(0x21, at + off, s, 4), 0xFF408000u, 1));
        CHECK(format_result(0x45, at + off, w, 8) == 0xFFFF00FFu);
        CHECK(close_to(format_result(0x16, at + off, &cmp, 4), 0xFF80FF80u, 1));
    }
    CHECK(close_to(format_result(0x72, at, f, 12), 0xFF4080BFu, 1));    /* 3W */
    method(0x1720 + 4 * 5, 0);                   /* no address: 0, 0, 0, 1 */
    method(0x1760 + 4 * 5, 0x42u | 32u << 8);
    clear(0);
    quad(0, 0, 8, 8, 0, 1, 1);
    flip();
    CHECK(pixel(3, 3) == 0xFF000000u);
    nv2a_vk_stats(&st);
    CHECK(st.programs - was.programs == st.draws - was.draws);
    method(0x1760 + 4 * 5, 0);
}

static void index16(uint32_t a, uint32_t b) { method(0x1800, a | b << 16); }

/* Indices that are few and far apart, and the two ways without arrays. */
static void test_vertex_sources(void)
{
    static const uint32_t index[4] = { 10, 9000, 4, 8000 };
    const float p[16] = { 0, 0, 0, 1,  8, 0, 0, 1,  8, 8, 0, 1,  0, 8, 0, 1 };
    const uint32_t far_at = 0x80000u;
    Nv2aVkStats was, st;
    unsigned i;

    program_begin();
    pass_attribute(0, 0, 0);
    pass_attribute(3, 3, 1);
    method(0x1EA0, 0);
    diffuse(0xFF336699u);
    nv2a_vk_stats(&was);
    method(0x1720, far_at);
    for (i = 0; i < 4; i++)
        memcpy(s_mem + far_at + index[i] * 16, p + 4 * i, 16);
    clear(0);
    method(0x17FC, 8);
    index16(index[0], index[1]);
    index16(index[2], index[3]);
    method(0x17FC, 0);
    flip();
    CHECK(pixel(3, 3) == 0xFF336699u && pixel(9, 3) == 0);
    nv2a_vk_stats(&st);
    CHECK(st.programs - was.programs == 1 && st.vertex_bytes - was.vertex_bytes < 1000);

    /* The same in runs of words, 16 and 32 bits an index. */
    {
        const uint32_t pairs[2] = { index[0] | index[1] << 16, index[2] | index[3] << 16 };

        CHECK(!nv2a_pb_exec_run(0x1810, pairs, 2) && !nv2a_pb_exec_run(0x1800, pairs, 0));
        CHECK(nv2a_pb_exec_run(0x1800, pairs, 2));       /* outside: nothing */
        for (i = 0; i < 2; i++) {
            clear(0);
            method(0x17FC, 8);
            CHECK(i ? nv2a_pb_exec_run(0x1808, index, 4) : nv2a_pb_exec_run(0x1800, pairs, 2));
            method(0x17FC, 0);
            flip();
            CHECK(pixel(3, 3) == 0xFF336699u && pixel(9, 3) == 0);
        }
        nv2a_vk_stats(&was);
    }

    /* SET_VERTEX4F and SET_VERTEX_DATA4UB between BEGIN and END */
    clear(0);
    method(0x17FC, 8);
    method(0x1940 + 4 * 3, 0xFF2010F0u);         /* diffuse, R first */
    for (i = 0; i < 16; i++)
        method_float(0x1518 + 4 * (i & 3), p[i]);
    method(0x17FC, 0);
    flip();
    CHECK(pixel(3, 3) == 0xFFF01020u && pixel(9, 3) == 0);

    /* INLINE_ARRAY: position and a D3DCOLOR */
    method(0x1760 + 4 * 9, 0);
    method(0x1760 + 4 * 3, 0x40);
    clear(0);
    method(0x17FC, 8);
    for (i = 0; i < 16; i++) {
        method_float(0x1818, p[i]);
        if ((i & 3) == 3)
            method(0x1818, 0xFF00C0FFu);
    }
    method(0x17FC, 0);
    flip();
    CHECK(pixel(3, 3) == 0xFF00C0FFu && pixel(9, 3) == 0);
    {
        uint32_t words[20];

        for (i = 0; i < 4; i++) {
            memcpy(words + 5 * i, p + 4 * i, 16);
            words[5 * i + 4] = 0xFFC000FFu;
        }
        clear(0);
        method(0x17FC, 8);
        CHECK(nv2a_pb_exec_run(0x1818, words, 20));
        method(0x17FC, 0);
        flip();
        CHECK(pixel(3, 3) == 0xFFC000FFu && pixel(9, 3) == 0);
    }
    nv2a_vk_stats(&st);
    CHECK(st.programs - was.programs == 3);
    method(0x1760 + 4 * 3, 0);
    method(0x1760 + 4 * 9, PB_FMT_F(2u, 8u));
    method(0x1720, POS_AT);
}

/* A program that writes a constant runs on the CPU, and both paths draw
 * alike. */
static void test_vertex_paths(void)
{
    Nv2aVkStats was, st;
    Ins s = { 0 };

    diffuse(0xFF336699u);
    nv2a_vk_stats(&was);
    program_begin();
    pass_attribute(0, 0, 0);
    s.mac = 1;
    s.input = 3;
    s.a.mux = 2;
    s.a.swz = XYZW;
    s.oaddr = 20;
    s.omask = 15;
    s.to_c = 1;
    put_ins(&s);
    pass_attribute(3, 3, 1);
    method(0x1EA0, 0);
    clear(0);
    quad(0, 0, 8, 8, 0, 1, 1);
    flip();
    CHECK(pixel(3, 3) == 0xFF336699u);
    nv2a_vk_stats(&st);
    CHECK(st.programs == was.programs);

    program_begin();
    pass_attribute(0, 0, 0);
    pass_attribute(3, 3, 1);
    method(0x1EA0, 0);
    nv2a_vk_cpu_vertices(1);
    clear(0);
    rect(1.3f, 2.7f, 20.6f, 9.4f, 0xFF2468ACu);
    flip();
    CHECK(pixel(5, 5) == 0xFF2468ACu);
    memcpy(s_mem + 0x90000u, s_mem + COLOR_AT, W * H * 4);
    nv2a_vk_cpu_vertices(0);
    nv2a_vk_stats(&st);
    CHECK(st.programs == was.programs);
    clear(0);
    rect(1.3f, 2.7f, 20.6f, 9.4f, 0xFF2468ACu);
    flip();
    CHECK(memcmp(s_mem + 0x90000u, s_mem + COLOR_AT, W * H * 4) == 0);
    nv2a_vk_stats(&st);
    CHECK(st.programs == was.programs + 1);
}

/* ---- textures the GPU drew ---------------------------------------------- */

#define A_AT 0xA0000u
#define B_AT 0xA8000u
#define C_AT 0xB8000u

static Nv2aVkStats s_mark;

static void mark(void) { nv2a_vk_stats(&s_mark); }

/* Since mark(): flips alone waited for the GPU. */
static int waited_for(unsigned flips)
{
    Nv2aVkStats st;

    if (s_touch)
        return 1;                       /* looking at the picture counts */
    nv2a_vk_stats(&st);
    return st.mid_frame == s_mark.mid_frame && st.flushes == s_mark.flushes + flips;
}

static unsigned copies(void)
{
    Nv2aVkStats st;

    nv2a_vk_stats(&st);
    return st.card_copies - s_mark.card_copies;
}

static unsigned rearranged(void)
{
    Nv2aVkStats st;

    nv2a_vk_stats(&st);
    return st.rearranged - s_mark.rearranged;
}

/* Draw flat colours into a surface; a clear first. */
static void paint(uint32_t at, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format,
                  uint32_t argb)
{
    surface(at, w, h, pitch, format);
    method(0x1E70, 0);
    pass(4);
    method(0x1D90, argb);
    method(0x1D94, 0xF0);
}

/* Back to the window's surface, texture 0 alone. */
static void show(void)
{
    main_surface();
    pass(8);
    method(0x1E70, 1);
}

static void bind_linear(uint32_t at, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    bind_texture(at, color, 0, 0);
    method(0x1B10, pitch << 16);
    method(0x1B1C, w << 16 | h);
}

static uint16_t half(uint32_t at)
{
    uint16_t v;
    memcpy(&v, s_mem + at, 2);
    return v;
}

/* Each surface format: what is stored, and what a texture of it reads. */
static void test_drawn_formats(void)
{
    static const struct { uint32_t surface, bytes, texture, stored, want; } k[] = {
        { 0x8, 4, 0x12, 0x80FF8040u, 0x80FF8040u },      /* A8R8G8B8 */
        { 0x8, 4, 0x1E, 0x80FF8040u, 0xFFFF8040u },      /* as X8R8G8B8 */
        { 0x8, 4, 0x3F, 0x80FF8040u, 0x804080FFu },      /* as A8B8G8R8 */
        { 0x8, 4, 0x40, 0x80FF8040u, 0x4080FF80u },      /* as B8G8R8A8 */
        { 0x8, 4, 0x41, 0x80FF8040u, 0x4080FF80u },      /* as R8G8B8A8 */
        { 0x3, 2, 0x11, 0xFC08u,     0xFFFF8142u },      /* R5G6B5 */
        { 0x3, 2, 0x10, 0xFC08u,     0xFFFF0042u },      /* as A1R5G5B5 */
        { 0x3, 2, 0x1D, 0xFC08u,     0xFFCC0088u },      /* as A4R4G4B4 */
        { 0x2, 2, 0x10, 0xFE08u,     0xFFFF8442u },      /* X1R5G5B5, ones */
        { 0x1, 2, 0x10, 0x7E08u,     0x00FF8442u },      /* and zeroes */
        { 0x1, 2, 0x1C, 0x7E08u,     0xFFFF8442u },      /* as X1R5G5B5 */
        { 0x9, 1, 0x13, 0x40u,       0xFF404040u },      /* B8 as Y8 */
        { 0x9, 1, 0x1F, 0x40u,       0x40FFFFFFu },      /* as A8 */
        { 0x9, 1, 0x1B, 0x40u,       0x40404040u },      /* as AY8 */
        { 0xA, 2, 0x20, 0x8040u,     0x80404040u },      /* G8B8 as A8Y8 */
        { 0xA, 2, 0x17, 0x8040u,     0x80408040u },      /* as G8B8 */
    };
    unsigned i;

    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        uint32_t at = A_AT + i * 0x1000u, v = 0;

        mark();
        paint(at, 16, 16, 16 * k[i].bytes, 0x0120u | k[i].surface, 0x00000000u);
        rect(0, 0, 8, 16, 0x80FF8040u);
        show();
        bind_linear(at, k[i].texture, 16, 16, 16 * k[i].bytes);
        quad(0, 0, 16, 16, 0, 16, 16);
        flip();
        memcpy(&v, s_mem + at + 3 * 16 * k[i].bytes + 2 * k[i].bytes, k[i].bytes);
        CHECK(v == k[i].stored);
        CHECK(close_to(pixel(3, 3), k[i].want, 1));
        CHECK(waited_for(1) && copies() == 0);
        if (s_fail) {
            fprintf(stderr, "format %u: stored %X, read %08X, wanted %08X\n", i, v,
                    pixel(3, 3), k[i].want);
            return;
        }
    }
}

/* Writing to part of a pixel, where the pixel is not 8:8:8:8. */
static void test_drawn_channels(void)
{
    paint(A_AT, 16, 16, 16, 0x0129, 0x00000011u);        /* B8 */
    method(0x0358, 0x00010000u);                         /* red: not there */
    rect(0, 0, 4, 4, 0xFFFFFFFFu);
    method(0x0358, 0x00000001u);
    rect(4, 0, 8, 4, 0xFF000099u);
    method(0x0358, 0x01010101u);
    method(0x0304, 1);
    method(0x0344, 0x8001);                              /* constant colour */
    method(0x0348, 0);
    method(0x034C, 0xFF000080u);
    rect(8, 0, 12, 4, 0xFFFFFFFFu);
    method(0x0304, 0);

    paint(B_AT, 16, 16, 32, 0x0121, 0x00000000u);        /* X1R5G5B5 */
    method(0x0304, 1);
    method(0x0344, 0x304);                               /* dst alpha: 1 */
    method(0x0348, 0);
    rect(0, 0, 4, 4, 0xFFFFFFFFu);
    method(0x0344, 0x305);                               /* 1 - dst alpha: 0 */
    rect(4, 0, 8, 4, 0xFFFFFFFFu);
    method(0x0344, 1);
    method(0x0304, 0);

    surface(0xB0000u, 16, 16, 32, 0x0123);               /* R5G6B5 */
    method(0x1D90, 0xFF000000u);
    method(0x1D94, 0xF0);
    method(0x1D90, 0x00FFFFFFu);
    method(0x1D94, 0x20);                                /* green alone */
    main_surface();
    flip();
    CHECK(s_mem[A_AT] == 0x11 && s_mem[A_AT + 4] == 0x99 && s_mem[A_AT + 8] == 0x80);
    CHECK(half(B_AT) == 0x7FFF && half(B_AT + 8) == 0);
    CHECK(half(0xB0000u) == 0x07E0);
}

/* Blending rounds to the surface's own pixels each time. */
static void test_drawn_precision(void)
{
    unsigned i;

    paint(A_AT, 16, 16, 32, 0x0123, 0xFF000000u);
    method(0x0304, 1);
    method(0x0344, 1);
    method(0x0348, 1);
    for (i = 0; i < 4; i++)
        rect(0, 0, 16, 16, 0xFF020202u);                 /* under half a step */
    method(0x0304, 0);
    show();
    bind_linear(A_AT, 0x11, 16, 16, 32);
    quad(0, 0, 16, 16, 0, 16, 16);
    flip();
    /* 2/255 is 0.24 of a 31st and 0.49 of a 63rd: nothing ever adds up. */
    CHECK(pixel(3, 3) == 0xFF000000u);
}

/* A rectangle inside a surface, and a surface sampled into itself. */
static void test_drawn_rectangle(void)
{
    uint32_t x, y, wrong = 0;

    mark();
    paint(A_AT, 64, 32, 256, 0x0128, 0xFF000000u);
    rect(16, 8, 20, 12, 0xFFFF0000u);
    rect(20, 8, 24, 12, 0xFF00FF00u);
    show();
    clear(0xFF0000FFu);
    bind_linear(A_AT + 8 * 256 + 16 * 4, 0x12, 8, 4, 256);
    quad(0, 0, 8, 4, 0, 8, 4);
    quad(0, 8, 8, 12, 0, 8, 4);                          /* copied once */
    CHECK(copies() == 1);
    paint(A_AT, 64, 32, 256, 0x0128, 0xFF000000u);
    rect(16, 8, 20, 12, 0xFF808080u);
    show();
    bind_linear(A_AT + 8 * 256 + 16 * 4, 0x12, 8, 4, 256);
    quad(0, 16, 8, 20, 0, 8, 4);                         /* drawn over: again */
    CHECK(copies() == 2);
    flip();
    for (y = 0; y < 4; y++)
        for (x = 0; x < 8; x++) {
            wrong += pixel(x, y) != (x < 4 ? 0xFFFF0000u : 0xFF00FF00u);
            wrong += pixel(x, y + 8) != pixel(x, y);
            wrong += pixel(x, y + 16) != (x < 4 ? 0xFF808080u : 0xFF000000u);
        }
    CHECK(wrong == 0);
    CHECK(waited_for(1));

    mark();
    method(0x1E70, 0);
    pass(4);
    clear(0xFF000000u);
    rect(0, 0, 8, 8, 0xFF123456u);
    show();
    bind_linear(COLOR_AT, 0x12, W, H, W * 4);
    quad(20, 0, 28, 8, 0, 8, 8);
    method(0x1E70, 0);
    pass(4);
    rect(0, 0, 8, 8, 0xFF654321u);
    show();
    quad(30, 0, 38, 8, 0, 8, 8);
    flip();
    CHECK(pixel(21, 1) == 0xFF123456u && pixel(31, 1) == 0xFF654321u);
    CHECK(pixel(1, 1) == 0xFF654321u && copies() == 2 && waited_for(1));
}

/* Cube faces and mip levels, each a surface of its own. */
static void test_drawn_parts(void)
{
    static const uint32_t colors[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
                                        0xFFFFFF00u, 0xFF00FFFFu, 0xFFFF00FFu };
    static const float to[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    uint32_t f, i, mx, my;

    mark();
    for (f = 0; f < 6; f++)
        paint(A_AT + f * 1024, 16, 16, 64, 0x0228u | 4u << 16 | 4u << 24, colors[f]);
    for (f = 0; f < 6; f++) {                    /* not left as they were cleared */
        surface(A_AT + f * 1024, 16, 16, 64, 0x0228u | 4u << 16 | 4u << 24);
        rect(0, 0, 16, 16, 0xFF123456u);
        rect(6, 0, 10, 16, colors[(f + 1) % 6]);     /* the middle is looked at */
    }
    main_surface();
    clear(0xFF000000u);
    pass(8);
    bind_texture(A_AT, 0x06, 4, 4);
    method(0x1B04, 0x2Cu | 0x06u << 8 | 1u << 16 | 4u << 20 | 4u << 24);
    method(0x1E70, 3);
    for (f = 0; f < 4; f++) {
        float x = (float)(f * 10);

        quad(x, 0, x + 8, 8, 0, 0, 0);           /* positions; then one direction */
        for (i = 0; i < 4; i++) {
            put_float(UV_AT + 8 * i, to[f][0]);
            put_float(UV_AT + 8 * i + 4, to[f][1]);
        }
        method(0x17FC, 8);
        method(0x1810, 3u << 24);
        method(0x17FC, 0);
    }
    flip();
    for (f = 0; f < 4; f++)
        CHECK(pixel(f * 10 + 3, 3) == colors[f + 1]);
    CHECK(copies() == 6 && waited_for(1));

    /* Level 0 drawn, level 1 the title's, level 2 drawn. */
    xbox_swizzle_masks(8, 8, &mx, &my);
    for (i = 0; i < 64; i++) {
        uint32_t v = 0xFF000000u | i;
        memcpy(s_mem + B_AT + 1024 + 4 * i, &v, 4);
    }
    mark();
    paint(B_AT, 16, 16, 64, 0x0228u | 4u << 16 | 4u << 24, 0);
    rect(0, 0, 16, 16, 0xFF112233u);
    paint(B_AT + 1024 + 256, 4, 4, 16, 0x0228u | 2u << 16 | 2u << 24, 0);
    rect(0, 0, 4, 4, 0xFF445566u);
    show();
    bind_texture(B_AT, 0x06, 4, 4);
    method(0x1B04, 0x28u | 0x06u << 8 | 3u << 16 | 4u << 20 | 4u << 24);
    method(0x1B14, 0x01032000u);                 /* nearest, nearest level */
    for (f = 0; f < 3; f++) {
        method(0x1B0C, 0x40000000u | f * 256u << 18 | f * 256u << 6);
        quad((float)(f * 10), 10, (float)(f * 10 + 8), 18, 0, 1, 1);
    }
    flip();
    CHECK(pixel(3, 13) == 0xFF112233u && pixel(23, 13) == 0xFF445566u);
    CHECK(pixel(13, 15) == (0xFF000000u | swizzle_deposit(3, mx) | swizzle_deposit(5, my)));
    CHECK(copies() == 2 && waited_for(1));
    method(0x1B0C, 0x4003FFC0u);
}

/* A texture seen at a slant, four texels a pixel one way and one the other: the
 * third level, or with anisotropy the first. */
static uint32_t slanted(uint32_t min, uint32_t mag, uint32_t control0)
{
    method(0x1B0C, control0);
    method(0x1B14, mag << 24 | min << 16 | 0x2000u);
    clear(0xFF000000u);
    quad(0, 0, 4, 16, 0, 1, 1);
    flip();
    return pixel(1, 7);
}

static void test_anisotropy(void)
{
    static const uint32_t colors[3] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
    const uint32_t mips = 0x4003FFC0u, one = 0x40000000u;
    uint32_t level, i, at = A_AT, n;

    for (level = 0; level < 3; level++)
        for (i = 0; i < 256u >> 2 * level; i++, at += 4)
            memcpy(s_mem + at, &colors[level], 4);
    show();
    bind_texture(A_AT, 0x06, 4, 4);
    method(0x1B04, 0x28u | 0x06u << 8 | 3u << 16 | 4u << 20 | 4u << 24);
    CHECK(nv2a_vk_anisotropy_is() == 1);
    for (n = 1; n <= NV2A_VK_ANISOTROPY; n *= 2) {
        nv2a_vk_anisotropy((int)n);
        CHECK(nv2a_vk_anisotropy_is() == (int)n);
        CHECK(slanted(4, 2, mips) == colors[n < 2 ? 2 : n < 4 ? 1 : 0]);
        CHECK(slanted(6, 2, mips) == colors[n < 2 ? 2 : n < 4 ? 1 : 0]);
        CHECK(slanted(3, 2, mips) == colors[2]);        /* not the title's nearest */
        CHECK(slanted(5, 2, mips) == colors[2]);
        CHECK(slanted(4, 1, mips) == colors[2]);
        CHECK(slanted(2, 2, mips) == colors[0] && slanted(4, 2, one) == colors[0]);
        /* Where the title asks for more, that. */
        CHECK(slanted(4, 2, mips | 0x20u) == colors[0]);
    }
    nv2a_vk_anisotropy(0);
    nv2a_vk_anisotropy(3);
    nv2a_vk_anisotropy(32);
    CHECK(nv2a_vk_anisotropy_is() == 16);
    /* Not where the title leaves it the first level alone: columns. */
    for (i = 0; i < 256; i++) {
        uint32_t mx, my, v;

        xbox_swizzle_masks(16, 16, &mx, &my);
        v = (i & 3) == 1 || (i & 3) == 2 ? 0xFFFFFFFFu : 0xFF000000u;
        memcpy(s_mem + A_AT + 4 * (swizzle_deposit(i & 15, mx) | swizzle_deposit(i >> 4, my)),
               &v, 4);
    }
    CHECK(slanted(4, 2, one) == 0xFFFFFFFFu && slanted(4, 2, mips) != 0xFFFFFFFFu);
    nv2a_vk_anisotropy(1);
    CHECK(slanted(4, 2, mips) == colors[2]);
    method(0x1B0C, mips);
    method(0x1E70, 0);
    pass(4);
}

static uint32_t rainbow(uint32_t x, uint32_t y) { return 0xFF000000u | x << 16 | y << 8 | 0x55u; }

/* A surface of 32x16 pixels, each its own colour. */
static void paint_rainbow(uint32_t at, uint32_t pitch, uint32_t format)
{
    uint32_t x, y;

    paint(at, 32, 16, pitch, format, 0);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 32; x++)
            rect((float)x, (float)y, (float)x + 1, (float)y + 1, rainbow(x, y));
}

/* Drawn in one layout, used in the other. */
static void test_drawn_layouts(void)
{
    const uint32_t swizzled = 0x0228u | 5u << 16 | 4u << 24;
    uint32_t x, y, mx, my, wrong = 0;

    xbox_swizzle_masks(32, 16, &mx, &my);

    /* Rows sampled as swizzled: texel n is pixel n. */
    mark();
    paint_rainbow(A_AT, 128, 0x0128);
    show();
    clear(0);
    bind_texture(A_AT, 0x06, 5, 4);
    quad(0, 0, 32, 16, 0, 1, 1);
    quad(32, 0, 64, 16, 0, 1, 1);
    CHECK(rearranged() == 1);
    flip();
    CHECK(waited_for(1));
    for (y = 0; y < 16; y++)
        for (x = 0; x < 32; x++) {
            uint32_t n = swizzle_deposit(x, mx) | swizzle_deposit(y, my);
            wrong += pixel(x, y) != rainbow(n % 32, n / 32) || pixel(x + 32, y) != pixel(x, y);
        }
    CHECK(wrong == 0);

    /* And the other way round. */
    mark();
    paint_rainbow(B_AT, 128, swizzled);
    show();
    clear(0);
    bind_linear(B_AT, 0x12, 32, 16, 128);
    quad(0, 0, 32, 16, 0, 32, 16);
    flip();
    CHECK(rearranged() == 1 && waited_for(1));
    for (wrong = y = 0; y < 16; y++)
        for (x = 0; x < 32; x++) {
            uint32_t n = swizzle_deposit(x, mx) | swizzle_deposit(y, my);
            wrong += pixel(n % 32, n / 32) != rainbow(x, y);
        }
    CHECK(wrong == 0);

    /* Drawn into as both: swizzled, the alpha set as rows, swizzled again. A
     * clear of all of it would leave guest memory right as well. */
    mark();
    paint_rainbow(C_AT, 128, swizzled);
    surface(C_AT, 32, 16, 128, 0x0128);
    method(0x1D90, 0x20000000u);
    method(0x1D94, 0x80);
    surface(C_AT, 32, 16, 128, swizzled);
    rect(0, 0, 4, 4, 0x40ABCDEFu);
    main_surface();
    flip();
    CHECK(rearranged() == 2);
    CHECK(waited_for(1));
    for (wrong = y = 0; y < 16; y++)
        for (x = 0; x < 32; x++) {
            uint32_t n = swizzle_deposit(x, mx) | swizzle_deposit(y, my);
            wrong += word(C_AT + 4 * n) != (x < 4 && y < 4 ? 0x40ABCDEFu
                                            : (rainbow(x, y) & 0xFFFFFFu) | 0x20000000u);
        }
    CHECK(wrong == 0);

    /* The same in the next frame, from what the flip left: nothing uploaded. */
    {
        Nv2aVkStats st;

        mark();
        surface(C_AT, 32, 16, 128, 0x0128);
        method(0x1D90, 0x60000000u);
        method(0x1D94, 0x80);
        surface(C_AT, 32, 16, 128, swizzled);
        rect(0, 0, 4, 4, 0x80FEDCBAu);
        main_surface();
        flip();
        nv2a_vk_stats(&st);
        CHECK(st.surface_uploads == s_mark.surface_uploads && waited_for(1));
        CHECK(word(C_AT) == 0x80FEDCBAu && word(C_AT + 4 * 511) == 0x601F0F55u);
    }
}

/* What nothing on the card can do goes through memory, and is right. */
static void test_drawn_fallback(void)
{
    Nv2aVkStats st;

    mark();
    paint(A_AT, 16, 16, 32, 0x0123, 0xFF000000u);
    rect(0, 0, 8, 16, 0xFFFF0000u);              /* 0xF800: R6G5B5's 62, 0, 0 */
    show();
    clear(0);
    bind_linear(A_AT, 0x37, 16, 16, 32);
    quad(0, 0, 16, 16, 0, 16, 16);
    flip();
    nv2a_vk_stats(&st);
    CHECK(st.mid_frame == s_mark.mid_frame + 1 || s_touch);
    CHECK(close_to(pixel(3, 3), 0xFFFB0000u, 1) && pixel(11, 3) == 0xFF000000u);
}

/* Over a flip, and under what the title's CPU writes. */
static void test_drawn_kept(void)
{
    uint32_t v = 0xFF777777u;

    paint(A_AT, 16, 16, 64, 0x0128, 0xFF000000u);
    rect(0, 0, 8, 16, 0xFF00FF00u);
    main_surface();
    flip();
    mark();
    show();
    clear(0xFF000000u);
    bind_linear(A_AT, 0x12, 16, 16, 64);
    quad(0, 0, 16, 16, 0, 16, 16);               /* the last frame's */
    CHECK(copies() == 0);
    surface(A_AT, 16, 16, 64, 0x0128);
    method(0x1E70, 0);
    pass(4);
    rect(8, 0, 12, 16, 0xFF0000FFu);             /* drawn, then written to */
    memcpy(s_mem + A_AT + 4 * 14, &v, 4);
    memcpy(s_mem + A_AT + 64 + 4 * 14, &v, 4);
    memcpy(s_mem + A_AT + 64 * 3, &v, 4);
    show();
    surface(COLOR_AT, W, H, W * 4, 0x0128);
    bind_linear(A_AT, 0x12, 16, 16, 64);
    quad(20, 0, 36, 16, 0, 16, 16);
    flip();
    CHECK(pixel(3, 3) == 0xFF00FF00u && pixel(9, 3) == 0xFF000000u);
    CHECK(pixel(29, 3) == 0xFF0000FFu && pixel(21, 2) == 0xFF00FF00u);
    CHECK(word(A_AT + 4 * 14) == v && word(A_AT + 64 + 4 * 14) == v
          && word(A_AT + 64 * 3) == v);
    CHECK(word(A_AT) == 0xFF00FF00u && word(A_AT + 4 * 9) == 0xFF0000FFu);
    CHECK(waited_for(1));
    show();
    quad(20, 0, 36, 16, 0, 16, 16);
    flip();
    CHECK(pixel(34, 0) == v && pixel(34, 1) == v && pixel(20, 3) == v);
    CHECK(pixel(29, 3) == 0xFF0000FFu);
}

/* The clip gets taller in the frame that draws into it. */
static void test_drawn_growing(void)
{
    mark();
    paint(0xB4000u, 16, 8, 64, 0x0128, 0xFF000000u);
    rect(0, 0, 16, 8, 0xFF112233u);
    surface(0xB4000u, 16, 16, 64, 0x0128);
    rect(0, 12, 16, 16, 0xFF445566u);
    main_surface();
    flip();
    CHECK(word(0xB4000u + 64 * 7) == 0xFF112233u && word(0xB4000u + 64 * 12) == 0xFF445566u);
    CHECK(waited_for(1));
}

/* A picture at random: its memory, layout, size and pixels. */
typedef struct { uint32_t at, w, h, pitch, swizzled, kind, bytes; } Shape;

static Shape random_shape(void)
{
    static const uint32_t bytes[3] = { 4, 1, 2 };
    Shape s;

    s.kind = rnd() % 4 ? 0 : 1 + rnd() % 2;
    s.bytes = bytes[s.kind];
    s.swizzled = rnd() % 2;
    s.w = 16u << rnd() % 2;
    s.h = 8u << rnd() % 3;
    s.pitch = s.swizzled || rnd() % 2 ? s.w * s.bytes : 128;
    if (!s.swizzled && s.pitch < 64)
        s.pitch = 64;
    s.at = A_AT + (rnd() % 4) * 0x400u;
    return s;
}

static void random_target(const Shape *s)
{
    static const uint32_t color[3] = { 0x8, 0x9, 0xA };

    surface(s->at, s->w, s->h, s->pitch,
            0x0020u | color[s->kind] | (s->swizzled ? 0x200u : 0x100u)
            | (s->w == 16 ? 4u : 5u) << 16 | (s->h == 8 ? 3u : s->h == 16 ? 4u : 5u) << 24);
}

static void random_texture(const Shape *s)
{
    static const uint32_t rows[3] = { 0x12, 0x13, 0x17 }, swizzled[3] = { 0x06, 0x00, 0x28 };

    if (s->swizzled)
        bind_texture(s->at, swizzled[s->kind], s->w == 16 ? 4 : 5,
                     s->h == 8 ? 3 : s->h == 16 ? 4 : 5);
    else
        bind_linear(s->at, rows[s->kind], s->w, s->h, s->pitch);
}

/* Nonzero: it was a flip. */
static int random_operation(int writes)
{
    Shape a = random_shape(), b = random_shape();
    uint32_t x = rnd() % 12, y = rnd() % 6, c = rnd(), what = rnd() % 16;

    random_target(&a);
    method(0x1E70, 0);
    pass(4);
    if (what < 6) {
        rect((float)x, (float)y, (float)x + 5, (float)y + 4, c);
    } else if (what < 8) {
        method(0x1D98, (a.w - 1) << 16);         /* not cut to the clip */
        method(0x1D9C, (a.h - 1) << 16);
        method(0x1D90, c);
        method(0x1D94, what == 6 ? 0xF0u : (rnd() & 0xF0u));
    } else if (what < 14) {
        pass(8);
        random_texture(&b);
        quad((float)x, (float)y, (float)(x + b.w), (float)(y + b.h), 0,
             b.swizzled ? 1.0f : (float)b.w, b.swizzled ? 1.0f : (float)b.h);
    } else {
        main_surface();
        flip();
        return writes && what == 15 ? 2 : 1;
    }
    return 0;
}

static uint64_t random_frames(void)
{
    uint64_t sum = 1469598103934665603ull;
    unsigned op, i;

    memset(s_mem + A_AT, 0x5A, 0x10000);
    for (op = 0; op < 1500; op++) {
        int flipped = random_operation(1);

        if (flipped)
            for (i = 0; i < 0x10000; i++)
                sum = (sum ^ s_mem[A_AT + i]) * 1099511628211ull;
        if (flipped == 2)                        /* and the title writes some */
            for (i = 0; i < 8; i++)
                s_mem[A_AT + rnd() % 0x2000] = (uint8_t)rnd();
    }
    main_surface();
    method(0x1D98, (W - 1u) << 16);
    method(0x1D9C, (H - 1u) << 16);
    flip();
    return sum;
}

/* Anything drawn and sampled as anything, on the card and through memory. */
static void test_drawn_random(void)
{
    unsigned round;

    for (round = 1; round <= 8; round++) {
        uint64_t card, memory;
        Nv2aVkStats st;

        mark();
        s_seed = round;
        card = random_frames();
        nv2a_vk_stats(&st);
        s_seed = round;
        nv2a_vk_textures_via_ram(1);
        memory = random_frames();
        nv2a_vk_textures_via_ram(0);
        CHECK(card == memory);
        CHECK(st.card_copies > s_mark.card_copies + 50
              && st.rearranged > s_mark.rearranged + 20);
    }
}

/* 16 by 16 pixels, one texel to each (`to` of its range) from `off` and
 * `down` on. */
static void compared(float r, float q, float to, float off, float down)
{
    static const float at[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    unsigned i;

    method(0x1760 + 4 * 9, PB_FMT_F(4u, 16u));
    for (i = 0; i < 4; i++) {
        corner(i, 16 * at[i][0], 16 * at[i][1]);
        put_float(UV_AT + 16 * i, (to * at[i][0] + off) * q);
        put_float(UV_AT + 16 * i + 4, (to * at[i][1] + down) * q);
        put_float(UV_AT + 16 * i + 8, r * q);
        put_float(UV_AT + 16 * i + 12, q);
    }
    drawn_as(8, 4);
    method(0x1760 + 4 * 9, PB_FMT_F(2u, 8u));
    flip();
}

static void depth_quad(float x0, float x1, float z)
{
    method(0x1E70, 0);
    method(0x030C, 1);
    method(0x035C, 1);
    quad(x0, 0, x1, 16, z, 1, 1);
    method(0x030C, 0);
    method(0x035C, 0);
    method(0x1E70, 2);
}

static void test_depth_textures(void)
{
    static const struct { uint32_t func; int under, same, over; } k[8] = {
        { 0, 0, 0, 0 }, { 1, 0, 0, 1 }, { 2, 0, 1, 0 }, { 3, 0, 1, 1 },
        { 4, 1, 0, 0 }, { 5, 1, 0, 1 }, { 6, 1, 1, 0 }, { 7, 1, 1, 1 } };
    static const struct { uint32_t color, bytes, v; float under, over; } m[6] = {
        { 0x2C, 2, 0x4000, 16000.0f, 17000.0f }, { 0x2D, 2, 0x8555, 2.66f, 2.67f },
        { 0x2A, 4, 0x40000012u, 4194300.0f, 4194308.0f },
        { 0x2B, 4, 0xE3000012u, 1.26e30f, 1.28e30f },
        { 0x30, 2, 0x4000, 16000.0f, 17000.0f },
        { 0x2E, 4, 0x40000012u, 4194300.0f, 4194308.0f } };
    unsigned i, x;

    clear(0);
    method(0x1D8C, 0x800000FFu);
    method(0x1D94, 0xF3);
    depth_quad(0, 8, 4000000.0f);
    bind_linear(ZETA_AT, 0x2E, W, H, W * 4);
    method(0x1E70, 2);
    pass(8);
    mark();
    for (i = 0; i < 8; i++) {
        method(0x1E6C, k[i].func);
        compared(3999000.0f, 1, 16, 0, 0);
        CHECK((pixel(3, 3) == 0xFFFFFFFFu) == k[i].under && (pixel(3, 3) == 0) == !k[i].under);
        compared(4000000.0f, 2, 16, 0, 0);          /* over q */
        CHECK((pixel(3, 3) == 0xFFFFFFFFu) == k[i].same);
        CHECK((pixel(12, 3) == 0xFFFFFFFFu) == k[i].under);
        compared(4001000.0f, 1, 16, 0, 0);
        CHECK((pixel(3, 3) == 0xFFFFFFFFu) == k[i].over);
    }
    CHECK(copies() == 1);
    method(0x1E6C, 1);
    compared(9000000.0f, 1, 16, 0, 0);
    CHECK(pixel(3, 3) == 0xFFFFFFFFu && pixel(12, 3) == 0xFFFFFFFFu);
    depth_quad(4, 8, 16000000.0f);               /* since drawn into */
    compared(5000000.0f, 1, 16, 0, 0);
    CHECK(pixel(2, 3) == 0xFFFFFFFFu && pixel(6, 3) == 0 && pixel(12, 3) == 0);
    method(0x1B14, 0x02062000u);                 /* four texels */
    compared(5000000.0f, 1, 16, 0.25f, 0);
    CHECK(close_to(pixel(3, 3), 0xBFBFBFBFu, 1) && pixel(2, 3) == 0xFFFFFFFFu && pixel(4, 3) == 0);
    compared(9000000.0f, 1, 16, 0, 12.25f);
    CHECK(close_to(pixel(6, 3), 0x40404040u, 1) && pixel(6, 2) == 0 && pixel(6, 4) == 0xFFFFFFFFu);
    mark();                                      /* not in this order */
    bind_texture(ZETA_AT, 0x2A, 4, 4);
    method(0x1E70, 2);
    compared(5000000.0f, 1, 1, 0, 0);
    CHECK(copies() == 0);
    bind_linear(ZETA_AT, 0x2E, W, H, W * 4);
    method(0x1E70, 2);
    method(0x1D8C, 0xFFFFFF00u);                 /* and cleared */
    method(0x1D94, 0x01);
    compared(5000000.0f, 1, 16, 0, 0);
    CHECK(pixel(2, 3) == 0);
    method(0x1E70, 1);                           /* without a third coordinate: 0 */
    method(0x1E6C, 6);
    compared(5000000.0f, 1, 16, 0, 0);
    CHECK(pixel(2, 3) == 0xFFFFFFFFu);
    method(0x1E70, 2);

    for (i = 0; i < 6; i++) {                    /* out of memory */
        for (x = 0; x < 256; x++)
            memcpy(s_mem + CUBE_AT + x * m[i].bytes, &m[i].v, m[i].bytes);
        if (m[i].color < 0x2E)
            bind_texture(CUBE_AT, m[i].color, 4, 4);
        else
            bind_linear(CUBE_AT, m[i].color, 16, 16, 16 * m[i].bytes);
        method(0x1E70, 2);
        method(0x1E6C, 1);
        compared(m[i].under, 1, m[i].color < 0x2E ? 1.0f : 16.0f, 0, 0);
        CHECK(pixel(3, 3) == 0);
        compared(m[i].over, 1, m[i].color < 0x2E ? 1.0f : 16.0f, 0, 0);
        CHECK(pixel(3, 3) == 0xFFFFFFFFu);
        flip();
    }
    method(0x1E6C, 0);
    method(0x1E70, 0);
    pass(4);
}

static unsigned refused(void)
{
    Nv2aVkStats st;

    nv2a_vk_stats(&st);
    return st.refused - s_mark.refused;
}

/* 16 by 16 pixels of it as they are. */
static void texels(uint32_t at, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    show();
    if (pitch)
        bind_linear(at, color, w, h, pitch);
    else
        bind_texture(at, color, w, h);
    quad(0, 0, 16, 16, 0, pitch ? 16.0f : 1.0f, pitch ? 16.0f : 1.0f);
    flip();
}

/* Depths alone: no colour surface. */
static void no_colours(uint32_t zeta, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format)
{
    surface(0, w, h, pitch, format);
    method(0x0214, zeta);
    method(0x1D8C, 0xFFFFFF00u);
    method(0x1D94, 0xF3);                        /* colours too: nowhere */
}

/* Depth buffers without colours, as colours, and colours as depths. */
static void test_depth_kinds(void)
{
    static const uint32_t zeta[3] = { 0x12345678u, 0xFFFFFFFFu, 0x000001C0u };
    unsigned i;

    main_surface();
    clear(0);
    for (i = 0; i < 3; i++) {
        method(0x1D8C, zeta[i]);
        method(0x1D94, 0x03);
        texels(ZETA_AT, 0x12, W, H, W * 4);
        CHECK(close_to(pixel(3, 3) & 0xFFFFFF00u, zeta[i] & 0xFFFFFF00u, 0));
        CHECK((pixel(3, 3) & 0xFF) == (zeta[i] & 0xFF));
    }
    method(0x1D8C, 0x99u);                       /* the stencils alone */
    method(0x1D94, 0x02);
    texels(ZETA_AT, 0x12, W, H, W * 4);
    CHECK(pixel(3, 3) == 0x00000199u);
    method(0x0290, 0x1000);
    method(0x1D8C, 0xE30000FFu);
    method(0x1D94, 0x03);
    texels(ZETA_AT, 0x12, W, H, W * 4);
    CHECK(pixel(3, 3) == 0xE30000FFu);
    method(0x0290, 0);

    mark();
    no_colours(A_AT, 32, 16, 128, 0x0128);
    count_on();
    depth_quad(0, 8, 4000000.0f);
    ask(0);
    CHECK(answer(0) == 128 && refused() == 0);
    main_surface();
    bind_linear(A_AT, 0x2E, 32, 16, 128);
    method(0x1E70, 2);
    method(0x1E6C, 1);
    pass(8);
    compared(5000000.0f, 1, 16, 0, 0);
    CHECK(pixel(3, 3) == 0xFFFFFFFFu && pixel(12, 3) == 0);
    texels(A_AT, 0x12, 32, 16, 128);
    CHECK(pixel(3, 3) == 0x3D090000u && pixel(12, 3) == 0xFFFFFF00u);
    texels(A_AT, 0x1E, 32, 16, 128);
    CHECK(pixel(3, 3) == 0xFF090000u && pixel(12, 3) == 0xFFFFFF00u);
    /* Into a surface and as depths again. */
    mark();
    paint(B_AT, 16, 16, 64, 0x0128, 0);
    pass(8);
    bind_linear(A_AT, 0x12, 32, 16, 128);
    quad(0, 0, 16, 16, 0, 16, 16);
    main_surface();
    bind_linear(B_AT, 0x2E, 16, 16, 64);
    method(0x1E70, 2);
    compared(3999990.0f, 1, 16, 0, 0);
    CHECK(pixel(3, 3) == 0 && pixel(12, 3) == 0);
    compared(4000010.0f, 1, 16, 0, 0);
    CHECK(pixel(3, 3) == 0xFFFFFFFFu && pixel(12, 3) == 0);
    /* Drawn into since: the surface is newer, then the depths are. */
    paint(A_AT, 32, 16, 128, 0x0128, 0xFF804020u);
    texels(A_AT, 0x12, 32, 16, 128);
    CHECK(pixel(3, 3) == 0xFF804020u);
    no_colours(A_AT, 32, 16, 128, 0x0128);
    texels(A_AT, 0x12, 32, 16, 128);
    CHECK(pixel(3, 3) == 0xFFFFFF00u);

    for (i = 0; i < 2; i++) {                    /* 24 bits, and floating-point */
        paint(B_AT, 16, 16, 64, 0x0128, 0);
        rect(4, 2, 16, 16, i ? 0xE3000012u : 0x40000012u);
        main_surface();
        bind_linear(B_AT + 2 * 64 + 4 * 4, 0x2E + i, 8, 8, 64);
        method(0x1E70, 2);
        method(0x1E6C, 1);
        pass(8);
        mark();
        compared(i ? 1.26e30f : 4194300.0f, 1, 16, 0, 0);
        CHECK(pixel(3, 3) == 0);
        compared(i ? 1.28e30f : 4194308.0f, 1, 16, 0, 0);
        CHECK(pixel(3, 3) == 0xFFFFFFFFu && pixel(7, 7) == 0xFFFFFFFFu);
        CHECK(copies() == 1);
        rect(0, 0, 1, 1, 0);
    }
    no_colours(C_AT, 16, 16, 64, 0x04040228u);   /* swizzled */
    depth_quad(0, 8, 4000000.0f);
    texels(C_AT, 0x06, 4, 4, 0);
    CHECK(pixel(3, 3) == 0x3D090000u && pixel(12, 3) == 0xFFFFFF00u);
    method(0x1E6C, 0);
    method(0x1E70, 0);
    pass(4);
    main_surface();
}

/* A quad with its left side's w and its right side's, of surface pixels. */
static void sloped(float x0, float x1, float y1, float w0, float w1)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        float w = i == 1 || i == 2 ? w1 : w0;

        corner(i, i == 1 || i == 2 ? x1 : x0, i >= 2 ? y1 : 0);
        put_float(POS_AT + 16 * i + 12, w);
    }
    drawn_as(8, 4);
}

static void w_quad(float x0, float x1, float w, uint32_t argb)
{
    diffuse(argb);
    s_quad_w = w;
    quad(x0, 0, x1, 8, w < 5000.0f ? 16000000.0f : 10.0f, 1, 1);    /* not as z */
    s_quad_w = 1.0f;
}

/* SET_CONTROL0's Z_PERSPECTIVE_ENABLE: depths are w. */
static void test_w_buffer(void)
{
    const uint32_t red = 0xFFFF0000u, green = 0xFF00FF00u, blue = 0xFF0000FFu, back = 0xFF000000u;
    unsigned i;

    for (i = 0; i < 2; i++) {                    /* the next frame's too */
        main_surface();
        method(0x1E70, 0);
        pass(4);
        clear(back);
        method(0x0290, 0x10000);
        method(0x030C, 1);
        method(0x035C, 1);
        method(0x0354, 0x201);                   /* less */
        w_quad(0, 16, 5000.0f, red);
        w_quad(4, 8, 6000.0f, green);
        w_quad(8, 12, 4999.0f, blue);
        method(0x0354, 0x202);                   /* equal */
        w_quad(0, 2, 5000.0f, green);
        w_quad(2, 4, 5001.0f, green);
        method(0x0354, 0x206);                   /* greater or equal */
        w_quad(12, 14, 5000.0f, green);
        w_quad(14, 16, 4990.0f, green);
        method(0x0354, 0x201);
        method(0x0338, 1);                       /* an offset, in units */
        method_float(0x0388, -20.0f);
        w_quad(2, 4, 5010.0f, blue);
        method_float(0x0388, 0.0f);
        method(0x0338, 0);
        method_float(0x0394, 2000.0f);           /* the range */
        method_float(0x0398, 8000.0f);
        w_quad(20, 24, 1990.0f, red);
        w_quad(24, 28, 2010.0f, red);
        w_quad(28, 32, 7990.0f, red);
        w_quad(32, 36, 8010.0f, red);
        method(0x1D78, 0x10);                    /* held, not left out */
        w_quad(36, 40, 1990.0f, red);
        method(0x1D78, 0);
        method_float(0x0394, 0.0f);
        method_float(0x0398, 16777215.0f);
        method(0x0290, 0);                       /* others in the same buffer */
        diffuse(green);
        quad(6, 0, 7, 8, 4000.0f, 1, 1);
        quad(7, 0, 8, 8, 5500.0f, 1, 1);
        quad(12, 4, 13, 8, 0.0f, 1, 1);
        method(0x030C, 0);
        flip();
        CHECK(pixel(1, 1) == green && pixel(3, 1) == blue && pixel(5, 1) == red);
        CHECK(pixel(9, 1) == blue && pixel(11, 1) == blue && pixel(13, 1) == green);
        CHECK(pixel(15, 1) == red && pixel(17, 1) == back);
        CHECK(pixel(22, 1) == back && pixel(26, 1) == red && pixel(30, 1) == red
              && pixel(34, 1) == back && pixel(38, 1) == red);
        CHECK(pixel(6, 1) == green && pixel(7, 1) == red && pixel(12, 6) == green);

        /* What is there, as colours and compared. */
        clear(back);
        method(0x1D8C, 0x12345678u);
        method(0x1D94, 0x03);
        method(0x0290, 0x10000);
        method(0x030C, 1);
        method(0x0354, 0x207);
        w_quad(0, 4, 1000000.0f, red);
        w_quad(4, 8, 5000.0f, red);
        sloped(8, 16, 8, 1000.0f, 4000.0f);
        method(0x0290, 0);
        method(0x030C, 0);
        texels(ZETA_AT, 0x12, W, H, W * 4);
        CHECK((pixel(1, 1) & 0xFFFFFF00u) == 0x0F424000u);
        CHECK((pixel(5, 1) & 0xFFFFFF00u) == 0x00138800u);
        CHECK(pixel(1, 12) == 0x12345678u);
        {                                        /* halfway in 1 / w: 1600 */
            uint32_t a = pixel(11, 1) >> 8, b = pixel(12, 1) >> 8;

            CHECK(nv2a_vk_scale_is() > 1         /* the bytes averaged */
                  || (a > 1400 && a < 1600 && b > 1600 && b < 1800));
        }
        bind_linear(ZETA_AT, 0x2E, W, H, W * 4);
        method(0x1E70, 2);
        method(0x1E6C, 1);
        pass(8);
        compared(5010.0f, 1, 16, 0, 0);
        CHECK(pixel(1, 1) == 0 && pixel(5, 1) == 0xFFFFFFFFu && pixel(1, 12) == 0);
        compared(4990.0f, 1, 16, 0, 0);
        CHECK(pixel(5, 1) == 0);
        compared(1193100.0f, 1, 16, 0, 0);
        CHECK(pixel(1, 1) == 0xFFFFFFFFu && pixel(1, 12) == 0xFFFFFFFFu);
        method(0x1E6C, 0);
        method(0x1E70, 0);
        pass(4);

        clear(back);                             /* depths as before */
        method(0x030C, 1);
        method(0x0354, 0x201);
        diffuse(red);
        quad(0, 0, 8, 8, 4000.0f, 1, 1);
        diffuse(green);
        quad(0, 0, 4, 8, 4100.0f, 1, 1);
        quad(4, 0, 8, 8, 3900.0f, 1, 1);
        method(0x030C, 0);
        flip();
        CHECK(pixel(1, 1) == red && pixel(5, 1) == green);
    }
    /* Where other depths are: those are lost, what is drawn is there. */
    method(0x030C, 1);
    method(0x0290, 0x10000);
    w_quad(0, 8, 9000.0f, blue);
    method(0x0290, 0);
    method(0x030C, 0);
    method(0x035C, 0);
    method(0x0354, 0x207);
    flip();
    CHECK(pixel(1, 1) == blue && pixel(5, 1) == blue);
}

static void test_drawn(void)
{
    /* The read-back buffer grows to its size, which takes a flush of its own. */
    paint(A_AT, 64, 128, 256, 0x0128, 0);
    rect(0, 0, 64, 128, 0xFFFFFFFFu);
    main_surface();
    flip();

    test_drawn_formats();
    test_drawn_channels();
    test_drawn_precision();
    test_drawn_rectangle();
    test_drawn_parts();
    test_drawn_layouts();
    test_drawn_fallback();
    test_drawn_kept();
    test_drawn_growing();
    test_drawn_random();
    main_surface();
    method(0x1E70, 0);
    pass(4);
}

/* Guest memory as the console has it: the window at 0x80000000 and again at
 * 0xF0000000, and some low memory. */
static int memory_init(void)
{
#if defined(_WIN32)
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, WINDOW_BYTES,
                                  NULL);
    unsigned tries;
    void *low;

    for (tries = 0; m && tries < 10; tries++) {
        s_base = (uint8_t *)VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
        if (!s_base)
            return 0;
        VirtualFree(s_base, 0, MEM_RELEASE);
        low = VirtualAlloc(s_base, LOW_AT + LOW_BYTES, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE);
        s_mem = (uint8_t *)MapViewOfFileEx(m, FILE_MAP_ALL_ACCESS, 0, 0, 0,
                                           s_base + 0x80000000u);
        s_tiled = MapViewOfFileEx(m, FILE_MAP_ALL_ACCESS, 0, 0, 0, s_base + 0xF0000000u);
        s_spare = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);   /* anywhere else */
        if (low && s_mem && s_tiled && s_spare)
            return 1;
        if (low)
            VirtualFree(low, 0, MEM_RELEASE);
        if (s_mem)
            UnmapViewOfFile(s_mem);
        if (s_tiled)
            UnmapViewOfFile(s_tiled);
        if (s_spare)
            UnmapViewOfFile(s_spare);
    }
    return 0;
#else
    s_base = (uint8_t *)mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    s_mem = s_base + 0x80000000u;
    return s_base != (uint8_t *)MAP_FAILED;
#endif
}

/* Two to four pixels drawn for one of the title's (test_all at a scale says
 * the rest, less what is a pixel's own: lines, points, halves of one). */
static void test_scale(void)
{
    uint32_t n, x, wrong, white = 0xFFFFFFFFu;

    setup();
    for (n = 2; n <= NV2A_VK_SCALES; n++) {
        clear(0xFF000000u);
        rect(50, 20, 58, 28, 0xFF8040C0u);           /* what is there stays */
        nv2a_vk_scale((int)n);
        flip();
        CHECK(nv2a_vk_scale_is() == (int)n && pixel(50, 20) == 0xFF8040C0u);
        count_on();
        rect(0, 0, 8, 8, white);                     /* whole pixels */
        ask(0);
        CHECK(answer(0) == 64);
        rect(10.5f, 0.5f, 18.5f, 8.5f, white);       /* and as Direct3D has them */
        count_on();
        rect(20, 0, 20.5f, 8, white);                /* half, of which edges go */
        ask(1);                                      /* to the pixel after */
        rect(30.25f, 0, 38, 8, white);
        s_mem[COLOR_AT + 4 * (W * 30 + 60)] = 0x77;  /* the title's own */
        flip();
        for (wrong = x = 0; x < 18; x++)
            wrong += pixel(x, 3) != (x < 8 || x >= 10 ? white : 0xFF000000u)
                  || pixel(x, 7) != pixel(x, 3) || pixel(x, 0) != pixel(x, 3)
                  || pixel(x, 8) != 0xFF000000u;
        CHECK(wrong == 0 && pixel(18, 3) == 0xFF000000u);
        method(0x0200, 8u << 16 | 40u);              /* a clip that begins at 40 */
        rect(30, 24, 64, 26, 0xFF223344u);
        main_surface();
        flip();
        CHECK(pixel(39, 25) == 0xFF000000u && pixel(40, 25) == 0xFF223344u
              && pixel(47, 25) == 0xFF223344u && pixel(48, 25) == 0xFF000000u);
        CHECK(pixel(50, 20) == 0xFF8040C0u && pixel(57, 27) == 0xFF8040C0u
              && pixel(58, 28) == 0xFF000000u);
        CHECK(pixel(31, 3) == white && (n != 2 || pixel(30, 3) != 0xFF000000u));
        CHECK(n != 4 || answer(1) <= 32);
        CHECK(s_mem[COLOR_AT + 4 * (W * 30 + 60)] == 0x77);
        rect(0, 12, 8, 14, white);                   /* on the card, only there */
        flip();
        CHECK(s_mem[COLOR_AT + 4 * (W * 30 + 60)] == 0x77 && pixel(59, 30) == 0xFF000000u
              && pixel(61, 30) == 0xFF000000u && pixel(40, 30) == 0xFF000000u
              && pixel(30, 15) == 0xFF000000u);
        if (n == 4) {                                /* in depth, finer steps too */
            for (x = 0; x < 4; x++) {
                corner(x, x == 1 || x == 2 ? 38.0f : 30.15625f, x >= 2 ? 8.0f : 0.0f);
                put_float(POS_AT + 16 * x + 12, 2.0f);
            }
            count_on();
            drawn_as(8, 4);
            ask(0);
            CHECK(answer(0) == 62);
        }

        /* Laid out anew, with what is between the title's pixels. */
        surface(A_AT, 16, 16, 64, 0x0228u | 4u << 16 | 4u << 24);
        clear(0xFF000000u);
        rect(0, 0, 8.75f, 16, white);
        main_surface();
        pass(8);
        bind_linear(A_AT, 0x12, 16, 16, 64);
        quad(0, 0, 16, 16, 0, 16, 16);
        flip();
        CHECK(n != 2 || close_to(pixel(0, 4), 0xFF808080u, 1));
        CHECK(pixel(0, 0) == white && pixel(1, 4) == 0xFF000000u);
        method(0x1E70, 0);
        pass(4);

        /* More rows: the colours and depths of the others move over. */
        surface(B_AT, 16, 8, 64, 0x0128);
        method(0x0214, 0xE0000u + n * 0x1000);
        clear(0xFF000000u);
        method(0x030C, 1);
        method(0x035C, 1);
        method(0x0354, 0x201);
        diffuse(0xFFFF0000u);
        quad(10, 4, 16, 8, 1000.0f, 1, 1);
        surface(B_AT, 16, 16, 64, 0x0128);
        method(0x0214, 0xE0000u + n * 0x1000);
        diffuse(0xFF0000FFu);
        quad(8, 0, 16, 16, 2000.0f, 1, 1);
        method(0x030C, 0);
        method(0x035C, 0);
        method(0x0354, 0x207);
        flip();
        CHECK(word(B_AT + 64 * 6 + 4 * 14) == 0xFFFF0000u);
        CHECK(word(B_AT + 64 * 6 + 4 * 9) == 0xFF0000FFu && word(B_AT + 64 * 12 + 4 * 9) == 0xFF0000FFu);
        main_surface();

        /* A picture drawn and then sampled: each pixel from its place. */
        surface(SMALL_AT, 16, 16, 64, 0x0128);
        clear(0xFF00FF00u);
        rect(3, 0, 7, 16, 0xFF0000FFu);
        main_surface();
        pass(8);
        bind_texture(SMALL_AT, 0x12, 0, 0);
        method(0x1B10, 64u << 16);
        method(0x1B1C, 16u << 16 | 16u);
        quad(0, 0, 16, 16, 0, 16, 16);
        flip();
        for (wrong = x = 0; x < 16; x++)
            wrong += pixel(x, 5) != (x >= 3 && x < 7 ? 0xFF0000FFu : 0xFF00FF00u);
        CHECK(wrong == 0);
        method(0x1E70, 0);
        pass(4);
        if (n < 4)
            test_drawn();
        test_depth_kinds();
        test_w_buffer();
    }
    clear(0xFF000000u);
    rect(0, 0, 8, 8, 0xFF0000FFu);
    flip();
    nv2a_vk_scale(1);
    nv2a_vk_scale(-1);
    nv2a_vk_scale(NV2A_VK_SCALES + 1);
    flip();
    CHECK(nv2a_vk_scale_is() == 1 && pixel(5, 5) == 0xFF0000FFu);
    nv2a_vk_scale(NV2A_VK_SCALE_FIT);                /* no window: as it is */
    for (n = 0; n < 40; n++)
        flip();
    rect(0, 0, 0.75f, 8, white);
    flip();
    CHECK(nv2a_vk_scale_is() == NV2A_VK_SCALE_FIT && pixel(0, 3) == white);
    nv2a_vk_scale(1);
}

#define MANY_AT  0x100000u
#define MANY_Z   0x1C0000u
#define MANY_W   320u
#define MANY_H   200u

static void many_surface(void);

#define PLACES 40u

/* A fixed-function pass, then one by a program as D3D ends them (DP4s with the
 * composite matrix, RCC, MAD), drawn where the depths are equal: everywhere. */
static void test_equal_depths(void)
{
    static const float offset[3] = { 0.53125f, 0.53125f, 0.0f };
    uint32_t round, i, j, x, y, left = 0, lit = 0;

    many_surface();
    for (round = 0; round < PLACES; round++) {
        float m[4][4], v[4] = { 0 };
        Ins ins = { 0 };

        /* A view of a floor or a wall, as large as a level: turned about, and
         * scaled to the viewport. */
        {
            float a = small_float(), b = small_float() * 0.4f, d = 400.0f + 100.0f * small_float();
            float r[3][4] = {
                { cosf(a), 0, -sinf(a), 37.3f * small_float() },
                { sinf(a) * sinf(b), cosf(b), cosf(a) * sinf(b), 61.7f * small_float() },
                { sinf(a) * cosf(b), -sinf(b), cosf(a) * cosf(b), d }
            };

            for (j = 0; j < 4; j++) {
                float one = j == 3 ? 1.0f : 0.0f;

                m[3][j] = r[2][j];
                m[0][j] = 160.0f * 1.3f * r[0][j] + 160.0f * r[2][j];
                m[1][j] = -100.0f * 1.7f * r[1][j] + 100.0f * r[2][j];
                m[2][j] = 16777215.0f * (1.0001f * r[2][j] - 4.0f * one);
            }
        }
        for (i = 0; i < 16; i++)
            method_float(0x0680 + 4 * i, m[i / 4][i % 4]);
        for (i = 0; i < 4; i++)
            constant(i, m[i][0], m[i][1], m[i][2], m[i][3]);
        constant(59, offset[0], offset[1], offset[2], 0);
        for (i = 0; i < 4; i++) {
            v[0] = (i == 1 || i == 2 ? 900.0f : -900.0f) + small_float();
            v[1] = round & 1 ? -70.0f + small_float() : (i >= 2 ? -300.0f : 300.0f);
            v[2] = round & 1 ? (i >= 2 ? -900.0f : 900.0f) + small_float() : 10.0f * small_float();
            v[3] = 1.0f;
            for (j = 0; j < 4; j++)
                put_float(POS_AT + 16 * i + 4 * j, v[j]);
        }
        method(0x0290, round & 2 ? 0x10000 : 0);    /* by w, with another limit after */
        method_float(0x0394, round & 2 ? 4.0f : 0.0f);
        method(0x030C, 1);
        method(0x035C, 1);
        method(0x0354, 0x207);
        clear(0xFF000000u);
        diffuse(0xFFFF0000u);
        method(0x1E94, 4);                       /* fixed function */
        method(0x17FC, 8);
        method(0x1810, 3u << 24);
        method(0x17FC, 0);

        program_begin();
        for (i = 0; i < 4; i++) {
            memset(&ins, 0, sizeof ins);
            ins.mac = 7;                         /* DP4 R12.c, v0, c[i] */
            ins.constant = i;
            ins.a.mux = 2;
            ins.b.mux = 3;
            ins.a.swz = ins.b.swz = XYZW;
            ins.omask = 8u >> i;
            put_ins(&ins);
        }
        memset(&ins, 0, sizeof ins);
        ins.ilu = 3;                             /* RCC R1.x, R12.w */
        ins.c.mux = 1;
        ins.c.reg = 12;
        ins.c.swz = 0xFF;
        ins.tdst = 1;
        ins.imask = 8;
        put_ins(&ins);
        memset(&ins, 0, sizeof ins);
        ins.mac = 4;                             /* MAD R12.xyz, R12, R1.x, c59 */
        ins.constant = 59;
        ins.a.mux = ins.b.mux = 1;
        ins.a.reg = 12;
        ins.b.reg = 1;
        ins.c.mux = 3;
        ins.a.swz = ins.c.swz = XYZW;
        ins.b.swz = XXXX;
        ins.omask = 14;
        put_ins(&ins);
        pass_attribute(3, 3, 1);
        method(0x1EA0, 0);
        method(0x1E94, 2);
        method(0x035C, 0);
        method(0x0354, 0x202);
        method_float(0x0394, round & 2 ? 3.99f : 0.0f);
        diffuse(0xFF0000FFu);
        method(0x17FC, 8);
        method(0x1810, 3u << 24);
        method(0x17FC, 0);
        method(0x030C, 0);
        method(0x0354, 0x207);
        flip();
        for (y = 0; y < MANY_H; y++)
            for (x = 0; x < MANY_W; x++) {
                left += (word(MANY_AT + (y * MANY_W + x) * 4) & 0xFF0000u) != 0;
                lit += (word(MANY_AT + (y * MANY_W + x) * 4) & 0xFFu) != 0;
            }
    }
    printf("equal depths: %u lit, %u not\n", lit, left);
    CHECK(left == 0 && lit > PLACES * 3000u);
    method(0x0290, 0);
    setup();
}

/* Several samples of a pixel: in a surface with a depth buffer as large as a
 * picture, where depths decide. */
static uint32_t many_pixel(uint32_t x, uint32_t y) { return word(MANY_AT + (y * MANY_W + x) * 4); }

static void many_surface(void)
{
    surface(MANY_AT, MANY_W, MANY_H, MANY_W * 4, 0x0128);
    method(0x0214, MANY_Z);
    method(0x1D98, 1023u << 16);                 /* the clears' rectangle */
    method(0x1D9C, 1023u << 16);
}

static void depths(int on)
{
    method(0x030C, (uint32_t)on);
    method(0x035C, (uint32_t)on);
    method(0x0354, on ? 0x201 : 0x207);
}

static int between(uint32_t p, uint32_t least, uint32_t most)
{
    uint32_t v = p & 0xFF;

    return v >= least && v <= most && ((p >> 8) & 0xFF) == v && ((p >> 16) & 0xFF) == v;
}

static Nv2aVkStats many_since(void)
{
    Nv2aVkStats st;

    nv2a_vk_stats(&st);
    st.resolved -= s_mark.resolved;
    st.spread -= s_mark.spread;
    return st;
}

static void many_at(uint32_t n)
{
    uint32_t x, wrong, white = 0xFFFFFFFFu, cpu = 0xFF112233u, i, v;
    Nv2aVkStats st;

    flip();                                      /* as few as were asked for */
    many_surface();
    depths(1);
    clear(0xFF000000u);
    quad(10, 4, 20.6875f, 12, 8000000.0f, 1, 1);
    diffuse(white);
    quad(10, 4, 20.6875f, 12, 4000000.0f, 1, 1);
    quad(30.5f, 4.5f, 38.5f, 12.5f, 4000000.0f, 1, 1);      /* through the centres */
    diffuse(0xFF0000FFu);
    quad(0, 0, 100, 3, 9000000.0f, 1, 1);
    quad(12, 5, 14, 6, 9000000.0f, 1, 1);        /* behind */
    depths(0);
    flip();
    CHECK(many_pixel(9, 8) == 0xFF000000u && many_pixel(10, 8) == white
          && many_pixel(19, 8) == white && many_pixel(21, 8) == 0xFF000000u);
    CHECK(n == 1 ? many_pixel(20, 8) == white : between(many_pixel(20, 8), 0x60, 0xD0));
    CHECK(many_pixel(12, 5) == white && many_pixel(99, 2) == 0xFF0000FFu);
    for (wrong = 0, x = 28; x < 42; x++)
        wrong += many_pixel(x, 6) != (x >= 30 && x < 38 ? white : 0xFF000000u)
              || many_pixel(x, 4) != many_pixel(x, 6) || many_pixel(x, 12) != 0xFF000000u;
    CHECK(wrong == 0);
    v = many_pixel(20, 8);

    /* A picture over it keeps its pixels, and so does what is there; what the
     * title wrote is in both. */
    many_surface();
    mark();
    rect(50, 4, 60.6875f, 12, white);
    memcpy(s_mem + MANY_AT + (100 * MANY_W + 7) * 4, &cpu, 4);
    flip();
    CHECK(many_pixel(60, 8) == white && many_pixel(20, 8) == v && many_pixel(12, 5) == white);
    many_surface();
    depths(1);
    diffuse(0xFFFF0000u);
    quad(15, 5, 17, 6, 4000000.0f, 1, 1);        /* not in front */
    quad(70, 4, 80, 12, 4000000.0f, 1, 1);
    depths(0);
    flip();
    CHECK(many_pixel(16, 5) == white && many_pixel(75, 8) == 0xFFFF0000u);
    CHECK(many_pixel(20, 8) == v && many_pixel(60, 8) == white
          && many_pixel(7, 100) == 0xFF112233u && many_pixel(8, 100) == 0xFF000000u);

    /* The title's own, while the samples have the newest. */
    memcpy(s_mem + MANY_AT + (8 * MANY_W + 75) * 4, &cpu, 4);
    many_surface();
    depths(1);
    quad(90, 4, 100, 12, 4000000.0f, 1, 1);
    depths(0);
    flip();
    CHECK(many_pixel(75, 8) == cpu && many_pixel(74, 8) == 0xFFFF0000u
          && many_pixel(95, 8) == 0xFFFF0000u);

    /* Written without being looked at. */
    many_surface();
    method(0x030C, 1);
    method(0x035C, 1);
    diffuse(0xFF00FF00u);
    quad(110, 4, 120, 12, 3000000.0f, 1, 1);
    depths(1);
    diffuse(0xFF0000FFu);
    quad(110, 4, 124, 12, 4000000.0f, 1, 1);
    depths(0);
    flip();
    CHECK(many_pixel(115, 8) == 0xFF00FF00u && many_pixel(122, 8) == 0xFF0000FFu);

    /* What it draws from the surface meanwhile is what is there then. */
    many_surface();
    depths(1);
    clear(0xFF000000u);
    diffuse(0xFF00FF00u);
    quad(0, 0, 8, 8, 4000000.0f, 1, 1);
    main_surface();
    depths(0);
    pass(8);
    bind_linear(MANY_AT, 0x12, MANY_W, MANY_H, MANY_W * 4);
    mark();
    quad(0, 0, 16, 16, 0, 16, 16);
    quad(16, 0, 32, 16, 0, 16, 16);
    st = many_since();
    CHECK(st.resolved == (n > 1) && st.spread == 0);
    many_surface();
    method(0x1E70, 0);
    pass(4);
    depths(1);
    diffuse(0xFFFF0000u);
    quad(4, 0, 12, 8, 5000000.0f, 1, 1);
    st = many_since();
    CHECK(st.resolved == (n > 1) && st.spread == 0);
    main_surface();
    depths(0);
    pass(8);
    bind_linear(MANY_AT, 0x12, MANY_W, MANY_H, MANY_W * 4);
    quad(0, 16, 16, 32, 0, 16, 16);
    method(0x1E70, 0);
    pass(4);
    flip();
    CHECK(pixel(6, 6) == 0xFF00FF00u && pixel(10, 6) == 0xFF000000u
          && pixel(22, 6) == 0xFF00FF00u);
    CHECK(pixel(6, 22) == 0xFF00FF00u && pixel(10, 22) == 0xFFFF0000u);

    /* Visible pixels, of both kinds in one count. */
    many_surface();
    depths(1);
    clear(0xFF000000u);
    count_on();
    quad(0, 0, 8, 8, 4000000.0f, 1, 1);
    quad(4, 0, 12, 8, 5000000.0f, 1, 1);
    depths(0);
    rect(20, 0, 24, 4, white);
    depths(1);
    quad(30, 0, 32, 4, 5000000.0f, 1, 1);
    ask(0);
    depths(0);
    CHECK(answer(0) == 64 + 32 + 16 + 8);

    /* Stencil alone is in depth too; so is a clear of the depths. */
    clear(0xFF000000u);
    method(0x032C, 1);
    method(0x0364, 0x207);
    method(0x0368, 0x55);
    method(0x036C, 0xFF);
    method(0x0360, 0xFF);
    method(0x0370, 0x1E00);
    method(0x0374, 0x1E00);
    method(0x0378, 0x1E01);
    rect(0, 0, 8, 8, 0xFFFF0000u);
    method(0x0364, 0x202);
    method(0x0378, 0x1E00);
    rect(4, 0, 12, 8, 0xFF0000FFu);
    method(0x032C, 0);
    flip();
    CHECK(many_pixel(2, 2) == 0xFFFF0000u && many_pixel(6, 2) == 0xFF0000FFu
          && many_pixel(10, 2) == 0xFF000000u);

    /* In turns: not for ever. */
    many_surface();
    clear(0xFF000000u);
    mark();
    for (i = 0; i < 20; i++) {
        depths(1);
        quad((float)i * 4, 0, (float)i * 4 + 2, 8, 4000000.0f, 1, 1);
        depths(0);
        rect((float)i * 4 + 2, 0, (float)i * 4 + 4, 8, 0xFF000000u | (i + 1));
    }
    st = many_since();
    CHECK(n == 1 ? st.spread + st.resolved == 0 : st.spread >= 4 && st.spread <= 9);
    flip();
    for (wrong = i = 0; i < 20; i++)
        wrong += many_pixel(i * 4 + 3, 4) != (0xFF000000u | (i + 1));
    CHECK(wrong == 0);
    many_surface();                              /* but again in the next frame */
    depths(1);
    quad(0, 20, 2, 28, 4000000.0f, 1, 1);
    depths(0);
    mark();
    rect(2, 20, 4.6875f, 28, white);             /* in whole pixels */
    flip();
    CHECK(n == 1 || many_since().resolved == 1);
    CHECK(many_pixel(3, 24) == white && many_pixel(4, 24) == white);

    /* Some channels only. */
    many_surface();
    clear(0xFF102030u);
    method(0x1D90, 0xFFFFFFFFu);
    method(0x1D94, 0x23);
    flip();
    CHECK(many_pixel(5, 5) == 0xFF10FF30u && many_pixel(MANY_W - 1, MANY_H - 1) == 0xFF10FF30u);

    /* Its depths as a texture. */
    many_surface();
    clear(0);
    depths(1);
    quad(0, 0, 8, 16, 4000000.0f, 1, 1);
    depths(0);
    main_surface();
    bind_linear(MANY_Z, 0x2E, MANY_W, MANY_H, MANY_W * 4);
    method(0x1E70, 2);
    method(0x1E6C, 1);
    pass(8);
    compared(5000000.0f, 1, 16, 0, 0);
    CHECK(pixel(3, 3) == white && pixel(12, 3) == 0);
    method(0x1E6C, 0);
    method(0x1E70, 0);
    pass(4);

    /* The same memory larger: both are kept. */
    surface(MANY_AT + 0x70000u, MANY_W, MANY_H, MANY_W * 4, 0x0128);
    method(0x0214, MANY_Z + 0x8000u);
    depths(1);
    clear(0xFF000000u);
    diffuse(0xFF000000u | n);                    /* not as it was before */
    quad(0, 0, 8, 8, 4000000.0f, 1, 1);
    surface(MANY_AT + 0x70000u, MANY_W, MANY_H + 20, MANY_W * 4, 0x0128);
    method(0x0214, MANY_Z + 0x8000u);
    diffuse(0xFFFF0000u);
    quad(4, 0, 12, 8, 5000000.0f, 1, 1);
    quad(0, MANY_H, 8, MANY_H + 8, 16000000.0f, 1, 1);
    depths(0);
    flip();
    CHECK(word(MANY_AT + 0x70000u + 6 * 4) == (0xFF000000u | n)
          && word(MANY_AT + 0x70000u + 10 * 4) == 0xFFFF0000u);
    main_surface();
}

/* The edge of a cut-out, from alpha 0 to 1 over 16 pixels: half of one is
 * covered, whichever side is drawn. */
static uint32_t cut_out(uint32_t func, uint32_t ref, int blend)
{
    uint32_t mem[4] = { 0x00FFFFFFu, 0xFFFFFFFFu, 0x00FFFFFFu, 0xFFFFFFFFu };

    memcpy(s_mem + TEX_AT, mem, sizeof mem);
    many_surface();
    depths(1);
    clear(0xFF000000u);
    pass(8);
    bind_linear(TEX_AT, 0x12, 2, 2, 8);
    method(0x1B14, 0x02022000u);
    method(0x0300, 1);
    method(0x033C, func);
    method(0x0340, ref);
    method(0x0304, (uint32_t)blend);
    quad(0, 0, 32, 8, 4000000.0f, 2, 2);
    method(0x0304, 0);
    method(0x0300, 0);
    method(0x1E70, 0);
    pass(4);
    depths(0);
    flip();
    main_surface();
    return many_pixel(15, 4);
}

/* As wide as alpha's own edge, whole where that is 1 or does not change. */
static void cut_out_soft(uint32_t n)
{
    uint32_t x, wrong = 0;

    nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_SOFT);
    cut_out(0x204, 200, 0);
    for (x = 0; n > 1 && x < 32; x++) {
        float a = ((float)x + 0.5f - 8.0f) / 16.0f;
        uint32_t v = (uint32_t)((a < 0 ? 0 : a > 1 ? 1 : a) * (float)n + 0.5f) * 255 / n;

        wrong += !between(many_pixel(x, 4), v < 2 ? 0 : v - 2, v + 2);
    }
    CHECK(wrong == 0);
    cut_out(0x201, 200, 0);
    CHECK((many_pixel(4, 4) | 0xFF000000u) == 0xFFFFFFFFu && many_pixel(28, 4) << 8 == 0);
    CHECK(n == 1 || between(many_pixel(13, 4), 0x7E, 0xE0));
    CHECK(n > 1 || (many_pixel(20, 4) << 8 != 0 && many_pixel(21, 4) << 8 == 0));
    CHECK(cut_out(0x204, 200, 1) << 8 == 0);
    many_surface();
    depths(1);
    clear(0xFF000000u);
    method(0x0300, 1);
    method(0x033C, 0x204);
    method(0x0340, 0x40);
    diffuse(0x60FFFFFFu);
    quad(0, 0, 8, 8, 4000000.0f, 1, 1);
    method(0x0300, 0);
    depths(0);
    flip();
    CHECK((many_pixel(4, 4) | 0xFF000000u) == 0xFFFFFFFFu);
    main_surface();
}

/* A test for each sample: the edge is where it is in the pixel. A flat picture's
 * pixels are whole. */
static void cut_out_each(uint32_t n)
{
    nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_SAMPLE);
    CHECK(cut_out(0x204, 120, 0) << 8 == 0 && many_pixel(16, 4) << 8 == 0xFFFFFF00u);
    s_quad_w = 2.0f;
    CHECK(between(cut_out(0x204, 120, 0), 0x40, 0xC0) || n == 1);
    CHECK(many_pixel(14, 4) << 8 == 0 && many_pixel(16, 4) << 8 == 0xFFFFFF00u);
    CHECK(between(cut_out(0x203, 120, 0), 0x40, 0xC0) || n == 1);
    CHECK((many_pixel(14, 4) | 0xFF000000u) == 0xFFFFFFFFu && many_pixel(16, 4) << 8 == 0);
    CHECK(n < 4 || between(cut_out(0x204, 116, 0), 0x80, 0xF0));    /* more of it */
    CHECK(n < 4 || between(cut_out(0x204, 124, 0), 0x20, 0x80));
    s_quad_w = 1.0f;
}

/* 16x16 texels on 4x4 pixels, of a picture's or of something at a distance. */
static uint32_t textured(float w, uint32_t control0, uint32_t filter)
{
    many_surface();
    depths(1);
    clear(0xFF000000u);
    pass(8);
    bind_texture(A_AT, 0x06, 4, 4);
    method(0x1B04, 0x28u | 0x06u << 8 | 3u << 16 | 4u << 20 | 4u << 24);
    method(0x1B0C, control0);
    method(0x1B14, filter);
    s_quad_w = w;
    quad(0, 0, 4, 4, 4000000.0f, 1, 1);
    s_quad_w = 1.0f;
    method(0x1B0C, 0x4003FFC0u);
    method(0x1E70, 0);
    pass(4);
    depths(0);
    flip();
    main_surface();
    return many_pixel(1, 1);
}

/* Each sample shaded: what is in a pixel of a texture is all seen, and its level
 * is what as many pixels would have. */
static void shaded_each(uint32_t n)
{
    static const uint32_t colors[3] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
    const uint32_t mips = 0x4003FFC0u, one = 0x40000000u;
    uint32_t level, i, mx, my, v, at = A_AT;

    xbox_swizzle_masks(16, 16, &mx, &my);
    for (level = 0; level < 3; level++)
        for (i = 0; i < 256u >> 2 * level; i++, at += 4)
            memcpy(s_mem + at, &colors[level], 4);
    show();
    CHECK(textured(2, mips, 0x01032000u) == colors[2]);
    nv2a_vk_shade_samples(1);
    CHECK(nv2a_vk_shade_samples_is());
    CHECK(n != 4 || textured(2, mips, 0x01032000u) == colors[1]);
    method(0x0300, 1);                          /* but a cut-out's level is the pixel's */
    method(0x033C, 0x204);
    method(0x0340, 0x40);
    for (i = NV2A_VK_ALPHA_TEST_AA_SOFT; i < NV2A_VK_ALPHA_TEST_AA_MODES; i++) {
        nv2a_vk_alpha_test_aa((Nv2aVkAlphaTestAa)i);
        CHECK(textured(2, mips, 0x01032000u) == colors[2]);
    }
    method(0x0300, 0);
    CHECK(textured(1, mips, 0x01032000u) == colors[2]);
    /* Cut-outs each way as well. */
    cut_out_soft(n);
    cut_out_each(n);
    for (i = 1; i <= 2; i++) {
        s_quad_w = (float)i;
        nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_PIXEL);
        CHECK(cut_out(0x204, 120, 0) << 8 == 0 && many_pixel(16, 4) << 8 == 0xFFFFFF00u);
        nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_SHARP);
        CHECK(between(cut_out(0x204, 120, 0), 0x40, 0xC0) || n == 1);
        CHECK(many_pixel(13, 4) << 8 == 0 && (many_pixel(17, 4) | 0xFF000000u) == 0xFFFFFFFFu);
        CHECK(n < 4 || between(cut_out(0x201, 121, 0), 0x40, 0xC0));
        nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_SOFT);  /* 0.16 and 0.78 of the samples */
        cut_out(0x204, 120, 0);
        CHECK(n < 4 || between(many_pixel(10, 4), 0x10, 0x48));
        CHECK(n < 4 || between(many_pixel(20, 4), 0xB0, 0xE8));
        CHECK(cut_out(0x204, 120, 1) << 8 == 0 || i == 2);      /* not when it blends */
    }
    nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_SHARP);
    s_quad_w = 1.0f;
    for (i = 0; i < 256; i++) {                 /* columns, half of each pixel */
        v = (i & 15) % 4 < 2 ? 0xFFFFFFFFu : 0xFF000000u;
        memcpy(s_mem + A_AT + 4 * (swizzle_deposit(i & 15, mx) | swizzle_deposit(i >> 4, my)),
               &v, 4);
    }
    show();
    v = textured(1, one, 0x01012000u) & 0xFFFFFFu;
    CHECK(v == 0 || v == 0xFFFFFF);
    CHECK(n == 1 || between(textured(2, one, 0x01012000u), 0x60, 0xA0));
    nv2a_vk_shade_samples(0);
    v = textured(2, one, 0x01012000u) & 0xFFFFFFu;
    CHECK(v == 0 || v == 0xFFFFFF);
}

static void test_many(void)
{
    uint32_t n, s, x;

    setup();
    for (s = 1; s <= 3; s++)
        for (n = 1; n <= 8; n *= 2) {
            nv2a_vk_scale((int)s);
            nv2a_vk_samples((int)n);
            CHECK(nv2a_vk_samples_is() == (int)n);
            flip();
            test_equal_depths();
            if (s == 1) {
                many_at(n);
                CHECK(cut_out(0x204, 120, 0) != 0xFF000000u || n == 1);
                CHECK(between(cut_out(0x204, 120, 0), 0x40, 0xC0) || n == 1);
                CHECK(many_pixel(14, 4) << 8 == 0 && (many_pixel(16, 4) | 0xFF000000u) == 0xFFFFFFFFu);
                CHECK(many_pixel(4, 4) << 8 == 0 && many_pixel(28, 4) == 0xFFFFFFFFu);
                CHECK(n == 1 || between(cut_out(0x206, 121, 0), 0x40, 0xC0));
                CHECK(n == 1 || between(cut_out(0x201, 121, 0), 0x40, 0xC0));
                CHECK(between(cut_out(0x203, 120, 0), 0x40, 0xC0) || n == 1);
                cut_out(0x203, 120, 0);
                CHECK((many_pixel(14, 4) | 0xFF000000u) == 0xFFFFFFFFu && many_pixel(16, 4) << 8 == 0);
                CHECK(cut_out(0x204, 254, 0) << 8 == 0 && many_pixel(25, 4) == 0xFFFFFFFFu
                      && many_pixel(22, 4) << 8 == 0);
                CHECK(cut_out(0x204, 120, 1) << 8 == 0);        /* not when it blends */
                for (x = 0; x < 4; x++) {       /* all or none, to the last step */
                    static const uint32_t func[4] = { 0x204, 0x206, 0x201, 0x203 };

                    many_surface();
                    depths(1);
                    clear(0xFF000000u);
                    method(0x0300, 1);
                    method(0x033C, func[x]);
                    method(0x0340, 0x80);
                    diffuse(0x7FFFFFFFu);
                    quad(0, 0, 8, 8, 4000000.0f, 1, 1);
                    diffuse(0x80FFFFFFu);
                    quad(10, 0, 18, 8, 4000000.0f, 1, 1);
                    diffuse(0x81FFFFFFu);
                    quad(20, 0, 28, 8, 4000000.0f, 1, 1);
                    method(0x0300, 0);
                    depths(0);
                    flip();
                    CHECK((many_pixel(4, 4) << 8 != 0) == (x >= 2));
                    CHECK((many_pixel(14, 4) << 8 != 0) == (x == 1 || x == 3));
                    CHECK((many_pixel(24, 4) << 8 != 0) == (x < 2));
                    main_surface();
                }
                nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_PIXEL);
                CHECK(cut_out(0x204, 120, 0) << 8 == 0 && many_pixel(16, 4) << 8 == 0xFFFFFF00u);
                cut_out_soft(n);
                cut_out_each(n);
                test_depth_kinds();
                test_w_buffer();
                nv2a_vk_alpha_test_aa(NV2A_VK_ALPHA_TEST_AA_SHARP);
                shaded_each(n);
            } else {
                /* Larger as well: averaged twice on its way back. */
                many_surface();
                depths(1);
                clear(0xFF000000u);
                diffuse(0xFFFFFFFFu);
                quad(0, 0, 8, 8, 4000000.0f, 1, 1);
                count_on();
                quad(16, 0, 24, 8, 4000000.0f, 1, 1);
                ask(0);
                depths(0);
                flip();
                CHECK(many_pixel(7, 7) == 0xFFFFFFFFu && many_pixel(8, 7) == 0xFF000000u);
                CHECK(answer(0) == 64);
                main_surface();
            }
        }
    /* What is there stays when they change; not every number is one. */
    nv2a_vk_scale(1);
    nv2a_vk_samples(4);
    flip();
    many_surface();
    depths(1);
    clear(0xFF00FFFFu);
    nv2a_vk_samples(3);
    nv2a_vk_samples(16);
    nv2a_vk_samples(0);
    CHECK(nv2a_vk_samples_is() == 4);
    nv2a_vk_samples(1);
    flip();
    CHECK(many_pixel(50, 50) == 0xFF00FFFFu);
    /* A texture's: square, and drawn with one. */
    nv2a_vk_samples(4);
    flip();
    /* A depth buffer drawn without colours first has several once it has some,
     * and then without them too. */
    surface(0, MANY_W, MANY_H, MANY_W * 4, 0x0128);
    method(0x0214, MANY_Z + 0x40000u);
    clear(0);
    for (x = 0; x < 2; x++) {
        many_surface();
        method(0x0214, MANY_Z + 0x40000u);
        clear(0xFF000000u);
        method(0x0210, 0);
        depths(1);
        quad(0, 0, 10.6875f, 8, 4000000.0f, 1, 1);
        many_surface();
        method(0x0214, MANY_Z + 0x40000u);
        diffuse(0xFFFFFFFFu);
        quad(4, 0, 20, 8, 8000000.0f, 1, 1);
        depths(0);
        flip();
        CHECK(many_pixel(6, 4) == 0xFF000000u && many_pixel(14, 4) == 0xFFFFFFFFu);
        CHECK(between(many_pixel(10, 4), 0x40, 0xC0));
    }
    texels(MANY_Z + 0x40000u, 0x12, MANY_W, MANY_H, MANY_W * 4);
    CHECK(pixel(3, 3) == 0x3D090000u && pixel(14, 14) == 0xFFFFFF00u);
    method(0x1E70, 0);
    pass(4);
    surface(MANY_AT, MANY_W, MANY_W, MANY_W * 4, 0x0128);
    method(0x0214, MANY_Z + 0x10000u);
    mark();
    clear(0xFF000000u);
    diffuse(0xFFFFFFFFu);
    quad(10, 4, 20.6875f, 12, 4000000.0f, 1, 1);
    depths(0);
    flip();
    CHECK(many_pixel(20, 8) == 0xFFFFFFFFu && many_since().resolved == 0);
    nv2a_vk_samples(1);
    main_surface();
    flip();
}

/* All that draws, with the title's CPU looking at each result. */
static void test_all(void)
{
    setup();
    test_clear();
    test_coverage();
    test_depth();
    test_float_depth();
    test_stencil();
    test_cull();
    test_blend();
    test_logic();
    test_alpha();
    test_reports();
    test_perf();
    test_formats();
    test_dither();
    test_texture();
    test_cpu_writes();
    test_drawn();
    test_anisotropy();
    test_lines_and_points();
    test_sizes();
    test_fill_modes();
    test_border();
    test_combiners();
    test_color_sum();
    test_two_sided();
    test_stages();
    test_depth_textures();
    test_depth_kinds();
    test_w_buffer();
    test_vertex_formats();
    test_vertex_sources();
    test_vertex_paths();
    test_program_corners();
    test_programs();
    test_scale();
    test_many();
}

/* The pipelines' file: whole and this card's, or nothing. */
static void test_cache_file(void)
{
    static const char path[] = "nv2a_vk_test.cache";
    Nv2aVkPipelineCacheOf of = {
        0x10DE, 0x2206, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } };
    Nv2aVkPipelineCacheOf other = of;
    uint32_t first[4] = { 32, 1, 0x10DE, 0x2206 };
    uint8_t data[100], *got;
    size_t n;
    FILE *f;
    int i;

    memset(data, 0xA5, sizeof data);
    memcpy(data, first, 16);
    memcpy(data + 16, of.uuid, 16);
    remove(path);
    CHECK(nv2a_vk_pipeline_cache_read(path, &of, &n) == NULL && n == 0);
    CHECK(nv2a_vk_pipeline_cache_write(path, data, sizeof data));
    data[99] = 0x5A;
    CHECK(nv2a_vk_pipeline_cache_write(path, data, sizeof data));   /* over the old one */
    got = (uint8_t *)nv2a_vk_pipeline_cache_read(path, &of, &n);
    CHECK(got && n == sizeof data && memcmp(got, data, n) == 0);
    free(got);
    other.device++;
    CHECK(!nv2a_vk_pipeline_cache_read(path, &other, &n));
    other = of;
    other.vendor++;
    CHECK(!nv2a_vk_pipeline_cache_read(path, &other, &n));
    other = of;
    other.uuid[15]++;                           /* another driver */
    CHECK(!nv2a_vk_pipeline_cache_read(path, &other, &n) && n == 0);
    for (i = 0; i < 2; i++) {                   /* damaged, then cut short */
        f = fopen(path, "r+b");
        CHECK(f != NULL);
        if (f) {
            fseek(f, i ? 0 : 60, SEEK_SET);
            for (n = 0; n < (i ? 8u : 1u); n++)
                fputc(0x11, f);
            fclose(f);
        }
        CHECK(!nv2a_vk_pipeline_cache_read(path, &of, &n));
        CHECK(nv2a_vk_pipeline_cache_write(path, data, sizeof data));
    }
    f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, 50, f);
        fclose(f);
    }
    CHECK(!nv2a_vk_pipeline_cache_read(path, &of, &n));
    CHECK(!nv2a_vk_pipeline_cache_write("", data, sizeof data));
    CHECK(!nv2a_vk_pipeline_cache_read("", &of, &n));
    CHECK(!nv2a_vk_pipeline_cache_write(path, data, 31));
    remove(path);
}

/* Pipelines of a state's own: there after a while, at once or never. */
static void test_shader_compile(void)
{
    Nv2aVkStats was, st;
    unsigned i;

    nv2a_vk_shader_compile(NV2A_VK_COMPILE_ASYNC);
    method(0x0300, 1);
    method(0x033C, 0x205);                      /* an alpha test nothing else has */
    method(0x0340, 0x55);
    nv2a_vk_stats(&was);
    for (i = 0; i < 1000; i++) {
        clear(0xFF000000u);
        rect(0, 0, 10, 10, 0x80FF8040u);
        rect(20, 0, 30, 10, 0x55FF8040u);       /* killed */
        flip();
        CHECK((pixel(5, 5) & 0xFFFFFF) == 0xFF8040u && (pixel(25, 5) & 0xFFFFFF) == 0);
        nv2a_vk_stats(&st);
        if (st.specialized_draws > was.specialized_draws)
            break;
        Sleep(10);
    }
    CHECK(st.specialized_draws > was.specialized_draws && st.specialized_pipelines > 0);
    for (i = 0; i < 1000 && st.specialized_due; i++) {
        Sleep(10);
        nv2a_vk_stats(&st);
    }
    CHECK(!st.specialized_due);

    nv2a_vk_shader_compile(NV2A_VK_COMPILE_OFF);
    was = st;
    rect(0, 0, 10, 10, 0x80FF8040u);
    flip();
    nv2a_vk_stats(&st);
    CHECK(st.specialized_draws == was.specialized_draws && st.draws > was.draws);

    nv2a_vk_shader_compile(NV2A_VK_COMPILE_SYNC);
    method(0x033C, 0x202);                      /* another */
    was = st;
    clear(0xFF000000u);
    rect(0, 0, 10, 10, 0x55FF8040u);
    flip();
    nv2a_vk_stats(&st);
    CHECK(st.specialized_draws - was.specialized_draws == st.draws - was.draws);
    CHECK(st.specialized_pipelines > was.specialized_pipelines);
    CHECK((pixel(5, 5) & 0xFFFFFF) == 0xFF8040u);
    method(0x0300, 0);
    method(0x033C, 0x207);
}

int main(int argc, char **argv)
{
    Nv2aVkStats st;

    (void)argc; (void)argv;
    nv2a_pb_exec_set_resolver(offset_va);
    test_cache_file();
#if defined(_WIN32)
    _putenv_s("RECOMP_NV2A_VK_PIPELINE_CACHE", "0");
#endif
    if (!memory_init()) {
        puts("no guest memory: skipped");
        return 77;
    }
    if (!nv2a_vk_init()) {
        puts("no Vulkan here: skipped");
        return 77;
    }
    nv2a_vk_copyback(NV2A_VK_COPYBACK_ALWAYS);
    test_all();
    test_shader_compile();
    nv2a_vk_stats(&st);
    CHECK(st.refused == 0 && st.draws > 400);    /* all of it drawn here */
    if (!s_fail)
        puts("NV2A Vulkan draws: ok");
    return s_fail;
}
