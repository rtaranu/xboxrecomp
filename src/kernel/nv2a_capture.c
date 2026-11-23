/*
 * RECOMP_FRAME_CAPTURE=<path>: while <path>.go exists at a flip, the frame
 * that follows is written to <path>-<n>.nvcap and <path>.go is removed.
 * RECOMP_FRAME_CAPTURE_FRAMES=<n> captures n frames into one file.
 *
 * A frame starts from state earlier frames left, so the file opens with it:
 * the last value of every method the title has sent, the vertex program and
 * the constants. Guest memory is saved when a batch is about to read it,
 * because a title fills vertex buffers while the frame runs; a range is
 * written again only when its bytes changed.
 *
 * Memory the captured frames themselves draw into is left to the replay. With
 * a host renderer drawing (nv2a_pb_set_draws_off) such memory holds nothing
 * this executor drew, so a target reused from an earlier frame is missing
 * from the capture.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_capture.h"
#include "xbox_memory_layout.h"
#include "../d3d/d3d8_swizzle.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#define REGS (0x2000 / 4)

static const char *s_path;
static int      s_frames;               /* per capture */
static int      s_left;                 /* frames still to capture; 0: idle */
static FILE    *s_file;
static unsigned s_count;

static uint32_t s_val[REGS];
static uint8_t  s_set[REGS];
static float    s_attr_now[16][4];
static int      s_attr_known;

/* DMA offsets seen, open addressing, kept from boot. */
#define RESOLVES 65536u
static struct { uint32_t offset, va; } s_res[RESOLVES];
static uint8_t s_res_used[RESOLVES];

/* Ranges written to the file, with the hash of what was written. */
#define RANGES 65536u
static struct { uint32_t va, bytes; uint64_t hash; } s_range[RANGES];
static uint32_t s_ranges;

/* What the capture has drawn into or cleared so far. */
static struct { uint32_t va, end; } s_target[256];
static uint32_t s_targets;

static uint8_t *s_buf;
static uint32_t s_buf_bytes;

static void put(uint32_t v) { fwrite(&v, 4, 1, s_file); }

static void put_word(uint32_t method, uint32_t param)
{
    put(NV2A_CAPTURE_WORD);
    put(0);
    put(method);
    put(param);
}

static void put_float(uint32_t method, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    put_word(method, v);
}

