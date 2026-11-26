/*
 * Captures a frame drawn by the CPU rasteriser and writes what it drew, for
 * nv2a-replay to draw again in a process that has nothing but the file.
 *
 * The frame leans on what a capture can lose: state set long before it, a
 * swizzled texture whose size comes from the format word, a vertex buffer the
 * title rewrites between two draws, vertices sent in the commands, a texture
 * the frame drew itself.
 *
 *   nv2a-capture-test <prefix>: <prefix>-0.nvcap and <prefix>.raw
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_capture.h"

#define MEM_BYTES 0x40000u
#define COLOR_AT  0x10000u
#define SMALL_AT  0x20000u
#define TEX_AT    0x30000u
#define POS_AT    0x32000u
#define UV_AT     0x33000u
#define W 64
#define H 48

static uint8_t s_mem[MEM_BYTES];

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
/* Every other offset means an address it does not equal: a replay that
 * ignores what was resolved draws somewhere else. */
static int offset_va(uint32_t pa, uint32_t *va)
{
    if (pa < 0x01000000u || pa >= 0x01000000u + MEM_BYTES)
        return 0;
    *va = pa - 0x01000000u;
    return 1;
}
int xbox_mem2_phys_to_guest(uint32_t pa, uint32_t *va) { (void)pa; (void)va; return 0; }
int xbox_mem2_arena_has(uint32_t va, uint32_t bytes) { (void)va; (void)bytes; return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
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

static void surface(uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch)
{
    method(0x0200, w << 16);
    method(0x0204, h << 16);
    method(0x020C, pitch);
    method(0x0210, offset);
}

/* A quad in surface pixels through the composite matrix set below. */
static void put_quad(float x0, float y0, float x1, float y1)
{
    const float p[16] = { x0, y0, 0, 1,  x1, y0, 0, 1,  x1, y1, 0, 1,  x0, y1, 0, 1 };
    const float t[8]  = { 0, 0,  1, 0,  1, 1,  0, 1 };
    unsigned i;

    for (i = 0; i < 16; i++)
        put_float(POS_AT + 4 * i, p[i]);
    for (i = 0; i < 8; i++)
        put_float(UV_AT + 4 * i, t[i]);
}

static void draw_quad(void)
{
    method(0x17FC, 8);
    method(0x1810, 3u << 24);
    method(0x17FC, 0);
}

static void setup(void)
{
    static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    unsigned i;

    for (i = 0; i < 0x1000; i++)
        s_mem[TEX_AT + i] = (uint8_t)(i * 37 + 11);
    method(0x0208, 0x0118);                          /* 32-bit, pitch */
    surface(0x01000000u + COLOR_AT, W, H, W * 4);
    for (i = 0; i < 16; i++)
        method_float(0x0680 + 4 * i, identity[i]);
    method(0x1E94, 0);
    method(0x1720, 0x01000000u + POS_AT);
    method(0x1760, 2 | (4 << 4) | (16 << 8));
    method(0x1720 + 4 * 9, 0x01000000u + UV_AT);
    method(0x1760 + 4 * 9, 2 | (2 << 4) | (8 << 8));
    /* The texture's size comes from the format word; the rectangle, sent
     * before it, is stale. */
    method(0x1B1C, (8u << 16) | 8u);
    method(0x1B00, 0x01000000u + TEX_AT);
    method(0x1B04, 0x28u | (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24));
    method(0x1B08, 0x00010101u);
    method(0x1B14, 0x02062000u);
    method(0x1B0C, 0x4003FFC0u);
}

static void frame(int salt)
{
    unsigned i, k;
    static const float tri[3][2] = { { 4, 30 }, { 20, 30 }, { 12, 46 } };

    /* A small target first, read back as a texture below. */
    surface(0x01000000u + SMALL_AT, 16, 16, 16 * 4);
    method(0x1D90, 0xFF00FF00u + (uint32_t)salt);
    method(0x1D94, 0xF0);
    put_quad(2, 2, 14, 14);
    draw_quad();

    surface(0x01000000u + COLOR_AT, W, H, W * 4);
    method(0x1D90, 0xFF203040u);
    method(0x1D94, 0xF0);
    put_quad(0, 0, 32, 24);
    draw_quad();
    put_quad(30, 10, 60, 40);                        /* the same buffer, rewritten */
    draw_quad();
    method(0x17FC, 5);                               /* vertices in the commands */
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 2; k++)
            method_float(0x1818, tri[i][k]);
        method_float(0x1818, 0.0f);
        method_float(0x1818, 1.0f);
        method_float(0x1818, (float)(i & 1));
        method_float(0x1818, (float)(i >> 1));
    }
    method(0x17FC, 0);
    /* The small target as a linear texture. */
    method(0x1B00, 0x01000000u + SMALL_AT);
    method(0x1B04, 0x28u | (0x12u << 8) | (1u << 16));
    method(0x1B10, 64u << 16);
    method(0x1B1C, (16u << 16) | 16u);
    put_float(UV_AT + 8, 16); put_float(UV_AT + 16, 16); put_float(UV_AT + 20, 16);
    {
        const float p[8] = { 40, 40, 56, 40, 56, 47, 40, 47 };
        for (i = 0; i < 4; i++) {
            put_float(POS_AT + 16 * i, p[2 * i]);
            put_float(POS_AT + 16 * i + 4, p[2 * i + 1]);
        }
        put_float(UV_AT + 28, 16);
    }
    draw_quad();
    method(0x1B00, 0x01000000u + TEX_AT);
    method(0x1B1C, (8u << 16) | 8u);
    method(0x1B04, 0x28u | (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24));
    method(0x0130, 0);                               /* FLIP_STALL */
}

int main(int argc, char **argv)
{
    static uint8_t drawn[W * H * 4];
    char env[1200], go[1100];
    unsigned distinct = 0, i;
    FILE *f;

    if (argc < 2)
        return 2;
    snprintf(env, sizeof env, "RECOMP_FRAME_CAPTURE=%s", argv[1]);
#if defined(_WIN32)
    _putenv(env);
#else
    putenv(env);
#endif
    nv2a_capture_init();
    nv2a_pb_exec_set_resolver(offset_va);
    setup();
    frame(0);
    frame(2);
    memcpy(drawn, s_mem + COLOR_AT, sizeof drawn);
    snprintf(go, sizeof go, "%s.go", argv[1]);
    f = fopen(go, "wb");
    if (!f)
        return 2;
    fclose(f);
    frame(1);                                        /* sees .go at its end */
    frame(2);                                        /* captured */
    f = fopen(go, "rb");
    if (f) {
        fprintf(stderr, "FAIL: %s is still there\n", go);
        return 1;
    }
    for (i = 4; i < W * H * 4; i += 4)
        distinct += memcmp(drawn + i, drawn + i - 4, 4) != 0;
    if (distinct < 200) {
        fprintf(stderr, "FAIL: the frame is nearly flat (%u changes)\n", distinct);
        return 1;
    }
    snprintf(env, sizeof env, "%s.raw", argv[1]);
    f = fopen(env, "wb");
    if (!f || fwrite(drawn, W * 4, H, f) != H)
        return 2;
    fclose(f);
    frame(3);                                        /* must not be captured */
    printf("captured, %u changes\n", distinct);
    return 0;
}
