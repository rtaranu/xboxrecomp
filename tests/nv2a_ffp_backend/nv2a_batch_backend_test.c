/*
 * The batch back end (nv2a_backend_batch.h): what a batch, a clear and a
 * vertex program look like on arrival, and that a refusal, a removal or
 * another version leaves every other path as it was.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend_batch.h"
#include "nv2a_backend_fixture.h"

#define MEM_BYTES 0x10000u
#define COLOR_AT  0x8000u
#define ZETA_AT   0xA000u

static uint8_t s_mem[MEM_BYTES];
static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

/* What the executor needs from the rest of the runtime. */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
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

static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

/* ---- The back end under test: keeps a copy of the last call. ---------- */

static struct {
    int draws, clears, accept;
    Nv2aRawBatch b;
    uint32_t indices[8];
    Nv2aBatchAttr attrs[16];
    Nv2aBatchTexture textures[4];
    Nv2aCombiner combiner;
    Nv2aRenderState state;
    Nv2aTarget target;
    uint32_t reg_0394, reg_09F8;
    uint32_t program0[4];
    float c5[4];
    float inline0[4];
    int transformed;
    Nv2aVshOutput out[4];
    float from[4][4];
    Nv2aRawClear clear;
    Nv2aTarget clear_target;
    Nv2aRenderState clear_state;
} s_got;

static int on_draw(const Nv2aRawBatch *b)
{
    uint32_t n = b->index_count < 4 ? b->index_count : 4;

    s_got.draws++;
    s_got.b = *b;
    memcpy(s_got.indices, b->indices,
           (b->index_count < 8 ? b->index_count : 8) * sizeof(uint32_t));
    memcpy(s_got.attrs, b->attrs, sizeof s_got.attrs);
    memcpy(s_got.textures, b->textures, sizeof s_got.textures);
    if (b->combiner)
        s_got.combiner = *b->combiner;
    s_got.state = *b->state;
    s_got.target = *b->target;
    s_got.reg_0394 = b->regs[0x0394 / 4];
    s_got.reg_09F8 = b->regs[0x09F8 / 4];
    memcpy(s_got.program0, b->program[0], sizeof s_got.program0);
    memcpy(s_got.c5, b->constants[5], sizeof s_got.c5);
    if (b->inline_data && b->inline_bytes >= sizeof s_got.inline0)
        memcpy(s_got.inline0, b->inline_data, sizeof s_got.inline0);
    s_got.transformed = nv2a_batch_transform(0, n, s_got.out);
    CHECK(!nv2a_batch_transform(b->index_count, 1, s_got.out));   /* past the end */
    CHECK(!nv2a_batch_positions(b->index_count, 1, s_got.from));
    CHECK(nv2a_batch_positions(0, n, s_got.from));
    return s_got.accept;
}

static int on_clear(const Nv2aTarget *t, const Nv2aRenderState *rs,
                    const Nv2aRawClear *c)
{
    s_got.clears++;
    s_got.clear = *c;
    s_got.clear_target = *t;
    s_got.clear_state = *rs;
    return s_got.accept;
}

static struct { int clears, reports, idles, accept; Nv2aReport r; } s_rep;

static void on_report_clear(void) { s_rep.clears++; }

static int on_report(const Nv2aReport *r)
{
    s_rep.reports++;
    s_rep.r = *r;
    return s_rep.accept;
}

static void on_idle(void) { s_rep.idles++; }

static const Nv2aBatchBackend s_ours = {
    NV2A_BATCH_VERSION, on_draw, on_clear, on_report_clear, on_report, on_idle
};

/* A 16x8 surface, positions at 0x2000 (float4 x 3) under an orthographic
 * composite matrix. */
