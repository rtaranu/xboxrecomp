/*
 * Texture decode for the Vulkan rasteriser (nv2a_vk_tex.h): sizes, levels,
 * faces and texels, against values worked out by hand.
 */
#include <stdio.h>
#include <string.h>

#include "nv2a_vk_tex.h"
#include "../d3d/d3d8_swizzle.h"

static int s_fail;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; \
} } while (0)

static uint8_t  s_guest[0x10000];
static uint32_t s_out[0x4000];
static Nv2aVkTexInfo s_info;

static Nv2aBatchTexture texture(uint32_t color, uint32_t w, uint32_t h,
                                uint32_t levels, uint32_t pitch)
{
    Nv2aBatchTexture t;

    memset(&t, 0, sizeof t);
    t.valid = 1;
    t.t.color = color;
    t.t.width = w;
    t.t.height = h;
    t.t.levels = levels;
    t.t.pitch = pitch;
    return t;
}

static int decode(const Nv2aBatchTexture *t, uint32_t format_word, uint32_t filter,
                  const uint32_t *palette)
{
    memset(s_out, 0xCD, sizeof s_out);
    if (!nv2a_vk_tex_info(t, format_word, filter, &s_info)
        || s_info.host_bytes > sizeof s_out)
        return 0;
    nv2a_vk_tex_decode(s_guest, palette, t->t.color, t->t.pitch, filter, &s_info,
                       (uint8_t *)s_out);
    return 1;
}

/* One texel of a 1x1 texture of the format. */
static uint32_t one(uint32_t color, uint32_t raw)
{
    Nv2aBatchTexture t = texture(color, 1, 1, 1, 64);

    memcpy(s_guest, &raw, 4);
    return decode(&t, 0x28, 0, NULL) ? s_out[0] : 0xBAD0BAD0u;
}

static void test_texels(void)
{
    static const struct { uint32_t color, raw, argb; } k[] = {
        { 0x00, 0x80,       0xFF808080u },      /* Y8 */
        { 0x01, 0x80,       0x80808080u },      /* AY8 */
        { 0x19, 0x80,       0x80FFFFFFu },      /* A8 */
        { 0x02, 0xFC00,     0xFFFF0000u },      /* A1R5G5B5 */
        { 0x02, 0x7C00,     0x00FF0000u },
        { 0x03, 0x03E0,     0xFF00FF00u },      /* X1R5G5B5 */
        { 0x04, 0x8F00,     0x88FF0000u },      /* A4R4G4B4 */
        { 0x05, 0x07E0,     0xFF00FF00u },      /* R5G6B5 */
        { 0x06, 0x12345678, 0x12345678u },      /* A8R8G8B8 */
        { 0x07, 0x12345678, 0xFF345678u },      /* X8R8G8B8 */
        { 0x1A, 0x4080,     0x40808080u },      /* A8Y8 */
        { 0x3A, 0x12345678, 0x12785634u },      /* A8B8G8R8 */
        { 0x3B, 0x12345678, 0x78563412u },      /* B8G8R8A8 */
        { 0x3C, 0x12345678, 0x78123456u },      /* R8G8B8A8 */
        { 0x10, 0xFC00,     0xFFFF0000u },      /* the linear forms */
        { 0x11, 0x001F,     0xFF0000FFu },
        { 0x12, 0x12345678, 0x12345678u },
        { 0x13, 0x80,       0xFF808080u },
        { 0x1E, 0x12345678, 0xFF345678u },
        { 0x3F, 0x12345678, 0x12785634u },
    };
    unsigned i;

    for (i = 0; i < sizeof k / sizeof *k; i++) {
        uint32_t got = one(k[i].color, k[i].raw);
        if (got != k[i].argb) {
            fprintf(stderr, "FAIL format %02X: %08X gave %08X, not %08X\n",
                    k[i].color, k[i].raw, got, k[i].argb);
            s_fail = 1;
        }
    }
    CHECK(one(0x08, 0) == 0xBAD0BAD0u);          /* no such texel */
}

