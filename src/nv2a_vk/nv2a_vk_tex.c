/*
 * Guest textures for the Vulkan rasteriser (nv2a_vk_tex.h).
 *
 * The format codes are the NV097 colour field, the D3DFMT table in
 * ../d3d/d3d8_xbox.h. DXT blocks are stored in rows, as Vulkan's BC1-BC3 want
 * them, so they are copied; everything else is expanded to 8 bits a channel.
 */
#include <math.h>
#include <string.h>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "nv2a_vk_tex.h"
#include "../d3d/d3d8_swizzle.h"

/* How one texel is read; the memory order is a separate question. */
typedef enum {
    T_NONE = 0, T_Y8, T_AY8, T_A8, T_P8,
    T_A1R5G5B5, T_X1R5G5B5, T_A4R4G4B4, T_R5G6B5, T_R6G5B5,
    T_R5G5B5A1, T_R4G4B4A4, T_A8Y8, T_G8B8, T_Y16,
    T_A8R8G8B8, T_X8R8G8B8, T_A8B8G8R8, T_B8G8R8A8, T_R8G8B8A8,
    T_YUY2, T_UYVY, T_Z24, T_Z24F, T_Z16, T_Z16F, T_DXT1, T_DXT3, T_DXT5
} Texel;

typedef enum { M_SWIZZLED, M_LINEAR, M_BLOCKS } Order;

static Texel texel_of(uint32_t color, Order *order)
{
    *order = M_SWIZZLED;
    switch (color) {
    case 0x00: return T_Y8;
    case 0x01: return T_AY8;
    case 0x02: return T_A1R5G5B5;
    case 0x03: return T_X1R5G5B5;
    case 0x04: return T_A4R4G4B4;
    case 0x05: return T_R5G6B5;
    case 0x06: return T_A8R8G8B8;
    case 0x07: return T_X8R8G8B8;
    case 0x0B: return T_P8;
    case 0x19: return T_A8;
    case 0x1A: return T_A8Y8;
    case 0x27: return T_R6G5B5;
    case 0x28: return T_G8B8;
    case 0x2A: return T_Z24;
    case 0x2B: return T_Z24F;
    case 0x2C: return T_Z16;
    case 0x2D: return T_Z16F;
    case 0x32: return T_Y16;
    case 0x38: return T_R5G5B5A1;
    case 0x39: return T_R4G4B4A4;
    case 0x3A: return T_A8B8G8R8;
    case 0x3B: return T_B8G8R8A8;
    case 0x3C: return T_R8G8B8A8;
    default: break;
    }
    *order = M_BLOCKS;
    switch (color) {
    case 0x0C: return T_DXT1;
    case 0x0E: return T_DXT3;
    case 0x0F: return T_DXT5;
    default: break;
    }
    *order = M_LINEAR;
    switch (color) {
    case 0x10: return T_A1R5G5B5;
    case 0x11: return T_R5G6B5;
    case 0x12: return T_A8R8G8B8;
    case 0x13: return T_Y8;
    case 0x17: return T_G8B8;
    case 0x1B: return T_AY8;
    case 0x1C: return T_X1R5G5B5;
    case 0x1D: return T_A4R4G4B4;
    case 0x1E: return T_X8R8G8B8;
    case 0x1F: return T_A8;
    case 0x20: return T_A8Y8;
    case 0x24: return T_YUY2;
    case 0x25: return T_UYVY;
    case 0x2E: return T_Z24;
    case 0x2F: return T_Z24F;
    case 0x30: return T_Z16;
    case 0x31: return T_Z16F;
    case 0x35: return T_Y16;
    case 0x37: return T_R6G5B5;
    case 0x3D: return T_R5G5B5A1;
    case 0x3E: return T_R4G4B4A4;
    case 0x3F: return T_A8B8G8R8;
    case 0x40: return T_B8G8R8A8;
    case 0x41: return T_R8G8B8A8;
    default: return T_NONE;
    }
}

