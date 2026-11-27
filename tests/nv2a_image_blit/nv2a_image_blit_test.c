/* 2D copies on the software GPU: a CONTEXT_SURFACES_2D object names the two
 * surfaces and an IMAGE_BLIT object copies a rectangle between them when SIZE
 * arrives. D3D's CopyRects sends these on subchannels other than the 3D
 * class's, and the executor used to drop them, so a copy (PersistDisplay's
 * among them) left its destination as it was.
 *
 * The object table is faked: a hash table at the start of RAMIN, instances
 * after it, so the subchannels get their classes and the DMA objects their
 * places. The destination's DMA object starts past 0, so offsets are taken
 * inside it. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x100000u
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

/* PFIFO's hash table register (0 = at RAMIN, 4 KB) and RAMIN. */
#define RAMIN 0x700000u
static uint32_t s_ramht_reg;
static uint32_t s_ramin[0x2000 / 4];

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    if (offset == 0x002210)
        return &s_ramht_reg;
    if (offset >= RAMIN && offset - RAMIN < sizeof s_ramin)
        return &s_ramin[(offset - RAMIN) / 4];
    return NULL;
}

/* Hash entry `slot` pairs `handle` with the instance at RAMIN + inst. */
static void object(uint32_t slot, uint32_t handle, uint32_t inst,
                   uint32_t w0, uint32_t w1, uint32_t w2)
{
    s_ramin[slot * 2] = handle;
    s_ramin[slot * 2 + 1] = 0x80000000u | (inst >> 4);
    s_ramin[inst / 4] = w0;
    s_ramin[inst / 4 + 1] = w1;
    s_ramin[inst / 4 + 2] = w2;
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define SURF 3          /* subchannel of the 2D surfaces */
#define BLIT 2          /* subchannel of the blit */
#define DST_DMA_AT 0x40000u          /* where the destination object starts */
#define SRC_AT   0x10000u            /* source surface, an offset from 0 */
#define DST_OFF  0x8000u             /* destination, inside its object */
#define SPITCH   256u
#define DPITCH   512u

static uint32_t src32(uint32_t x, uint32_t y) { return 0xA0000000u | (y << 12) | x; }

static uint32_t rd32(uint32_t at) { uint32_t v; memcpy(&v, s_mem + at, 4); return v; }

static void surfaces(uint32_t format, uint32_t src, uint32_t dst)
{
    nv2a_pb_exec_method(SURF, 0x300, format);                   /* COLOR_FORMAT */
    nv2a_pb_exec_method(SURF, 0x304, (DPITCH << 16) | SPITCH);  /* PITCH */
    nv2a_pb_exec_method(SURF, 0x308, src);                      /* OFFSET_SOURCE */
    nv2a_pb_exec_method(SURF, 0x30C, dst);                      /* OFFSET_DESTIN */
}

static void blit(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
                 uint32_t w, uint32_t h)
{
    nv2a_pb_exec_method(BLIT, 0x300, (sy << 16) | sx);          /* CONTROL_POINT_IN */
    nv2a_pb_exec_method(BLIT, 0x304, (dy << 16) | dx);          /* CONTROL_POINT_OUT */
    nv2a_pb_exec_method(BLIT, 0x308, (h << 16) | w);            /* SIZE */
}

int main(void)
{
    const uint32_t dst = DST_DMA_AT + DST_OFF;
    uint32_t x, y, bad;

    /* Objects: the two 2D classes, a DMA object over all of memory and one
     * that starts at DST_DMA_AT and is 64 KB long. */
    object(0, 0x11, 0x1000, 0x62, 0, 0);
    object(1, 0x10, 0x1010, 0x9F, 0, 0);
    object(2, 0x03, 0x1020, 0x3000u | 0x3D, MEM_BYTES - 1, 0);
    object(3, 0x0B, 0x1030, 0x3000u | 0x3D, 0xFFFFu, DST_DMA_AT);
    nv2a_pb_exec_method(SURF, 0, 0x11);
    nv2a_pb_exec_method(BLIT, 0, 0x10);
    nv2a_pb_exec_method(SURF, 0x184, 0x03);                     /* IMAGE_SOURCE */
    nv2a_pb_exec_method(SURF, 0x188, 0x0B);                     /* IMAGE_DESTIN */

    for (y = 0; y < 64; y++)
        for (x = 0; x < SPITCH / 4; x++) {
            uint32_t v = src32(x, y);
            memcpy(s_mem + SRC_AT + y * SPITCH + 4 * x, &v, 4);
        }

    /* No operation set yet: not modelled, nothing written. */
    surfaces(0x0A, SRC_AT, DST_OFF);
    blit(0, 0, 0, 0, 8, 8);
    CHECK(rd32(dst) == 0, "copied with no operation set: %08X", rd32(dst));

    /* SRCCOPY, 32 bits: a 20x10 rectangle from (5, 3) to (7, 2). */
    nv2a_pb_exec_method(BLIT, 0x2FC, 3);                        /* SET_OPERATION */
    blit(5, 3, 7, 2, 20, 10);
    bad = 0;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 40; x++) {
            int in = x >= 7 && x < 27 && y >= 2 && y < 12;
            uint32_t want = in ? src32(x - 7 + 5, y - 2 + 3) : 0;
            if (rd32(dst + y * DPITCH + 4 * x) != want)
                bad++;
        }
    CHECK(bad == 0, "32-bit copy: %u pixels wrong", bad);

    /* 16 bits: the same rectangle in R5G6B5 copies half the bytes. */
    memset(s_mem + dst, 0, 16 * DPITCH);
    surfaces(0x04, SRC_AT, DST_OFF);
    blit(2, 1, 0, 0, 6, 4);
    bad = 0;
    for (y = 0; y < 5; y++)
        for (x = 0; x < 8; x++) {
            uint16_t got, want = 0;
            memcpy(&got, s_mem + dst + y * DPITCH + 2 * x, 2);
            if (x < 6 && y < 4)
                memcpy(&want, s_mem + SRC_AT + (y + 1) * SPITCH + 2 * (x + 2), 2);
            if (got != want)
                bad++;
        }
    CHECK(bad == 0, "16-bit copy: %u pixels wrong", bad);

    /* Past the destination object's end: refused, nothing written. */
    memset(s_mem + dst, 0, 16 * DPITCH);
    surfaces(0x0A, SRC_AT, 0xFF00u);
    blit(0, 0, 0, 0, 8, 2);
    CHECK(rd32(DST_DMA_AT + 0xFF00u) == 0, "copied past the DMA object's limit");

    /* Overlapping, inside one surface: rows 0-9 down by one row. */
    nv2a_pb_exec_method(SURF, 0x188, 0x03);                     /* IMAGE_DESTIN */
    surfaces(0x0A, SRC_AT, SRC_AT);
    nv2a_pb_exec_method(SURF, 0x304, (SPITCH << 16) | SPITCH);
    blit(0, 0, 0, 1, 16, 10);
    bad = 0;
    for (y = 1; y <= 10; y++)
        for (x = 0; x < 16; x++)
            if (rd32(SRC_AT + y * SPITCH + 4 * x) != src32(x, y - 1))
                bad++;
    CHECK(bad == 0, "overlapping copy: %u pixels wrong", bad);

    if (!s_fail)
        printf("nv2a image blit: ok\n");
    return s_fail ? 1 : 0;
}