static void test_palette(void)
{
    Nv2aBatchTexture t = texture(0x0B, 2, 1, 1, 0);
    uint32_t pal[256] = { 0 };

    pal[3] = 0x11223344u;
    pal[255] = 0xAABBCCDDu;
    s_guest[0] = 3;
    s_guest[1] = 255;
    CHECK(decode(&t, 0x28, 0, pal) && s_out[0] == 0x11223344u && s_out[1] == 0xAABBCCDDu);
}

/* Bits of x and y alternate, x first, until the smaller side's run out. */
static void test_swizzle(void)
{
    Nv2aBatchTexture t = texture(0x06, 8, 2, 1, 0);
    uint32_t i, x, y;

    for (i = 0; i < 16; i++)
        memcpy(s_guest + 4 * i, &i, 4);
    CHECK(decode(&t, 0x28, 0, NULL));
    for (y = 0; y < 2; y++)
        for (x = 0; x < 8; x++)
            CHECK(s_out[y * 8 + x] == ((x & 1) | y << 1 | (x & 6) << 1));
}

static void test_linear_pitch(void)
{
    Nv2aBatchTexture t = texture(0x12, 2, 3, 5, 64);    /* levels do not apply */
    uint32_t v;

    v = 0x01020304u; memcpy(s_guest + 64 * 2 + 4, &v, 4);
    CHECK(decode(&t, 0x28, 0, NULL));
    CHECK(s_info.linear && s_info.levels == 1 && s_info.guest_bytes == 192);
    CHECK(s_out[2 * 2 + 1] == 0x01020304u);
    t.t.pitch = 4;                               /* shorter than a row */
    CHECK(!nv2a_vk_tex_info(&t, 0x28, 0, &s_info));
}

static void test_levels_and_faces(void)
{
    Nv2aBatchTexture t = texture(0x06, 4, 4, 9, 0);
    uint32_t i, face_dwords = 16 + 4 + 1;

    for (i = 0; i < sizeof s_guest / 4; i++)
        memcpy(s_guest + 4 * i, &i, 4);
    t.cube = 1;
    CHECK(decode(&t, 0x28, 0, NULL));
    CHECK(s_info.levels == 3 && s_info.faces == 6);      /* 4, 2, 1 */
    CHECK(s_info.face_stride == 128);                    /* 84 bytes, rounded */
    CHECK(s_info.host_bytes == 6 * face_dwords * 4);
    CHECK(nv2a_vk_tex_level_offset(&s_info, 0, 1) == 64);
    CHECK(nv2a_vk_tex_level_offset(&s_info, 2, 2) == (2 * face_dwords + 20) * 4);
    CHECK(s_out[0] == 0 && s_out[16] == 16 && s_out[20] == 20);
    CHECK(s_out[face_dwords] == 32);                     /* face 1's first */
    CHECK(s_out[5 * face_dwords + 20] == 5 * 32 + 20);   /* face 5's last */
}

static void test_volume(void)
{
    Nv2aBatchTexture t = texture(0x00, 2, 2, 1, 0);
    uint32_t i;

    for (i = 0; i < 8; i++)
        s_guest[i] = (uint8_t)(i * 10);
    CHECK(decode(&t, 0x38u | 1u << 28, 0, NULL));        /* three dimensions, 2 deep */
    CHECK(s_info.depth == 2 && s_info.guest_bytes == 8);
    CHECK((s_out[1] & 0xFF) == 10 && (s_out[2] & 0xFF) == 20 && (s_out[4] & 0xFF) == 40);
}

/* Two blocks by one, eight deep: a level's fours, then 4 by 4 by 4 and 2 by 2 by
 * 2 in one block to a slice. */