static uint32_t texel_bytes(Texel t)
{
    switch (t) {
    case T_Y8: case T_AY8: case T_A8: case T_P8:
        return 1;
    case T_A8R8G8B8: case T_X8R8G8B8: case T_A8B8G8R8: case T_B8G8R8A8:
    case T_R8G8B8A8: case T_Z24: case T_Z24F:
        return 4;
    default:
        return 2;
    }
}

int nv2a_vk_tex_depth(uint32_t color, Nv2aVkDepth *out)
{
    Order order;
    Texel t = texel_of(color, &order);

    out->bytes = t == T_Z24 || t == T_Z24F ? 4 : 2;
    out->is_float = t == T_Z24F || t == T_Z16F;
    return t == T_Z24 || t == T_Z24F || t == T_Z16 || t == T_Z16F;
}

double nv2a_vk_depth_most(const Nv2aVkDepth *d)
{
    if (d->bytes == 2)
        return d->is_float ? 511.9375 : 65535.0;
    return d->is_float ? 3.4027977134981e38 : 16777215.0;
}

float nv2a_vk_depth_read(const Nv2aVkDepth *d, uint32_t v)
{
    float f;

    if (d->bytes == 2) {
        v &= 0xFFFF;
        if (!d->is_float)
            return (float)v / 65535.0f;
        return v ? ldexpf((float)(0x1000 | (v & 0xFFF)), (int)(v >> 12) - 7 - 12) / 511.9375f
                 : 0.0f;
    }
    if (!d->is_float)
        return (float)(v >> 8) / 16777215.0f;
    v = (v >> 8) << 7;                              /* a float without its sign */
    if (v >= 0x7F800000u)
        return 1.0f;
    memcpy(&f, &v, 4);
    return (float)((double)f / nv2a_vk_depth_most(d));
}

static uint32_t at_level(uint32_t size, uint32_t level)
{
    return (size >> level) ? (size >> level) : 1;
}

static uint32_t level_host_bytes(const Nv2aVkTexInfo *i, uint32_t level)
{
    uint32_t w = at_level(i->width, level), h = at_level(i->height, level);
    uint32_t d = at_level(i->depth, level);

    switch (i->format) {
    case NV2A_VK_BC1:     return ((w + 3) / 4) * ((h + 3) / 4) * d * 8;
    case NV2A_VK_BC2:
    case NV2A_VK_BC3:     return ((w + 3) / 4) * ((h + 3) / 4) * d * 16;
    case NV2A_VK_RGBA16S: return w * h * d * 8;
    default:              return w * h * d * 4;
    }
}

/* With border texels (bit 3 of SET_TEXTURE_FORMAT clear) there are four around
 * a level's own, in twice the size and at least 16: nxdk_pgraph_tests'
 * Texture_border results, taken on the hardware, for level 0. */
static uint32_t stored(const Nv2aVkTexInfo *i, uint32_t size)
{
    return !i->bordered ? size : size < 8 ? 16 : size * 2;
}

static uint32_t level_guest_bytes(Texel t, Order order, const Nv2aVkTexInfo *i,
                                  uint32_t level)
{
    uint32_t two = 2 * (uint32_t)i->border;

    if (order == M_BLOCKS)
        return level_host_bytes(i, level);
    return stored(i, at_level(i->width - two, level))
         * stored(i, at_level(i->height - two, level))
         * (i->depth > 1 ? stored(i, at_level(i->depth, level)) : 1) * texel_bytes(t);
}

static uint32_t face_host_bytes(const Nv2aVkTexInfo *i)
{
    uint32_t level, total = 0;

    for (level = 0; level < i->levels; level++)
        total += level_host_bytes(i, level);
    return total;
}

/* SET_TEXTURE_FILTER: A, R, G and B read as signed in bits 28-31. Only for
 * the texels whose channels are whole bytes: for the others a signed channel
 * would be narrower than the byte it was expanded to. */