static void configure(void)
{
    static const float matrix[16] = {
        8, 0, 0, 8,   0, -4, 0, 4,   0, 0, 1, 0,   0, 0, 0, 1,
    };
    static const float pos[12] = { -1, -1, 0, 1,   1, -1, 0, 1,   0, 1, 0, 1 };
    unsigned i;

    method(0x0208, 0x0118);                      /* 32-bit colour, Z16, pitch */
    method(0x0200, 16u << 16);
    method(0x0204, 8u << 16);
    method(0x020C, 64u | (32u << 16));
    method(0x0210, COLOR_AT);
    method(0x0214, ZETA_AT);
    for (i = 0; i < 16; i++)
        method_float(0x0680 + 4 * i, matrix[i]);
    method(0x1E94, 0);                           /* fixed function */
    for (i = 0; i < 12; i++)
        put_float(0x2000 + 4 * i, pos[i]);
    method(0x1720, 0x2000);
    method(0x1760, 2 | (4 << 4) | (16 << 8));
}

static void draw_arrays(uint32_t prim, uint32_t first, uint32_t count)
{
    method(0x17FC, prim);
    method(0x1810, first | ((count - 1u) << 24));
    method(0x17FC, 0);
}

static void test_registration(void)
{
    Nv2aBatchBackend other = s_ours;

    other.version = NV2A_BATCH_VERSION + 1;
    CHECK(!nv2a_backend_register_batches(&other));
    draw_arrays(5, 0, 3);
    CHECK(s_got.draws == 0);
    CHECK(nv2a_backend_register_batches(&s_ours));
}

static void test_fixed_function(void)
{
    s_got.accept = 1;
    memset(s_mem + COLOR_AT, 0, 64 * 8);
    method_float(0x0394, 0.25f);
    method(0x09F8, 0x12345678u);                 /* no field of its own */
    draw_arrays(5, 0, 3);
    CHECK(s_got.draws == 1 && s_got.b.prim == 5 && s_got.b.index_count == 3);
    CHECK(s_got.indices[0] == 0 && s_got.indices[2] == 2);
    CHECK(s_got.b.inline_data == NULL);
    CHECK(s_got.b.transform == NV2A_TRANSFORM_FIXED);
    CHECK(s_got.attrs[0].offset == 0x2000 && s_got.attrs[0].type == 2);
    CHECK(s_got.attrs[0].size == 4 && s_got.attrs[0].stride == 16);
    CHECK(s_got.attrs[3].size == 0 && near(s_got.attrs[3].current[3], 1));
    CHECK(s_got.target.color_va == COLOR_AT && s_got.target.zeta_va == ZETA_AT);
    CHECK(s_got.target.color_pitch == 64 && s_got.target.zeta_pitch == 32);
    CHECK(s_got.target.format == 0x0118 && s_got.target.bytes_per_pixel == 4);
    CHECK(s_got.target.clip_w == 16 && s_got.target.clip_h == 8);
    CHECK(s_got.target.aa_sx == 1 && s_got.target.aa_sy == 1);
    CHECK(s_got.b.combiner == NULL);
    CHECK(near(s_got.state.depth_min, 0.25f));
    CHECK(s_got.reg_0394 == 0x3E800000u && s_got.reg_09F8 == 0x12345678u);
    CHECK(s_got.transformed);
    CHECK(near(s_got.out[0].pos[0], 0) && near(s_got.out[0].pos[1], 8));
    CHECK(near(s_got.out[1].pos[0], 16) && near(s_got.out[2].pos[1], 0));
    CHECK(near(s_got.from[1][0], 1) && near(s_got.from[1][1], -1) && near(s_got.from[1][3], 1));
    CHECK(!nv2a_batch_transform(0, 1, s_got.out));   /* outside draw() */
    CHECK(!nv2a_batch_positions(0, 1, s_got.from));
    {
        unsigned i, touched = 0;
        for (i = 0; i < 64 * 8; i++)
            touched |= s_mem[COLOR_AT + i];
        CHECK(!touched);                         /* drawn once, not twice */
    }
}

/* Lit from behind: nxdk_pgraph_tests' TwoSidedLighting, whose picture from the
 * hardware has the back face's colours within 3 of 255 of these. */