static void test_volume_blocks(void)
{
    static const uint8_t order[16] = { 0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15 };
    Nv2aBatchTexture t = texture(0x0C, 8, 4, 3, 0);
    uint8_t *out = (uint8_t *)s_out;
    uint32_t i;

    for (i = 0; i < 22 * 8; i++)
        s_guest[i] = (uint8_t)(i / 8);
    CHECK(decode(&t, 0x38u | 3u << 28, 0, NULL));
    CHECK(s_info.format == NV2A_VK_BC1 && s_info.depth == 8 && s_info.guest_bytes == 22 * 8
          && s_info.host_bytes == 22 * 8);
    for (i = 0; i < 16; i++)
        CHECK(out[8 * i] == order[i] && out[8 * i + 7] == order[i]);
    for (; i < 22; i++)
        CHECK(out[8 * i] == i);
    t.t.color = 0x0E;
    t.t.levels = 1;
    for (i = 0; i < 16 * 16; i++)
        s_guest[i] = (uint8_t)(i / 16);
    CHECK(decode(&t, 0x38u | 3u << 28, 0, NULL) && s_info.format == NV2A_VK_BC2);
    for (i = 0; i < 16; i++)
        CHECK(out[16 * i] == order[i]);
}

/* Four border texels around each level's own, in twice the size and at least
 * 16. */
static void test_bordered(void)
{
    Nv2aBatchTexture t = texture(0x00, 4, 8, 4, 0);
    uint32_t x, y, mx, my, i, at;

    memset(s_guest, 0, sizeof s_guest);
    xbox_swizzle_masks(16, 16, &mx, &my);
    for (y = 0; y < 16; y++)                             /* level 0 is 16 x 16 */
        for (x = 0; x < 16; x++)
            s_guest[swizzle_deposit(x, mx) | swizzle_deposit(y, my)] = (uint8_t)(x << 4 | y);
    s_guest[256 + (swizzle_deposit(5, mx) | swizzle_deposit(7, my))] = 0x99;    /* and 1 */
    CHECK(decode(&t, 0x20, 0, NULL));
    CHECK(s_info.width == 4 && s_info.height == 8 && s_info.levels == 4 && !s_info.border);
    CHECK(s_info.guest_bytes == 4 * 256);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 4; x++)
            CHECK((s_out[y * 4 + x] & 0xFF) == ((x + 4) << 4 | (y + 4)));
    CHECK((s_out[32 + 3 * 2 + 1] & 0xFF) == 0x99);

    t.t.addr_u = t.t.addr_v = 4;                         /* the border is read */
    CHECK(decode(&t, 0x20, 0, NULL));
    CHECK(s_info.width == 12 && s_info.height == 16 && s_info.levels == 1 && s_info.border == 4);
    CHECK(s_info.guest_bytes == 256);
    for (y = 0; y < 16; y++)
        for (x = 0; x < 12; x++)
            CHECK((s_out[y * 12 + x] & 0xFF) == (x << 4 | y));
    t.t.addr_v = 1;
    CHECK(decode(&t, 0x20, 0, NULL) && !s_info.border && s_info.bordered);

    t = texture(0x00, 16, 2, 1, 0);                      /* in 32 x 16 */
    xbox_swizzle_masks(32, 16, &mx, &my);
    s_guest[swizzle_deposit(4 + 15, mx) | swizzle_deposit(4 + 1, my)] = 0x77;
    CHECK(decode(&t, 0x20, 0, NULL) && s_info.guest_bytes == 512);
    CHECK((s_out[16 + 15] & 0xFF) == 0x77);

    t = texture(0x00, 2, 2, 1, 0);                       /* a volume, in 16 x 16 x 16 */
    at = xbox_swizzle_offset_3d(5, 4, 5, 16, 16, 16);
    s_guest[at] = 0x55;
    CHECK(decode(&t, 0x30u | 1u << 28, 0, NULL) && s_info.guest_bytes == 4096);
    CHECK((s_out[4 + 1] & 0xFF) == 0x55);

    t = texture(0x00, 2, 2, 1, 0);                       /* faces */
    t.cube = 1;
    t.t.addr_u = t.t.addr_v = 4;
    xbox_swizzle_masks(16, 16, &mx, &my);
    for (i = 0; i < 6; i++)
        s_guest[256 * i + (swizzle_deposit(4, mx) | swizzle_deposit(5, my))] = (uint8_t)(0x10 + i);
    CHECK(decode(&t, 0x24, 0, NULL) && s_info.face_stride == 256 && !s_info.border);
    for (i = 0; i < 6; i++)
        CHECK((s_out[4 * i + 2] & 0xFF) == 0x10 + i);

    t = texture(0x12, 2, 2, 1, 8);                       /* rows have none */
    CHECK(decode(&t, 0x20, 0, NULL) && !s_info.bordered && s_info.guest_bytes == 16);
}