static uint32_t signed_channels(Texel t, uint32_t filter)
{
    switch (t) {
    case T_A8R8G8B8: case T_X8R8G8B8: case T_A8B8G8R8: case T_B8G8R8A8:
    case T_R8G8B8A8: case T_G8B8: case T_A8Y8: case T_Y8: case T_AY8: case T_A8:
        return filter >> 28;
    default:
        return 0;
    }
}

int nv2a_vk_tex_info(const Nv2aBatchTexture *t, uint32_t format_word,
                     uint32_t filter, Nv2aVkTexInfo *out)
{
    Order order;
    Texel texel = texel_of(t->t.color, &order);
    Nv2aVkDepth depth;
    uint32_t level, most, total = 0;

    memset(out, 0, sizeof *out);
    if (texel == T_NONE || !t->valid || !t->t.width || !t->t.height
        || t->t.width > 4096 || t->t.height > 4096)
        return 0;
    out->width  = t->t.width;
    out->height = t->t.height;
    out->depth  = 1;
    out->faces  = t->cube ? 6 : 1;
    out->linear = order == M_LINEAR;
    out->format = order != M_BLOCKS
                ? (signed_channels(texel, filter) ? NV2A_VK_RGBA16S : NV2A_VK_BGRA8)
                : texel == T_DXT1 ? NV2A_VK_BC1
                : texel == T_DXT3 ? NV2A_VK_BC2 : NV2A_VK_BC3;
    if (nv2a_vk_tex_depth(t->t.color, &depth))
        out->format = NV2A_VK_DEPTH;
    if (((format_word >> 4) & 0xF) == 3 && order != M_LINEAR) {
        out->depth = 1u << (format_word >> 28);
        if (out->depth > 512 || t->cube)
            return 0;
    }
    out->bordered = order == M_SWIZZLED && !(format_word & 8);
    if (order == M_LINEAR) {
        out->levels = 1;
        if (t->t.pitch < out->width * texel_bytes(texel))
            return 0;
        out->face_stride = (t->t.pitch * out->height + 127u) & ~127u;
        out->guest_bytes = out->face_stride * (out->faces - 1)
                         + t->t.pitch * out->height;
        out->host_bytes = face_host_bytes(out) * out->faces;
        return 1;
    }
    /* No more levels than the sizes have: a title may ask for the maximum. */
    most = 1;
    while ((out->width >> most) || (out->height >> most) || (out->depth >> most))
        most++;
    out->levels = t->t.levels ? t->t.levels : 1;
    if (out->levels > most)
        out->levels = most;
    if (out->bordered && !t->cube && out->depth == 1 && t->t.addr_u == 4 && t->t.addr_v == 4
        && out->width <= 2048 && out->height <= 2048) {
        out->border = 4;
        out->levels = 1;
        out->width += 8;
        out->height += 8;
    }
    for (level = 0; level < out->levels; level++)
        total += level_guest_bytes(texel, order, out, level);
    /* Faces start on 128 bytes, as in nv2a_pb_exec.c's tex_face_stride. */
    out->face_stride = (total + 127u) & ~127u;
    out->guest_bytes = out->face_stride * (out->faces - 1) + total;
    out->host_bytes = face_host_bytes(out) * out->faces;
    return 1;
}

uint32_t nv2a_vk_tex_level_offset(const Nv2aVkTexInfo *i, uint32_t face,
                                  uint32_t level)
{
    uint32_t k, at = face * face_host_bytes(i);

    for (k = 0; k < level; k++)
        at += level_host_bytes(i, k);
    return at;
}

uint32_t nv2a_vk_tex_guest_offset(const Nv2aVkTexInfo *i, uint32_t color,
                                  uint32_t face, uint32_t level)
{
    Order order;
    Texel t = texel_of(color, &order);
    uint32_t k, at = face * i->face_stride;

    for (k = 0; k < level; k++)
        at += level_guest_bytes(t, order, i, k);
    return at;
}