static void test_two_sided(void)
{
    static const struct { uint32_t reg; float v[3]; } lit[] = {
        { 0x0A10, { 0.031373f, 0.031373f, 0.031373f } },    /* scene, front */
        { 0x17A0, { 0, 0, 0 } },
        { 0x1000, { 0, 0, 0 } },                            /* light 0: ambient */
        { 0x100C, { 1, 0, 0.25f } },                        /* diffuse */
        { 0x1034, { 0, 0, 1 } },                            /* direction */
        { 0x0C00, { 0, 0, 0.25f } },                        /* its back colours */
        { 0x0C0C, { 0, 0.75f, 0.35f } },
        { 0x0C40, { 9, 9, 9 } },                            /* light 1's, off */
    };
    unsigned i, k;

    for (i = 0; i < 16; i++)
        method_float(0x0480 + 4 * i, i % 5 ? 0.0f : 1.0f);
    for (i = 0; i < sizeof lit / sizeof lit[0]; i++)
        for (k = 0; k < 3; k++)
            method_float(lit[i].reg + 4 * k, lit[i].v[k]);
    method_float(0x03B4, 1.0f);
    method_float(0x17AC, 0.5f);
    method(0x0298, 0);
    method(0x03BC, 1);
    method(0x0314, 1);
    for (i = 0; i < 4; i++) {
        put_float(0x2800 + 12 * i, 0);
        put_float(0x2804 + 12 * i, 0);
        put_float(0x2808 + 12 * i, -1);
    }
    method(0x1720 + 4 * 2, 0x2800);
    method(0x1760 + 4 * 2, 2u | 3u << 4 | 12u << 8);
    method(0x17C4, 1);
    draw_arrays(5, 0, 3);
    CHECK(s_got.transformed);
    CHECK(near(s_got.out[0].d0[0], 0.031373f) && near(s_got.out[0].d0[3], 1));
    CHECK(near(s_got.out[0].b0[0], 0) && near(s_got.out[0].b0[1], 0.75f));
    CHECK(near(s_got.out[0].b0[2], 0.6f) && near(s_got.out[0].b0[3], 0.5f));
    CHECK(near(s_got.out[0].b1[0], 0) && near(s_got.out[0].b1[3], 1));
    method(0x0298, 1u << 12);                    /* the back diffuse: the vertex's */
    for (i = 0; i < 16; i++)
        put_float(0x2900 + 4 * i, 0.5f);
    method(0x1720 + 4 * 3, 0x2900);
    method(0x1760 + 4 * 3, 2u | 4u << 4 | 16u << 8);
    draw_arrays(5, 0, 3);
    CHECK(near(s_got.out[0].d0[0], 0.031373f) && near(s_got.out[0].b0[1], 0.375f));
    method(0x0298, 0);
    method(0x0314, 0);                           /* unlit */
    draw_arrays(5, 0, 3);
    CHECK(near(s_got.out[0].d0[0], 0.5f) && near(s_got.out[0].b0[1], 0));
    CHECK(near(s_got.out[0].b0[3], 1));
    method(0x17C4, 0);
    method(0x1760 + 4 * 2, 2);
    method(0x1760 + 4 * 3, 2);
}

/* The executor's own drawing needs three vertices; a batch back end is handed
 * points and lines. */
static void test_points(void)
{
    int before = s_got.draws;

    draw_arrays(1, 2, 1);
    CHECK(s_got.draws == before + 1);
    CHECK(s_got.b.prim == 1 && s_got.b.index_count == 1);
    CHECK(s_got.indices[0] == 2);
}

