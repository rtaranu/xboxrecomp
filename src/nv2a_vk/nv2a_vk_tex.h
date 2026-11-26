/*
 * Guest textures for the Vulkan rasteriser: what a stage's registers describe
 * and its texels in a layout Vulkan takes. No Vulkan and no GPU needed, so it
 * is tested on its own.
 */
#ifndef NV2A_VK_TEX_H
#define NV2A_VK_TEX_H

#include <stdint.h>
#include "nv2a_backend_batch.h"

typedef enum {
    NV2A_VK_BGRA8 = 0,          /* 0xAARRGGBB dwords */
    NV2A_VK_BC1, NV2A_VK_BC2, NV2A_VK_BC3,
    NV2A_VK_RGBA16S,            /* four s16, for textures with signed channels */
    NV2A_VK_DEPTH               /* a float, of 0..1 (nv2a_vk_tex_depth) */
} Nv2aVkTexFormat;

typedef struct {
    Nv2aVkTexFormat format;
    uint32_t width, height, depth;      /* of level 0 */
    uint32_t faces, levels;
    uint32_t guest_bytes;               /* from the texture's offset */
    uint32_t face_stride;               /* guest bytes between faces */
    uint32_t host_bytes;                /* all faces and levels */
    int      linear;                    /* coordinates are in texels */
    int      bordered;                  /* guest memory has border texels */
    int      border;                    /* of them in the image, on each side of
                                         * level 0, the only one: width and height
                                         * count them */
} Nv2aVkTexInfo;

/* What the stage samples. `format_word` and `filter` are SET_TEXTURE_FORMAT
 * and SET_TEXTURE_FILTER as sent. 0: not a texture this can decode. */
int nv2a_vk_tex_info(const Nv2aBatchTexture *t, uint32_t format_word,
                     uint32_t filter, Nv2aVkTexInfo *out);

/* A depth buffer's format: 0 if the colour field is not one of a texture.
 * `is_float`: 4 bits of exponent (bias 7) and 12 of mantissa, or 8 (127) and
 * 16, without a sign. */
typedef struct { uint32_t bytes; int is_float; } Nv2aVkDepth;
int nv2a_vk_tex_depth(uint32_t color, Nv2aVkDepth *out);

/* The largest depth of a format, in its own units, and a stored depth over it:
 * 16 bits, or the upper 24 of 32. */
double nv2a_vk_depth_most(const Nv2aVkDepth *d);
float nv2a_vk_depth_read(const Nv2aVkDepth *d, uint32_t v);

/* Where a level of a face starts in the decoded texture. */
uint32_t nv2a_vk_tex_level_offset(const Nv2aVkTexInfo *i, uint32_t face,
                                  uint32_t level);

/* Decode everything into `out` (host_bytes): faces in order, each with its
 * levels in order, rows tightly packed. `palette`: 256 dwords, for P8. */
void nv2a_vk_tex_decode(const uint8_t *guest, const uint32_t *palette,
                        uint32_t color, uint32_t pitch, uint32_t filter,
                        const Nv2aVkTexInfo *i, uint8_t *out);

/* Where a level of a face is in guest memory, from the texture's offset. */
uint32_t nv2a_vk_tex_guest_offset(const Nv2aVkTexInfo *i, uint32_t color,
                                  uint32_t face, uint32_t level);

/* A texel as a Vulkan format has it, for textures the GPU drew. */
typedef enum {
    NV2A_VK_VIEW_8888 = 0,      /* B8G8R8A8_UNORM */
    NV2A_VK_VIEW_565,           /* R5G6B5_UNORM_PACK16 */
    NV2A_VK_VIEW_1555,          /* A1R5G5B5_UNORM_PACK16 */
    NV2A_VK_VIEW_4444,          /* B4G4R4A4_UNORM_PACK16 */
    NV2A_VK_VIEW_5551,          /* R5G5B5A1_UNORM_PACK16 */
    NV2A_VK_VIEW_8,             /* R8_UNORM */
    NV2A_VK_VIEW_88,            /* R8G8_UNORM */
    NV2A_VK_VIEWS
} Nv2aVkTexView;

typedef struct {
    Nv2aVkTexView view;
    char     swizzle[4];        /* the view's channel r, g, b and a read: one of
                                 * "rgba", or '1' */
    uint32_t bytes;             /* a texel */
    int      swizzled;          /* not in rows */
} Nv2aVkTexNative;

/* 0: no format has these texels as they are. */
int nv2a_vk_tex_native(uint32_t color, uint32_t filter, Nv2aVkTexNative *out);

/* Texels as they are, one picture, in tightly packed rows. */
void nv2a_vk_tex_raw(const uint8_t *guest, uint32_t bytes, int swizzled,
                     uint32_t w, uint32_t h, uint32_t pitch, uint8_t *out);

/* Not cryptographic; fast enough to run over every texture once a frame. */
uint64_t nv2a_vk_hash(const void *p, uint32_t bytes);

#endif