/* Copy guest memory; pages the runtime has not mapped read as zero. */
static void guest_read(uint32_t va, uint8_t *out, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + va;
#if defined(_WIN32)
    SIZE_T got;
    if (ReadProcessMemory(GetCurrentProcess(), src, out, bytes, &got)
        && got == bytes)
        return;
    while (bytes) {
        uint32_t n = 4096u - ((uint32_t)(uintptr_t)src & 4095u);
        if (n > bytes)
            n = bytes;
        if (!ReadProcessMemory(GetCurrentProcess(), src, out, n, &got) || got != n)
            memset(out, 0, n);
        src += n;
        out += n;
        bytes -= n;
    }
#else
    memcpy(out, src, bytes);
#endif
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull, v;

    for (; n >= 8; p += 8, n -= 8) {
        memcpy(&v, p, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    while (n--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
}

static void save_range(uint32_t va, uint64_t bytes)
{
    uint64_t h;
    uint32_t slot;

    if (!va || !bytes || bytes > 0x4000000u || va + bytes > 0x100000000ull)
        return;
    if (bytes > s_buf_bytes) {
        uint8_t *n = (uint8_t *)realloc(s_buf, (size_t)bytes);
        if (!n)
            return;
        s_buf = n;
        s_buf_bytes = (uint32_t)bytes;
    }
    guest_read(va, s_buf, (uint32_t)bytes);
    h = hash_bytes(s_buf, (uint32_t)bytes);
    for (slot = (va * 2654435761u + (uint32_t)bytes) & (RANGES - 1); ;
         slot = (slot + 1) & (RANGES - 1)) {
        if (!s_range[slot].bytes) {
            if (s_ranges < RANGES / 2) {
                s_ranges++;
                s_range[slot].va = va;
                s_range[slot].bytes = (uint32_t)bytes;
                s_range[slot].hash = h;
            }
            break;
        }
        if (s_range[slot].va == va && s_range[slot].bytes == bytes) {
            if (s_range[slot].hash == h)
                return;
            s_range[slot].hash = h;
            break;
        }
    }
    put(NV2A_CAPTURE_MEM);
    put(va);
    put((uint32_t)bytes);
    fwrite(s_buf, 1, (size_t)bytes, s_file);
}

static void note_target(const Nv2aTarget *t)
{
    uint32_t i, va = t->color_va;
    uint32_t end = va + t->color_pitch * (t->clip_y + t->clip_h);

    if (!va || end <= va)
        return;
    for (i = 0; i < s_targets; i++)
        if (s_target[i].va == va) {
            if (end > s_target[i].end)
                s_target[i].end = end;
            return;
        }
    if (s_targets < sizeof s_target / sizeof s_target[0]) {
        s_target[s_targets].va = va;
        s_target[s_targets++].end = end;
    }
}

static int drawn_into(uint32_t va)
{
    uint32_t i;

    for (i = 0; i < s_targets; i++)
        if (va >= s_target[i].va && va < s_target[i].end)
            return 1;
    return 0;
}

/* An upper bound on what a texture occupies: reading too much only makes the
 * file larger. */
static uint64_t texture_bytes(const Nv2aBatchTexture *t, uint32_t format_word)
{
    uint32_t block = d3d8_format_dxt_block_bytes(t->t.color);
    uint32_t w = t->t.width, h = t->t.height, level;
    uint32_t depth = 1u << ((format_word >> 28) & 0xF);
    uint32_t bpp;
    uint64_t sum = 0;

    if (!block && !d3d8_format_is_swizzled(t->t.color))
        return (uint64_t)t->t.pitch * h;
    if (((format_word >> 4) & 0xF) != 3)           /* not a volume */
        depth = 1;
    switch (t->t.color) {
    case 0x00: case 0x01: case 0x0B: case 0x19: bpp = 1; break;
    case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x1A: case 0x27: case 0x28: case 0x29: bpp = 2; break;
    default: bpp = 4; break;
    }
    for (level = 0; level == 0 || level < t->t.levels; level++) {
        sum += (uint64_t)depth * (block ? (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * block
                                        : (uint64_t)w * h * bpp);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        depth = depth > 1 ? depth / 2 : 1;
    }
    return t->cube ? ((sum + 127) & ~127ull) * 6 : sum;
}

static uint32_t attr_bytes(const Nv2aBatchAttr *a)
{
    switch (a->type) {
    case 0: case 6: return 4;
    case 1: case 5: return 2 * a->size;
    case 4:         return a->size;
    default:        return 4 * a->size;
    }
}

static void on_batch(const Nv2aRawBatch *b)
{
    uint32_t i, lo = 0xFFFFFFFFu, hi = 0;

    for (i = 0; i < 16; i++)
        memcpy(s_attr_now[i], b->attrs[i].current, sizeof s_attr_now[i]);
    s_attr_known = 1;
    if (!s_left)
        return;
    if (!b->inline_data && b->index_count) {
        for (i = 0; i < b->index_count; i++) {
            if (b->indices[i] < lo) lo = b->indices[i];
            if (b->indices[i] > hi) hi = b->indices[i];
        }
        for (i = 0; i < 16; i++) {
            const Nv2aBatchAttr *a = &b->attrs[i];
            if (a->size && a->stride && a->offset)
                save_range(a->offset + lo * a->stride,
                           (uint64_t)(hi - lo) * a->stride + attr_bytes(a));
        }
    }
    for (i = 0; i < 4; i++) {
        const Nv2aBatchTexture *t = &b->textures[i];
        if (!t->valid)
            continue;
        if (!drawn_into(t->t.offset))
            save_range(t->t.offset, texture_bytes(t, b->regs[(0x1B04 + 0x40 * i) / 4]));
        if (t->t.color == 0x0B && t->palette)
            save_range(t->palette, 256 * 4);
    }
    note_target(b->target);
}

static void on_clear(const Nv2aTarget *t)
{
    if (s_left)
        note_target(t);
}

/* A command is written once it has been carried out: the memory its batch
 * reads is saved meanwhile and has to come first in the file. */
static uint32_t s_pending[3];
static int s_is_pending;

static void flush_word(void)
{
    if (s_is_pending) {
        put(NV2A_CAPTURE_WORD);
        fwrite(s_pending, 4, 3, s_file);
    }
    s_is_pending = 0;
}

static void on_word(uint32_t subch, uint32_t method, uint32_t param)
{
    if (subch == 0) {
        s_val[(method & 0x1FFCu) / 4] = param;
        s_set[(method & 0x1FFCu) / 4] = 1;
    }
    if (s_left) {
        flush_word();
        s_pending[0] = subch;
        s_pending[1] = method;
        s_pending[2] = param;
        s_is_pending = 1;
    }
}

static void on_resolved(uint32_t offset, uint32_t va)
{
    uint32_t slot = (offset * 2654435761u) >> 16, tries;

    for (tries = 0; tries < RESOLVES; tries++, slot = (slot + 1) & (RESOLVES - 1)) {
        if (s_res_used[slot] && s_res[slot].offset != offset)
            continue;
        s_res_used[slot] = 1;
        s_res[slot].offset = offset;
        s_res[slot].va = va;
        return;
    }
}

/* Methods that act, or whose last value alone means nothing: not part of the
 * state a frame starts from. */
static int is_action(uint32_t m)
{
    return m < 0x0120                                   /* object, NOP, waits */
        || (m >= 0x012C && m <= 0x0130)                 /* flip */
        || (m >= 0x0B00 && m < 0x0C00)                  /* program, constants */
        || (m >= 0x1500 && m < 0x1540)                  /* immediate vertices */
        || m == 0x17C8 || m == 0x17D0                   /* report */
        || (m >= 0x17FC && m < 0x1820)                  /* begin/end, arrays */
        || (m >= 0x1880 && m < 0x1B00)                  /* vertex data */
        || m == 0x1D70 || m == 0x1D94                   /* semaphore, clear */
        || m == 0x1E9C || m == 0x1EA4;                  /* load pointers */
}

static void save_state(void)
{
    uint32_t start, rev, i, k;
    const uint32_t (*program)[4] = nv2a_vsh_program_memory(&start, &rev);
    const float (*constants)[4] = nv2a_vsh_constants(&rev);
    int pass;

    /* The texture formats go last: for a swizzled texture they set the size,
     * which the image rectangle would overwrite. */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < REGS; i++) {
            uint32_t m = i * 4;
            int format = m >= 0x1B00 && m < 0x1C00 && (m & 0x3F) == 0x04;
            if (s_set[i] && !is_action(m) && format == pass)
                put_word(m, s_val[i]);
        }
    put_word(0x1E9C, 0);
    for (i = 0; i < NV2A_VSH_SLOTS; i++)
        for (k = 0; k < 4; k++)
            put_word(0x0B00 + 4 * k, program[i][k]);
    put_word(0x1EA4, 0);
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
        for (k = 0; k < 4; k++)
            put_float(0x0B80 + 4 * k, constants[i][k]);
    if (s_set[0x1E9C / 4])
        put_word(0x1E9C, s_val[0x1E9C / 4]);
    if (s_set[0x1EA4 / 4])
        put_word(0x1EA4, s_val[0x1EA4 / 4]);
    /* Attribute 0 is the position: writing it would emit a vertex. */
    for (i = 1; s_attr_known && i < 16; i++)
        for (k = 0; k < 4; k++)
            put_float(0x1A00 + 16 * i + 4 * k, s_attr_now[i][k]);
    put(NV2A_CAPTURE_STATE_END);
}

static void finish(void)
{
    uint32_t i;
    long bytes;

    flush_word();
    for (i = 0; i < RESOLVES; i++)
        if (s_res_used[i]) {
            put(NV2A_CAPTURE_RESOLVE);
            put(s_res[i].offset);
            put(s_res[i].va);
        }
    bytes = ftell(s_file);
    fclose(s_file);
    s_file = NULL;
    fprintf(stderr, "[CAPTURE] %s-%u.nvcap: %ld bytes, %u ranges\n",
            s_path, s_count - 1, bytes, s_ranges);
}

static void on_flip(void)
{
    char name[1024];
    FILE *go;

    if (s_left) {
        if (--s_left == 0)
            finish();
        return;
    }
    snprintf(name, sizeof name, "%s.go", s_path);
    go = fopen(name, "rb");
    if (!go)
        return;
    fclose(go);
    remove(name);
    snprintf(name, sizeof name, "%s-%u.nvcap", s_path, s_count);
    s_file = fopen(name, "wb");
    if (!s_file) {
        fprintf(stderr, "[CAPTURE] cannot write %s\n", name);
        return;
    }
    setvbuf(s_file, NULL, _IOFBF, 4u << 20);
    s_count++;
    memset(s_range, 0, sizeof s_range);
    s_ranges = s_targets = 0;
    fwrite(NV2A_CAPTURE_MAGIC, 8, 1, s_file);
    save_state();
    s_left = s_frames;
}

void nv2a_capture_init(void)
{
    static const Nv2aCaptureHooks hooks = {
        on_word, on_resolved, on_batch, on_clear, on_flip
    };
    const char *n = getenv("RECOMP_FRAME_CAPTURE_FRAMES");

    s_path = getenv("RECOMP_FRAME_CAPTURE");
    if (!s_path || !*s_path)
        return;
    s_frames = n && atoi(n) > 0 ? atoi(n) : 1;
    nv2a_pb_exec_set_capture(&hooks);
    fprintf(stderr, "[CAPTURE] on: create %s.go to capture %d frame(s)\n",
            s_path, s_frames);
}