static void test_textures_and_combiner(void)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        method(0x1B00 + 0x40 * i, 0x3000 + 0x100 * i);
        /* A8R8G8B8 swizzled, BORDER from the colour, 2 levels, 4x8 */
        method(0x1B04 + 0x40 * i, 0x28u | (0x06u << 8) | (2u << 16)
                                | (2u << 20) | (3u << 24));
        method(0x1B08 + 0x40 * i, 0x00010203u);
        method(0x1B14 + 0x40 * i, 0x02000000u + i);
        method(0x1B24 + 0x40 * i, 0xFF000000u + i);
    }
    method(0x1E60, 0x00000002u);                 /* COMBINER_CONTROL */
    method(0x0AC0, 0x11223344u);                 /* COLOR_ICW 0 */
    method(0x1E70, 0x00000421u);                 /* SHADER_STAGE_PROGRAM */
    draw_arrays(5, 0, 3);
    for (i = 0; i < 4; i++) {
        CHECK(s_got.textures[i].valid);
        CHECK(s_got.textures[i].t.offset == 0x3000 + 0x100 * i);
        CHECK(s_got.textures[i].t.width == 4 && s_got.textures[i].t.height == 8);
        CHECK(s_got.textures[i].t.color == 0x06 && s_got.textures[i].t.levels == 2);
        CHECK(s_got.textures[i].t.addr_u == 3 && s_got.textures[i].t.addr_v == 2);
        CHECK(s_got.textures[i].t.filter == 0x02000000u + i);
        CHECK(s_got.textures[i].border == 0xFF000000u + i);
        CHECK(s_got.textures[i].border_color == 1 && !s_got.textures[i].cube);
    }
    CHECK(s_got.b.combiner != NULL);
    CHECK(s_got.combiner.control == 2 && s_got.combiner.stage_program == 0x421u);
    CHECK(s_got.combiner.color_icw[0] == 0x11223344u);
}

static void test_inline_and_immediate(void)
{
    int before = s_got.draws;
    unsigned i, k;

    method(0x17FC, 5);
    for (i = 0; i < 3; i++)
        for (k = 0; k < 4; k++)
            method_float(0x1818, (float)(10 * i + k));
    method(0x17FC, 0);
    CHECK(s_got.draws == before + 1);
    CHECK(s_got.b.inline_data != NULL && s_got.b.inline_bytes == 48);
    CHECK(s_got.b.index_count == 3 && s_got.attrs[0].offset == 0);
    CHECK(s_got.attrs[0].stride == 16 && near(s_got.inline0[1], 1));

    method_float(0x1A00 + 16 * 3 + 0, 0.5f);     /* diffuse, all four */
    method_float(0x1A00 + 16 * 3 + 4, 0.25f);
    method_float(0x1A00 + 16 * 3 + 8, 0.125f);
    method_float(0x1A00 + 16 * 3 + 12, 1.0f);
    method(0x17FC, 2);                           /* a line */
    for (i = 0; i < 2; i++)
        for (k = 0; k < 4; k++)
            method_float(0x1518 + 4 * k, (float)(i + k));
    method(0x17FC, 0);
    CHECK(s_got.draws == before + 2 && s_got.b.prim == 2);
    CHECK(s_got.b.index_count == 2 && s_got.b.inline_data != NULL);
    CHECK(s_got.attrs[3].size == 4 && s_got.attrs[3].offset == 48);
    CHECK(near(s_got.attrs[3].current[0], 0.5f));
    /* The title's own arrays are back. */
    draw_arrays(5, 0, 3);
    CHECK(s_got.attrs[0].offset == 0x2000 && s_got.b.inline_data == NULL);
}

static void test_program(void)
{
    uint32_t program_rev, constants_rev;
    unsigned k;

    method(0x1E94, 2);                           /* program */
    method(0x1E9C, 0);                           /* PROGRAM_LOAD */
    for (k = 0; k < 4; k++)
        method(0x0B00 + 4 * k, 0xA0u + k);
    method(0x1EA0, 0);                           /* PROGRAM_START */
    method(0x1EA4, 5);                           /* CONSTANT_LOAD */
    for (k = 0; k < 4; k++)
        method_float(0x0B80 + 4 * k, (float)k + 0.5f);
    draw_arrays(5, 0, 3);
    CHECK(s_got.b.transform == NV2A_TRANSFORM_PROGRAM);
    CHECK(s_got.b.program_start == 0);
    CHECK(s_got.program0[0] == 0xA0 && s_got.program0[3] == 0xA3);
    CHECK(near(s_got.c5[0], 0.5f) && near(s_got.c5[3], 3.5f));
    program_rev = s_got.b.program_revision;
    constants_rev = s_got.b.constants_revision;

    draw_arrays(5, 0, 3);                        /* nothing sent between */
    CHECK(s_got.b.program_revision == program_rev);
    CHECK(s_got.b.constants_revision == constants_rev);
    method(0x1EA4, 5);
    method_float(0x0B80, 9.0f);
    draw_arrays(5, 0, 3);
    CHECK(s_got.b.program_revision == program_rev);
    CHECK(s_got.b.constants_revision != constants_rev);
    CHECK(near(s_got.c5[0], 9.0f));
    method(0x1E9C, 0);
    method(0x0B00, 0xB0);
    draw_arrays(5, 0, 3);
    CHECK(s_got.b.program_revision != program_rev && s_got.program0[0] == 0xB0);
    method(0x1E94, 0);
}