int nv2a_vk_tex_native(uint32_t color, uint32_t filter, Nv2aVkTexNative *out)
{
    Order order;
    Texel t = texel_of(color, &order);
    const char *sw;

    if (order == M_BLOCKS || signed_channels(t, filter))
        return 0;
    out->bytes = texel_bytes(t);
    out->swizzled = order == M_SWIZZLED;
    switch (t) {
    case T_A8R8G8B8: out->view = NV2A_VK_VIEW_8888; sw = "rgba"; break;
    case T_X8R8G8B8: out->view = NV2A_VK_VIEW_8888; sw = "rgb1"; break;
    case T_A8B8G8R8: out->view = NV2A_VK_VIEW_8888; sw = "bgra"; break;
    case T_B8G8R8A8: out->view = NV2A_VK_VIEW_8888; sw = "grab"; break;
    case T_R8G8B8A8: out->view = NV2A_VK_VIEW_8888; sw = "argb"; break;
    case T_R5G6B5:   out->view = NV2A_VK_VIEW_565;  sw = "rgb1"; break;
    case T_A1R5G5B5: out->view = NV2A_VK_VIEW_1555; sw = "rgba"; break;
    case T_X1R5G5B5: out->view = NV2A_VK_VIEW_1555; sw = "rgb1"; break;
    case T_A4R4G4B4: out->view = NV2A_VK_VIEW_4444; sw = "grab"; break;
    case T_R4G4B4A4: out->view = NV2A_VK_VIEW_4444; sw = "bgra"; break;
    case T_R5G5B5A1: out->view = NV2A_VK_VIEW_5551; sw = "rgba"; break;
    case T_A8Y8:     out->view = NV2A_VK_VIEW_88;   sw = "rrrg"; break;
    case T_G8B8:     out->view = NV2A_VK_VIEW_88;   sw = "rgrg"; break;
    case T_Y8:       out->view = NV2A_VK_VIEW_8;    sw = "rrr1"; break;
    case T_AY8:      out->view = NV2A_VK_VIEW_8;    sw = "rrrr"; break;
    case T_A8:       out->view = NV2A_VK_VIEW_8;    sw = "111r"; break;
    default:         return 0;
    }
    memcpy(out->swizzle, sw, 4);
    return 1;
}

void nv2a_vk_tex_raw(const uint8_t *guest, uint32_t bytes, int swizzled,
                     uint32_t w, uint32_t h, uint32_t pitch, uint8_t *out)
{
    uint32_t x, y, mx, my, ox, oy = 0;

    xbox_swizzle_masks(w, h, &mx, &my);
    for (y = 0; y < h; y++, out += (size_t)w * bytes) {
        if (!swizzled) {
            memcpy(out, guest + (size_t)y * pitch, (size_t)w * bytes);
            continue;
        }
#define ROW(n) for (x = 0, ox = 0; x < w; x++) { \
            memcpy(out + (size_t)x * (n), guest + (size_t)(ox | oy) * (n), n); \
            ox = (ox - mx) & mx; \
        }
        if (bytes == 4)
            ROW(4)
        else if (bytes == 2)
            ROW(2)
        else
            ROW(1)
#undef ROW
        oy = (oy - my) & my;
    }
}

static uint32_t clamp8(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v; }

/* BT.601, the coefficients the CPU rasteriser converts with. */
static uint32_t yuv(int y, int cb, int cr)
{
    int c = y - 16, u = cb - 128, v = cr - 128;

    return 0xFF000000u
         | (clamp8((298 * c + 409 * v + 128) >> 8) << 16)
         | (clamp8((298 * c - 100 * u - 208 * v + 128) >> 8) << 8)
         |  clamp8((298 * c + 516 * u + 128) >> 8);
}

#define X(v, sh, bits) d3d8_expand_channel(((v) >> (sh)) & ((1u << (bits)) - 1u), bits)

