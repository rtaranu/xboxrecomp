/*
 * The Vulkan rasteriser for the pushbuffer executor: a batch back end
 * (../kernel/nv2a_backend_batch.h). Design: docs/technical/nv2a-vulkan.md.
 */
#ifndef NV2A_VK_H
#define NV2A_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring Vulkan up and register with the executor. Call before the title
 * submits work. 0: this host cannot (said on stderr); the CPU draws. */
int nv2a_vk_init(void);

/* The same, when RECOMP_NV2A_VK=1 asks for it and the host can show the
 * pictures. */
void nv2a_vk_init_from_env(void);

/* Nonzero: vertex programs run on the CPU, to compare with. For tests. */
void nv2a_vk_cpu_vertices(int on);

/* Nonzero: what the GPU drew is sampled by way of guest memory, to compare with.
 * Slow. For tests. */
void nv2a_vk_textures_via_ram(int on);

/* When what the GPU drew reaches guest memory, for the title's CPU to see.
 * NEVER, the default: only where the toolkit itself needs it. For a title that
 * reads its own pictures, ALWAYS: at each flip, which waits for the GPU; or TOUCH:
 * when it looks (guest_guard.h; where pages cannot be guarded, ALWAYS). A title
 * project may call this for its default before the executor starts;
 * RECOMP_NV2A_VK_COPYBACK=always|touch|never wins. */
typedef enum {
    NV2A_VK_COPYBACK_ALWAYS = 0, NV2A_VK_COPYBACK_NEVER, NV2A_VK_COPYBACK_TOUCH
} Nv2aVkCopyback;
void nv2a_vk_copyback(Nv2aVkCopyback mode);

/* How many pixels wide and high the GPU draws one of the title's, 1 to
 * NV2A_VK_SCALES, or NV2A_VK_SCALE_FIT: the fewest that fill the picture in the
 * window, once that has kept its size for a while (RECOMP_NV2A_VK_SCALE, a
 * number or `fit`). Any thread, any time: from the next flip on. */
#define NV2A_VK_SCALES 4
#define NV2A_VK_SCALE_FIT 0
void nv2a_vk_scale(int scale);
int nv2a_vk_scale_is(void);

/* The least anisotropy, 2 to 16, of textures with mip levels that the title
 * filters; 1 is the title's own (RECOMP_NV2A_VK_ANISO=title|2|4|8|16). Any
 * thread, any time. */
#define NV2A_VK_ANISOTROPY 16
void nv2a_vk_anisotropy(int most);
int nv2a_vk_anisotropy_is(void);

/* Samples of a pixel where the title draws with a depth buffer as large as a
 * picture, 1, 2, 4 or 8, or what the card has below that; what it draws without
 * depths keeps its pixels. And whether the fragment's program runs for each of them
 * (supersampling) where the card can. RECOMP_NV2A_VK_AA=off, msaa2, msaa4, msaa8, or
 * ssaa2, ssaa4, ssaa8 for both. Any thread, any time: from the next flip and the
 * next batch on. */
#define NV2A_VK_SAMPLES 8
void nv2a_vk_samples(int n);
int nv2a_vk_samples_is(void);
void nv2a_vk_shade_samples(int on);
int nv2a_vk_shade_samples_is(void);

/* An alpha test there, without blending, as cut-out leaves and fences have edges
 * of their own (RECOMP_NV2A_VK_ALPHA_TEST_AA=pixel|soft|sharp|sample). PIXEL: one for a
 * pixel, as the console's. SOFT: alpha says how many samples are covered. SHARP,
 * the default: as many as the pixel's part of the edge. SAMPLE: one for each, or
 * SHARP where the card cannot. With supersampling too: SOFT and SHARP then go by each
 * sample's own alpha, and PIXEL shades a cut-out once for a pixel. Any thread, any
 * time: from the next batch on. */