static void test_clear(void)
{
    s_got.clears = 0;
    method(0x1D8C, 0x00FFFF00u);
    method(0x1D90, 0xFF102030u);
    method(0x1D94, 0xF3);
    CHECK(s_got.clears == 1 && s_got.clear.flags == 0xF3);
    CHECK(s_got.clear.argb == 0xFF102030u && s_got.clear.zstencil == 0x00FFFF00u);
    CHECK(!s_got.clear.rect_set);
    CHECK(s_got.clear_target.color_va == COLOR_AT);
    CHECK(s_got.clear_state.zeta_va == ZETA_AT);
    CHECK(s_mem[COLOR_AT] == 0);                 /* not cleared by anyone else */
    method(0x1D98, 2u | (9u << 16));
    method(0x1D9C, 1u | (4u << 16));
    method(0x1D94, 0x01);                        /* depth alone */
    CHECK(s_got.clears == 2 && s_got.clear.flags == 1 && s_got.clear.rect_set);
    CHECK(s_got.clear.rect_h == (2u | (9u << 16)));
    CHECK(s_got.clear.rect_v == (1u | (4u << 16)));
}

extern void nv2a_pb_exec_idle(void);

/* A report is the back end's to write, or the executor's. */
static void test_reports(void)
{
    uint32_t w[4];
    uint64_t first;

    memset(s_mem + 0x3000, 0xFF, 16);
    s_rep.accept = 1;
    nv2a_pb_exec_idle();
    CHECK(s_rep.idles == 0);                     /* none asked for */
    method(0x17C8, 1);
    CHECK(s_rep.clears == 1);
    method(0x17CC, 1);
    method(0x17CC, 0);
    method(0x17D0, 0x01003000u);
    CHECK(s_rep.reports == 1 && s_rep.r.pixels == 0);
    first = s_rep.r.time;
    memcpy(w, s_mem + 0x3000, 16);
    CHECK(w[2] == 0xFFFFFFFFu && w[3] == 0xFFFFFFFFu);
    nv2a_pb_exec_idle();
    nv2a_pb_exec_idle();
    CHECK(s_rep.idles == 1);
    nv2a_batch_report(s_mem + 0x3000, 0x123456789ull, 77);
    memcpy(w, s_mem + 0x3000, 16);
    CHECK(w[0] == 0x23456789u && w[1] == 1 && w[2] == 77 && w[3] == 0);

    s_rep.accept = 0;
    memset(s_mem + 0x3000, 0xFF, 16);
    method(0x17D0, 0x01003000u);
    memcpy(w, s_mem + 0x3000, 16);
    CHECK(s_rep.reports == 2 && s_rep.r.time > first);
    CHECK(w[0] == (uint32_t)s_rep.r.time && w[2] == 0 && w[3] == 0);
    nv2a_pb_exec_idle();
    CHECK(s_rep.idles == 1);

    /* What the executor drew itself is handed over. */
    s_got.accept = 0;
    method(0x17C8, 1);
    method(0x17CC, 1);
    draw_arrays(5, 0, 3);
    method(0x17CC, 0);
    method(0x17D0, 0x01003000u);
    memcpy(w, s_mem + 0x3000, 16);
    CHECK(w[2] > 0 && w[2] == s_rep.r.pixels);
    s_got.accept = 1;
}

/* Pixels of the triangle, at this depth, that the executor's own depth test
 * passes. */
static uint32_t visible(float z)
{
    uint32_t i, w[4];

    for (i = 0; i < 3; i++)
        put_float(0x2000 + 16 * i + 8, z);
    s_got.accept = 0;
    method(0x17C8, 1);
    method(0x17CC, 1);
    draw_arrays(5, 0, 3);
    method(0x17CC, 0);
    method(0x17D0, 0x01003000u);
    memcpy(w, s_mem + 0x3000, 16);
    return w[2];
}