/* One row of raw texels to 0xAARRGGBB. */
static void convert(Texel t, const uint32_t *raw, const uint32_t *palette,
                    uint32_t *out, uint32_t n)
{
    uint32_t i, v;

#define EACH(expr) for (i = 0; i < n; i++) { v = raw[i]; out[i] = (expr); } break
    switch (t) {
    case T_Y8:       EACH(0xFF000000u | (v * 0x010101u));
    case T_AY8:      EACH(v * 0x01010101u);
    case T_A8:       EACH((v << 24) | 0x00FFFFFFu);
    case T_P8:       EACH(palette ? palette[v] : 0xFFFFFFFFu);
    case T_A1R5G5B5: EACH(((v & 0x8000u) ? 0xFF000000u : 0u)
                          | (X(v, 10, 5) << 16) | (X(v, 5, 5) << 8) | X(v, 0, 5));
    case T_X1R5G5B5: EACH(0xFF000000u
                          | (X(v, 10, 5) << 16) | (X(v, 5, 5) << 8) | X(v, 0, 5));
    case T_A4R4G4B4: EACH((X(v, 12, 4) << 24) | (X(v, 8, 4) << 16)
                          | (X(v, 4, 4) << 8) | X(v, 0, 4));
    case T_R5G6B5:   EACH(0xFF000000u
                          | (X(v, 11, 5) << 16) | (X(v, 5, 6) << 8) | X(v, 0, 5));
    case T_R6G5B5:   EACH(0xFF000000u
                          | (X(v, 10, 6) << 16) | (X(v, 5, 5) << 8) | X(v, 0, 5));
    case T_R5G5B5A1: EACH(((v & 1u) ? 0xFF000000u : 0u)
                          | (X(v, 11, 5) << 16) | (X(v, 6, 5) << 8) | X(v, 1, 5));
    case T_R4G4B4A4: EACH((X(v, 0, 4) << 24) | (X(v, 12, 4) << 16)
                          | (X(v, 8, 4) << 8) | X(v, 4, 4));
    case T_A8Y8:     EACH(((v >> 8) << 24) | ((v & 0xFF) * 0x010101u));
    /* Two channels: G and B where the name says, and R and A repeat B and G
     * (nxdk_pgraph_tests' results, taken on the hardware). */
    case T_G8B8:     EACH(((v >> 8) << 24) | ((v & 0xFF) << 16) | v);
    case T_Y16:      EACH(0xFF000000u | ((v >> 8) * 0x010101u));
    case T_A8R8G8B8: EACH(v);
    case T_X8R8G8B8: EACH(v | 0xFF000000u);
    case T_A8B8G8R8: EACH((v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF));
    case T_B8G8R8A8: EACH(((v & 0xFFu) << 24) | (((v >> 8) & 0xFFu) << 16)
                          | (((v >> 16) & 0xFFu) << 8) | (v >> 24));
    case T_R8G8B8A8: EACH(((v & 0xFFu) << 24) | (v >> 8));
    /* Two texels share a pair of chroma bytes; raw[] holds 16 bits of each. */
    case T_YUY2: case T_UYVY:
        for (i = 0; i < n; i++) {
            uint32_t a = raw[i & ~1u], b = raw[(i | 1u) < n ? (i | 1u) : i];
            unsigned ys = t == T_YUY2 ? 0 : 8, cs = 8 - ys;
            out[i] = yuv((int)((raw[i] >> ys) & 0xFF), (int)((a >> cs) & 0xFF),
                         (int)((b >> cs) & 0xFF));
        }
        break;
    default:
        for (i = 0; i < n; i++)
            out[i] = 0xFFFFFFFFu;
        break;
    }
#undef EACH
}

static void load(const uint8_t *p, uint32_t bytes, uint32_t *raw)
{
    uint16_t v16;

    switch (bytes) {
    case 1:  *raw = *p; break;
    case 2:  memcpy(&v16, p, 2); *raw = v16; break;
    default: memcpy(raw, p, 4); break;
    }
}