static void test_blocks(void)
{
    Nv2aBatchTexture t = texture(0x0C, 8, 4, 3, 0);
    uint32_t i;

    for (i = 0; i < 64; i++)
        s_guest[i] = (uint8_t)i;
    CHECK(decode(&t, 0x28, 0, NULL));
    /* 8x4: two blocks; 4x2 and 2x1: one each. Copied as they are. */
    CHECK(s_info.format == NV2A_VK_BC1 && s_info.guest_bytes == 32 && s_info.host_bytes == 32);
    CHECK(!memcmp(s_out, s_guest, 32));
    t.t.color = 0x0F;
    CHECK(decode(&t, 0x28, 0, NULL) && s_info.format == NV2A_VK_BC3 && s_info.host_bytes == 64);
}

static void test_signed(void)
{
    Nv2aBatchTexture t = texture(0x06, 1, 1, 1, 0);
    uint32_t v = 0x80FF8040u;                            /* A R G B */
    int16_t c[4];

    memcpy(s_guest, &v, 4);
    CHECK(decode(&t, 0x28, 0x20000000u, NULL));          /* R alone is signed */
    CHECK(s_info.format == NV2A_VK_RGBA16S && s_info.host_bytes == 8);
    memcpy(c, s_out, 8);
    CHECK(c[0] == -256 && c[1] == 128 * 32767 / 255 && c[2] == 64 * 32767 / 255);
    CHECK(c[3] == 128 * 32767 / 255);
}

static void test_yuv(void)
{
    Nv2aBatchTexture t = texture(0x24, 2, 1, 1, 64);
    static const uint8_t yuy2[4] = { 16, 128, 235, 128 };
    static const uint8_t uyvy[4] = { 128, 16, 128, 235 };

    memcpy(s_guest, yuy2, 4);
    CHECK(decode(&t, 0x28, 0, NULL) && s_out[0] == 0xFF000000u && s_out[1] == 0xFFFFFFFFu);
    memcpy(s_guest, uyvy, 4);
    t.t.color = 0x25;
    CHECK(decode(&t, 0x28, 0, NULL) && s_out[0] == 0xFF000000u && s_out[1] == 0xFFFFFFFFu);
}

static void test_refused(void)
{
    Nv2aBatchTexture t = texture(0x06, 4, 4, 1, 0);

    t.valid = 0;
    CHECK(!nv2a_vk_tex_info(&t, 0x28, 0, &s_info));
    t = texture(0x06, 8192, 4, 1, 0);
    CHECK(!nv2a_vk_tex_info(&t, 0x28, 0, &s_info));
}

/* Every byte counts, at every length. */
static void test_hash(void)
{
    uint8_t b[300];
    uint32_t n, i, k;

    for (i = 0; i < sizeof b; i++)
        b[i] = (uint8_t)(i * 7);
    for (n = 1; n <= sizeof b; n++) {
        uint64_t h = nv2a_vk_hash(b, n);
        CHECK(h == nv2a_vk_hash(b, n) && h != nv2a_vk_hash(b, n - 1));
        for (i = 0; i < n; i++) {
            b[i] ^= 0x10;
            CHECK(nv2a_vk_hash(b, n) != h);
            b[i] ^= 0x10;
        }
        if (n == 272)                            /* every other byte */
            for (i = 0; i < n; i++)
                for (k = 1; k < 256; k++) {
                    b[i] ^= (uint8_t)k;
                    CHECK(nv2a_vk_hash(b, n) != h);
                    b[i] ^= (uint8_t)k;
                }
    }
}