typedef enum {
    NV2A_VK_ALPHA_TEST_AA_PIXEL = 0, NV2A_VK_ALPHA_TEST_AA_SOFT, NV2A_VK_ALPHA_TEST_AA_SHARP,
    NV2A_VK_ALPHA_TEST_AA_SAMPLE,
    NV2A_VK_ALPHA_TEST_AA_MODES
} Nv2aVkAlphaTestAa;
void nv2a_vk_alpha_test_aa(Nv2aVkAlphaTestAa how);
Nv2aVkAlphaTestAa nv2a_vk_alpha_test_aa_is(void);

/* Whether the fragment's program becomes a pipeline of its own for each of the
 * title's states: many times as fast for the card, slow to compile. ASYNC, the
 * default: other threads compile them, and until one is there a pipeline that reads
 * the state draws the same. SYNC: each before its first draw, to compare with.
 * RECOMP_NV2A_VK_SHADER_COMPILE=off|async|sync. Any thread, any time: from the next
 * batch on. */
typedef enum {
    NV2A_VK_COMPILE_OFF = 0, NV2A_VK_COMPILE_ASYNC, NV2A_VK_COMPILE_SYNC,
    NV2A_VK_COMPILE_MODES
} Nv2aVkShaderCompile;
void nv2a_vk_shader_compile(Nv2aVkShaderCompile how);

/* The picture's aspect ratio in the window. AUTO: 16:9 when the title set a
 * widescreen mode, its movies too, as on the console; else 4:3. STRETCH: the
 * window's. Any thread, any time; RECOMP_NV2A_VK_ASPECT=auto|4:3|16:9|stretch. */
typedef enum {
    NV2A_VK_ASPECT_AUTO = 0, NV2A_VK_ASPECT_4_3, NV2A_VK_ASPECT_16_9, NV2A_VK_ASPECT_STRETCH,
    NV2A_VK_ASPECTS
} Nv2aVkAspect;
void nv2a_vk_aspect(Nv2aVkAspect aspect);
Nv2aVkAspect nv2a_vk_aspect_is(void);

/* The title's contiguous memory was freed. Any thread. */
void nv2a_vk_memory_freed(uint32_t va, uint32_t bytes);

/* Waits until everything drawn is done and every picture handed over: for
 * tools, on the executor's thread. */
void nv2a_vk_wait(void);

/* Everything drawn, into guest memory. On the executor's thread. */
void nv2a_vk_store(void);

/* The card, and the title's size of the last picture shown and how many times as
 * wide and high it was drawn; 0: none yet. For the menu; any thread. */
const char *nv2a_vk_card(void);
uint32_t nv2a_vk_picture(uint32_t *w, uint32_t *h);

/* Totals since the start, for tools. */
typedef struct {
    unsigned draws, vertices, refused;
    unsigned programs;          /* draws whose vertices the GPU transformed */
    unsigned specialized_draws; /* draws with a pipeline of their state's own */
    unsigned specialized_pipelines;     /* such pipelines, */
    unsigned specialized_due;   /* and those still to compile */
    unsigned long long vertex_bytes;    /* copied for those */
    unsigned flushes;           /* submit and wait for the GPU */
    unsigned touches;           /* by the title's CPU, of guarded pages */
    unsigned reports;           /* visibility tests taken */
    unsigned early;             /* submissions before the flip, for their sake */
    unsigned readbacks;         /* surfaces copied to guest memory */
    unsigned mid_frame;         /* of those, before a flip asked for it */
    unsigned surface_uploads, texture_uploads;
    unsigned sampled;           /* stages that sampled a drawn surface on the card */
    unsigned card_copies, rearranged;   /* surfaces copied there, or laid out anew */
    unsigned resolved, spread;  /* from several samples to one, and back */
    unsigned long long texture_bytes, hashed_bytes;
    double wait_ms, readback_ms;
    /* Only timed with RECOMP_NV2A_VK_STATS: */
    double transform_ms, texture_ms;
    double vertex_ms;           /* copying vertices, programs and constants */
    double gpu_ms;              /* busy, by its own clock; 0 if it has none */
    double frame_wait_ms;       /* of wait_ms, for the GPU to finish an earlier frame */
} Nv2aVkStats;
void nv2a_vk_stats(Nv2aVkStats *out);

#ifdef __cplusplus
}
#endif

#endif