/* 8 bits a channel to s16, the channels in `mask` (A R G B from bit 0) read
 * as signed bytes over 128, the others as unsigned over 255. */
static void put_signed(const uint32_t *argb, uint32_t n, uint32_t mask, uint8_t *out)
{
    uint32_t i, c;

    for (i = 0; i < n; i++) {
        uint32_t ch[4];
        int16_t v[4];

        ch[0] = (argb[i] >> 16) & 0xFF;             /* r g b a */
        ch[1] = (argb[i] >> 8) & 0xFF;
        ch[2] = argb[i] & 0xFF;
        ch[3] = argb[i] >> 24;
        for (c = 0; c < 4; c++) {
            uint32_t bit = c == 3 ? 1u : 2u << c;
            v[c] = mask & bit ? (int16_t)((int8_t)ch[c] * 256)
                              : (int16_t)(ch[c] * 32767u / 255u);
        }
        memcpy(out + (size_t)i * 8, v, 8);
    }
}

static void decode_level(const uint8_t *guest, const uint32_t *palette, Texel t,
                         Order order, uint32_t pitch, uint32_t mask,
                         const Nv2aVkTexInfo *i, uint32_t level, uint8_t *out)
{
    uint32_t w = at_level(i->width, level), h = at_level(i->height, level);
    uint32_t d = at_level(i->depth, level), bytes = texel_bytes(t);
    uint32_t raw[4096], argb[4096], x, y, z, mx, my, ox, oy = 0, fromx = 0;
    uint32_t two = 2 * (uint32_t)i->border, from = i->bordered && !i->border ? 4 : 0;
    uint32_t sw = stored(i, w - two), sh = stored(i, h - two);
    uint32_t sd = i->depth > 1 ? stored(i, d) : 1;

    if (order == M_BLOCKS) {
        /* A volume's blocks follow their neighbours in depth, up to four of
         * them, before the next in the row (NV_texture_compression_vtc). */
        uint32_t row = (w + 3) / 4 * ((h + 3) / 4), n = i->format == NV2A_VK_BC1 ? 8 : 16;

        for (z = 0; z < d; z++)
            for (x = 0; x < row; x++, out += n) {
                uint32_t first = z & ~3u, deep = d - first < 4 ? d - first : 4;

                memcpy(out, guest + (size_t)(first * row + x * deep + (z - first)) * n, n);
            }
        return;
    }
    xbox_swizzle_masks(sw, sh, &mx, &my);
    for (y = 0; y < from; y++)
        oy = (oy - my) & my;
    for (x = 0; x < from; x++)
        fromx = (fromx - mx) & mx;
    for (z = 0; z < d; z++)
        for (y = 0; y < h; y++) {
            if (order == M_LINEAR) {
                const uint8_t *row = guest + (size_t)y * pitch;
                for (x = 0; x < w; x++)
                    load(row + (size_t)x * bytes, bytes, &raw[x]);
            } else if (sd > 1) {
                for (x = 0; x < w; x++)
                    load(guest + (size_t)xbox_swizzle_offset_3d(x + from, y + from, z + from,
                                                                sw, sh, sd) * bytes,
                         bytes, &raw[x]);
            } else {
                for (x = 0, ox = fromx; x < w; x++) {
                    load(guest + (size_t)(ox | oy) * bytes, bytes, &raw[x]);
                    ox = (ox - mx) & mx;
                }
                oy = (oy - my) & my;
            }
            if (i->format == NV2A_VK_DEPTH) {
                Nv2aVkDepth depth = { bytes, t == T_Z24F || t == T_Z16F };

                for (x = 0; x < w; x++) {
                    float f = nv2a_vk_depth_read(&depth, raw[x]);

                    memcpy(out + (size_t)x * 4, &f, 4);
                }
                out += (size_t)w * 4;
                continue;
            }
            convert(t, raw, palette, argb, w);
            if (i->format == NV2A_VK_RGBA16S) {
                put_signed(argb, w, mask, out);
                out += (size_t)w * 8;
            } else {
                memcpy(out, argb, (size_t)w * 4);
                out += (size_t)w * 4;
            }
        }
}