static void clear_depth(uint32_t zeta, uint32_t value, int accept)
{
    s_got.accept = accept;
    method(0x0214, zeta);
    method(0x1D8C, value);
    method(0x1D94, 0x01);
}

/* The executor's own depth, whoever takes the clears and whenever it is
 * filled: what a draw sees is the last clear of its surface. */
static void test_own_depth(void)
{
    uint32_t i, all;

    method(0x030C, 1);                           /* DEPTH_TEST_ENABLE */
    method(0x0354, 0x201);                       /* DEPTH_FUNC: less */
    method(0x035C, 1);                           /* DEPTH_MASK */
    clear_depth(ZETA_AT, 1000, 0);
    all = visible(100.0f);
    CHECK(all > 0 && visible(100.0f) == 0);
    clear_depth(ZETA_AT, 1000, 0);
    CHECK(visible(500.0f) == all && visible(600.0f) == 0);

    clear_depth(ZETA_AT, 50, 1);                 /* by a back end */
    CHECK(visible(100.0f) == 0);
    clear_depth(ZETA_AT, 1000, 1);
    CHECK(visible(100.0f) == all);
    clear_depth(ZETA_AT, 1000, 0);               /* the last one counts */
    clear_depth(ZETA_AT, 50, 0);
    CHECK(visible(100.0f) == 0);
    clear_depth(ZETA_AT, 50, 0);
    method(0x1D8C, 1000);                        /* the value as it was then */
    CHECK(visible(100.0f) == 0);

    clear_depth(ZETA_AT, 50, 0);                 /* each its own */
    clear_depth(ZETA_AT + 0x100, 1000, 0);
    CHECK(visible(100.0f) == all);
    method(0x0214, ZETA_AT);
    CHECK(visible(100.0f) == 0);

    /* Four are kept; one that is new hides nothing. */
    for (i = 2; i < 6; i++)
        clear_depth(ZETA_AT + 0x100 * i, 50, 0);
    method(0x0214, ZETA_AT + 0x600);
    CHECK(visible(100.0f) == all);
    method(0x0214, ZETA_AT + 0x500);
    CHECK(visible(100.0f) == 0);

    method(0x030C, 0);
    method(0x035C, 0);
    method(0x0214, ZETA_AT);
    for (i = 0; i < 3; i++)
        put_float(0x2000 + 16 * i + 8, 0.0f);
    s_got.accept = 1;
}

/* Refused, a batch or a clear goes where it went before. */
static void test_refusal_and_removal(void)
{
    Nv2aBackendFixture f;
    int draws;

    nv2a_backend_fixture_init(&f);
    nv2a_backend_register(nv2a_backend_fixture_bind(&f));
    draw_arrays(5, 0, 3);
    method(0x1D94, 0xF0);
    CHECK(f.draws == 0 && f.clears == 0);

    s_got.accept = 0;
    draws = s_got.draws;
    draw_arrays(5, 0, 3);
    draw_arrays(1, 0, 1);                        /* too few for triangles */
    method(0x1D94, 0xF0);
    CHECK(s_got.draws == draws + 2 && f.draws == 1 && f.clears == 1);

    CHECK(nv2a_backend_register_batches(NULL));
    draw_arrays(5, 0, 3);
    method(0x1D94, 0xF0);
    CHECK(s_got.draws == draws + 2 && f.draws == 2 && f.clears == 2);

    nv2a_backend_register(NULL);
    method(0x1D94, 0xF0);                        /* the executor's own */
    CHECK(s_mem[COLOR_AT] == 0x30 && s_mem[COLOR_AT + 3] == 0xFF);
}

int main(void)
{
    configure();
    test_registration();
    test_fixed_function();
    test_two_sided();
    test_points();
    test_textures_and_combiner();
    test_inline_and_immediate();
    test_program();
    test_clear();
    test_reports();
    test_own_depth();
    test_refusal_and_removal();
    if (!s_fail)
        puts("NV2A batch back end: ok");
    return s_fail;
}