/* A texel through the Vulkan format and its swizzle reads as it decodes. */
static uint32_t through(const Nv2aVkTexNative *n, uint32_t v)
{
    uint32_t c[4] = { 0, 0, 0, 255 }, out = 0, i;       /* r g b a */

#define FIELD(sh, bits) (((v >> (sh)) & ((1u << (bits)) - 1u)) * 255u / ((1u << (bits)) - 1u))
    switch (n->view) {
    case NV2A_VK_VIEW_8888:
        c[0] = FIELD(16, 8); c[1] = FIELD(8, 8); c[2] = FIELD(0, 8); c[3] = v >> 24; break;
    case NV2A_VK_VIEW_565:
        c[0] = FIELD(11, 5); c[1] = FIELD(5, 6); c[2] = FIELD(0, 5); break;
    case NV2A_VK_VIEW_1555:
        c[0] = FIELD(10, 5); c[1] = FIELD(5, 5); c[2] = FIELD(0, 5); c[3] = FIELD(15, 1); break;
    case NV2A_VK_VIEW_4444:
        c[2] = FIELD(12, 4); c[1] = FIELD(8, 4); c[0] = FIELD(4, 4); c[3] = FIELD(0, 4); break;
    case NV2A_VK_VIEW_5551:
        c[0] = FIELD(11, 5); c[1] = FIELD(6, 5); c[2] = FIELD(1, 5); c[3] = FIELD(0, 1); break;
    case NV2A_VK_VIEW_8:
        c[0] = FIELD(0, 8); break;
    default:
        c[0] = FIELD(0, 8); c[1] = FIELD(8, 8); break;
    }
    for (i = 0; i < 4; i++) {                           /* to A R G B */
        char w = n->swizzle[i];
        uint32_t x = w == 'r' ? c[0] : w == 'g' ? c[1] : w == 'b' ? c[2]
                   : w == 'a' ? c[3] : 255;
        out |= x << (i == 3 ? 24 : 16 - 8 * i);
    }
    return out;
}

static void test_native(void)
{
    uint32_t color, k, i, count = 0, v;
    Nv2aVkTexNative n;

    for (color = 0; color < 0x60; color++) {
        if (!nv2a_vk_tex_native(color, 0, &n))
            continue;
        count++;
        for (k = 0; k < 64; k++) {
            Nv2aBatchTexture t = texture(color, 1, 1, 1, 64);

            v = k * 0x9E3779B1u ^ k << 13;
            memcpy(s_guest, &v, 4);
            if (n.bytes < 4)
                v &= (1u << (8 * n.bytes)) - 1u;
            CHECK(decode(&t, 0x28, 0, NULL) && s_info.linear == !n.swizzled);
            for (i = 0; i < 32; i += 8) {
                int a = (int)(s_out[0] >> i & 0xFF), b = (int)(through(&n, v) >> i & 0xFF);
                CHECK(a - b <= 1 && b - a <= 1);
            }
        }
    }
    CHECK(count == 32);
    CHECK(!nv2a_vk_tex_native(0x06, 0x10000000u, &n));  /* signed */
    CHECK(!nv2a_vk_tex_native(0x0C, 0, &n));
}

static void test_raw(void)
{
    uint8_t out[8 * 4 * 2], row[6];
    uint32_t x, y;

    for (x = 0; x < 1024; x++)
        s_guest[x] = (uint8_t)(x * 5 + 1);
    nv2a_vk_tex_raw(s_guest, 2, 1, 8, 4, 0, out);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 8; x++) {
            uint32_t at = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2;
            CHECK(memcmp(out + (y * 8 + x) * 2, s_guest + at * 2, 2) == 0);
        }
    nv2a_vk_tex_raw(s_guest, 1, 0, 3, 2, 64, row);
    CHECK(memcmp(row, s_guest, 3) == 0 && memcmp(row + 3, s_guest + 64, 3) == 0);
}

int main(void)
{
    test_texels();
    test_palette();
    test_swizzle();
    test_linear_pitch();
    test_levels_and_faces();
    test_volume();
    test_volume_blocks();
    test_bordered();
    test_blocks();
    test_signed();
    test_yuv();
    test_refused();
    test_native();
    test_raw();
    test_hash();
    if (!s_fail)
        puts("NV2A Vulkan texture decode: ok");
    return s_fail;
}