void nv2a_vk_tex_decode(const uint8_t *guest, const uint32_t *palette,
                        uint32_t color, uint32_t pitch, uint32_t filter,
                        const Nv2aVkTexInfo *i, uint8_t *out)
{
    Order order;
    Texel t = texel_of(color, &order);
    uint32_t mask = signed_channels(t, filter), face, level;

    for (face = 0; face < i->faces; face++) {
        const uint8_t *p = guest + (size_t)face * i->face_stride;
        for (level = 0; level < i->levels; level++) {
            decode_level(p, palette, t, order, pitch, mask, i, level,
                         out + nv2a_vk_tex_level_offset(i, face, level));
            p += level_guest_bytes(t, order, i, level);
        }
    }
}

/* Eight lanes of 16 bytes. In a 64-bit half, with k odd: low half * k plus the
 * two halves swapped, mixed with its upper bits. */
#if defined(_M_X64) || defined(__SSE2__)
typedef __m128i Lane;
#define LANE_SET(hi, lo)  _mm_set_epi64x((int64_t)(hi), (int64_t)(lo))
#define LANE_GET(v, l)    _mm_storeu_si128((__m128i *)(void *)(v), l)

static Lane lane_mix(Lane l, const uint8_t *b, Lane k)
{
    __m128i x = _mm_xor_si128(l, _mm_loadu_si128((const __m128i *)(const void *)b));
    __m128i m = _mm_mul_epu32(x, k);

    return _mm_xor_si128(_mm_add_epi64(m, _mm_shuffle_epi32(x, 0xB1)), _mm_srli_epi64(m, 29));
}
#else
typedef struct { uint64_t v[2]; } Lane;

static Lane LANE_SET(uint64_t hi, uint64_t lo) { Lane l = { { lo, hi } }; return l; }
#define LANE_GET(to, l)   memcpy(to, (l).v, 16)

static Lane lane_mix(Lane l, const uint8_t *b, Lane k)
{
    uint64_t v[2], x, m;
    int i;

    memcpy(v, b, 16);
    for (i = 0; i < 2; i++) {
        x = l.v[i] ^ v[i];
        m = (x & 0xFFFFFFFFu) * (k.v[i] & 0xFFFFFFFFu);
        l.v[i] = (m + (x >> 32 | x << 32)) ^ (m >> 29);
    }
    return l;
}
#endif

uint64_t nv2a_vk_hash(const void *p, uint32_t bytes)
{
    const uint8_t *b = (const uint8_t *)p;
    const Lane k = LANE_SET(0x1E98DF25u, 0x1E98DF25u);
    Lane l[8];
    uint8_t rest[128];
    uint64_t out = bytes, v[2];
    uint32_t n, i;

    for (i = 0; i < 8; i++)
        l[i] = LANE_SET(0x9E3779B97F4A7C15ull * (2 * i + 1), 0xC2B2AE3D27D4EB4Full * (2 * i + 3));
    for (n = bytes; n; b += 128) {
        if (n < 128) {
            memset(rest, 0, sizeof rest);
            memcpy(rest, b, n);
            b = rest;
        }
        for (i = 0; i < 8; i++)
            l[i] = lane_mix(l[i], b + 16 * i, k);
        n -= n < 128 ? n : 128;
    }
    for (i = 0; i < 8; i++) {
        LANE_GET(v, l[i]);
        out = (out ^ v[0]) * 0x9FB21C651E98DF25ull;
        out = (out ^ out >> 32 ^ v[1]) * 0x9FB21C651E98DF25ull;
        out ^= out >> 32;
    }
    return out;
}
