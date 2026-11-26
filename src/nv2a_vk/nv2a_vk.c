/*
 * The Vulkan rasteriser (nv2a_vk.h), docs/technical/nv2a-vulkan.md: vertex
 * programs and everything after them on the GPU. Batches without a program
 * are transformed on the CPU.
 *
 * A surface drawn on the GPU is sampled on the GPU, whichever way its memory
 * is looked at, as far as texture_drawn() knows the way. For the rest it is
 * copied back to guest memory, before what cannot be done on the card. Each
 * copy back waits for the GPU (docs/technical/nv2a-vulkan-sync.md). The window
 * is Vulkan's (nv2a_vk_present.h).
 *
 * Commands are recorded into one pair of command buffers, uploads of textures
 * in the first and everything in the title's order in the second. flush()
 * submits both and waits. A flip does so only with
 * RECOMP_NV2A_VK_COPYBACK=always. Otherwise frame_end() submits them and goes on
 * with the next frame's. With touch, the default, the pages of drawn surfaces are
 * guarded, and the title's CPU gets the pixels when it looks (touched()).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_batch.h"
#include "nv2a_perf.h"
#include "platform/host_time.h"
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vk.h"
#include "nv2a_vk_api.h"
#include "nv2a_vk_guard.h"
#include "nv2a_vk_pipeline_cache.h"
#include "nv2a_vk_present.h"
#include "nv2a_vk_tex.h"
#include "nv2a_vk_vtx.h"
#include "nv2a_vk_spirv.h"

#define SLOTS_2D   8192u
#define SLOTS_3D   512u
#define SLOTS_CUBE 1024u
#define SAMPLERS   1024u
#define STREAM_BYTES (128u << 20)
#define FRAME_SUBMITS 16u
#define COUNTS     4096u        /* visibility queries, a power of two */
#define ANSWERS    1024u        /* reports in flight, a power of two */

/* ---- the shaders' view of a batch (shaders/draw.glsl) ------------------- */

typedef struct {
    uint32_t kind, image, smp, flags;
    float    scale[2], lum[2], bump[4];
} GpuStage;

typedef struct {
    float    surface[4];
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t factor0[8], factor1[8];
    uint32_t final0, final1, final_c0, final_c1;
    uint32_t control, stage_program, other_stage, clip_plane;
    uint32_t flags, alpha_func, alpha_ref, fog_color;
    uint32_t fog_mode;
    float    fog0, fog1;
    uint32_t dot_mapping;
    float    eye[3];
    uint32_t shadow_func;
    float    range[2], offset, unused;
    GpuStage stage[4];
} GpuDraw;

typedef char gpu_draw_is_496_bytes[sizeof(GpuDraw) == 496 ? 1 : -1];
typedef char vertex_is_176_bytes[sizeof(Nv2aVshOutput) == 176 ? 1 : -1];

/* ---- state -------------------------------------------------------------- */

typedef struct { uint32_t block; VkDeviceSize at, bytes; } Mem;

typedef struct { VkImage image; VkImageView view; Mem mem; } Image;

/* How a surface's pixels are kept (s_classes). */
enum { C_8888, C_565, C_1555, C_8, C_88, CLASSES };

#define MAX_VIEWS 4
typedef struct { uint32_t key, slot; VkImageView view; } SurfaceView;

/* A colour surface the title draws into. */
/* Where a surface's newest pixels are. */
enum { IN_ONE, IN_MANY, IN_BOTH };

typedef struct Surface {
    int      used;
    int      valid;             /* its image has been given pixels */
    int      dirty;             /* and they are newer than guest memory's */
    uint32_t va, pitch, color, cls, swizzled, bpp, w, h;
    uint32_t bytes;             /* of guest memory, from va */
    GuestGuard guard;           /* of its pages, with TOUCH */
    int      seen;              /* guest memory cannot have changed since it was
                                 * compared: the guard would have said so */
    uint8_t *shadow;            /* guest memory at the last copy either way */
    Image    img;
    VkImageLayout layout;
    Image    flat;              /* scaled: its pixels as the title has them, on
                                 * their way to or from memory */
    VkImageLayout flat_layout;
    Image    many;              /* several samples of each pixel; `img` is what
                                 * they come to, for all but drawing */
    VkImageLayout many_layout;
    int      newest;
    uint64_t turn_frame;        /* the frame of these: */
    uint32_t turns;             /* img to many, for a draw */
    SurfaceView views[MAX_VIEWS];       /* to sample */
    uint32_t view_count;
    uint32_t serial, version;   /* which surface, and which of its pictures */
    uint64_t drawn;             /* g.drawn when it was last drawn into */
    uint32_t copy_of, copy_version;     /* the picture it was rearranged from */
    uint64_t last_use;
    uint64_t checked;           /* the frame guest memory was last compared */
} Surface;

typedef struct {
    int      used;
    uint32_t va, w, h;
    uint32_t pitch, swizzled;   /* as when it was last drawn into */
    Nv2aVkDepth format;
    uint32_t serial, version;   /* which buffer, and which of its depths */
    uint64_t drawn, drawn_frame;        /* g.drawn and g.frame at the last of them */
    uint32_t samples;           /* of img's pixels */
    int      alone;             /* never drawn into with a colour surface */
    int      reciprocal;        /* the image has 1 over each depth: zeta_holds() */
    float    fresh;             /* the only depth in it, over the largest, or -1 */
    Image    img;
    VkImageLayout layout;
    Image    one;               /* with several, a sample of each, to copy */
    VkImageLayout one_layout;
    uint64_t last_use;
} Zeta;

/* A face's level: the surface's picture it holds, or (serial 0) guest
 * memory's. */
typedef struct { uint32_t serial, version, x, y, known; uint64_t hash; } Part;

#define MAX_PARTS (6 * 16)

typedef struct Texture {
    struct Texture *next;       /* in its hash bucket */
    uint32_t offset, palette, format_word, width, height, pitch, filter;
    uint32_t drawn;             /* put together from surfaces, on the card; or 2,
                                 * a depth buffer's copy, 3, as colours, or 4, a
                                 * surface's as depths */
    Nv2aVkTexInfo info;
    uint64_t hash, frame;       /* of the contents, and when last compared */
    uint64_t last_use;
    Image    img;
    uint32_t kind, slot;
    VkImageLayout layout;       /* of the drawn ones */
    Part    *parts;
} Texture;

typedef struct { uint32_t key[4]; VkSampler sampler; } Sampler;
typedef struct { uint64_t key; VkPipeline pipeline; } Pipeline;

/* A pipeline whose fragment program has the state's words as constants. */
#define SPECIALIZED_WORDS 50u
enum { SPECIALIZED_DUE = 0, SPECIALIZED_READY, SPECIALIZED_FAILED };
typedef struct {
    uint64_t hash, key;         /* hash 0: free */
    uint32_t words[SPECIALIZED_WORDS];
    VkPipeline pipeline;        /* once it is READY */
    volatile LONG state;
} Specialized;
typedef struct { Image img; uint32_t kind, slot; } Retired;

/* What belongs to a frame until the GPU is done with it. */
#define MAX_FRAMES 3
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer list[FRAME_SUBMITS][2];     /* up and cmd of each submission */
    uint32_t used;              /* submitted, of these */
    int open;                   /* and its last one is still to come */
    VkFence fence;
    VkQueryPool clock;          /* two timestamps a submission; or none */
    uint32_t perf;              /* nv2a_perf_frame_open() when it began */
    int submitted;              /* all of it */
    Retired *retired;
    uint32_t retired_count, retired_room;
} Frame;

static struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue, show_queue;  /* the second, or the same */
    uint32_t family;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    int has_border, has_fill_modes, has_wide_lines, has_smooth_lines, has_aniso, has_exact_counts;
    int has_parts;              /* pipelines can be made of them */
    int has_logic_ops;
    int has_sample_shading;
    int view_ok[NV2A_VK_VIEWS];

    Frame frames[MAX_FRAMES];
    Frame *f;                   /* being recorded */
    uint32_t frame_count;
    VkCommandBuffer up, cmd;    /* its, of this submission */
    int lost;
    int recording, rendering;

    VkBuffer stream;
    uint8_t *stream_map;
    VkDeviceSize stream_at, stream_from, stream_end;    /* this frame's part */
    uint32_t stream_round;      /* changes when it starts over */
    VkBuffer back;              /* readback */
    VkDeviceMemory back_mem;
    uint8_t *back_map;
    VkDeviceSize back_bytes;
    int back_coherent;

    VkDescriptorSetLayout set_layout;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkSemaphore timeline;       /* the number of the last submission done */
    uint64_t submits;           /* of the last one made */
    uint64_t clocked;           /* GPU time counted up to here, in its ticks */
    VkQueryPool counts;
    uint32_t counted;           /* queries begun */
    int counting;               /* the last of them is open */
    uint64_t answer_at;         /* the submission the last report needs */
    VkShaderModule vert, prog, frag, depth_frag, rearrange_vert, rearrange_frag, cutout_frag;
    VkShaderModule depths_frag, w_vert, w_prog;
    VkShaderModule each_frag, each_depth_frag;  /* run for each sample; 0: no such card */
    int cpu_vertices, textures_via_ram;
    uint32_t scale;                     /* what is drawn is larger than the title's:
                                         * images by this, all other sizes not */
    uint32_t samples;                   /* of a pixel, in what is drawn in depth */
    int many;                           /* the batch is drawn with them */
    int bound_many;                     /* and what rendering is open on is so */
    Nv2aVkCopyback copyback;            /* as asked for */
    Nv2aVkCopyback mode;                /* as it is done */
    uint8_t *window;                    /* the window's unguarded view, or NULL */
    int busy;                           /* in a call from the executor */
    int part;                           /* nv2a_perf_part() before it */
    uint32_t drawn_lo, drawn_hi;        /* every surface is within */

    uint64_t frame;             /* counts flips, from 1 */
    uint64_t drawn;             /* and draws and clears */
    Surface *surf;              /* what the batch draws into */
    Zeta    *zeta;
    Surface *bound_surf;        /* what rendering is open on */
    Zeta    *bound_zeta;
    VkPipelineCache cache;
    VkPipeline bound_pipe;
    GpuDraw  last_draw;
    uint32_t last_index;
    int      last_valid;
} g = { .scale = 1, .samples = 1, .copyback = NV2A_VK_COPYBACK_NEVER };
static volatile LONG s_scale = 1;       /* as asked for */
static volatile LONG s_aniso = 1;       /* likewise */
static volatile LONG s_samples = 1, s_shade_samples;
static volatile LONG s_alpha_test_aa = NV2A_VK_ALPHA_TEST_AA_SHARP;
static volatile LONG s_picture;         /* the last shown: its scale and size */
static volatile LONG s_aspect = NV2A_VK_ASPECT_AUTO;

static Nv2aVkStats s_stats;

#define MAX_SURFACES 64
#define MAX_ZETAS    32
static Surface s_surfaces[MAX_SURFACES];
static Zeta    s_zetas[MAX_ZETAS];

#define TEX_BUCKETS 4096u
static Texture *s_tex[TEX_BUCKETS];
static uint32_t *s_free_slots[4], s_free_count[4];

static Sampler s_samplers[SAMPLERS];
static uint32_t s_sampler_count;

#define PIPELINES 1024u
static Pipeline s_pipes[PIPELINES];

/* The executor's thread adds them, in the order of s_specialized_list; the compiling
 * threads only fill in the pipeline. */
#define MAX_SPECIALIZED 4096u
static Specialized s_specialized[MAX_SPECIALIZED], *s_specialized_list[MAX_SPECIALIZED];
static volatile LONG s_shader_compile = NV2A_VK_COMPILE_ASYNC;
static volatile LONG s_specialized_count, s_specialized_taken, s_specialized_done;
static uint32_t s_specialized_queued;
static HANDLE s_specialized_ready;

static double now_ms(void) { return (double)xbox_HostNowNs() / 1e6; }

/* For what is timed at every draw: 0 unless RECOMP_NV2A_VK_STATS asks. */
static int s_draw_clock;
static double draw_ms(void) { return s_draw_clock ? now_ms() : 0.0; }

static uint8_t *guest(uint32_t va)
{
    if (g.window && va - XBOX_CONTIG_BASE < XBOX_CONTIG_SIZE)
        return g.window + (va - XBOX_CONTIG_BASE);
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static void say_once(uint32_t key, const char *what, uint32_t value)
{
    static uint32_t seen[64];
    static unsigned n;
    unsigned i;

    for (i = 0; i < n; i++)
        if (seen[i] == key)
            return;
    if (n < 64)
        seen[n++] = key;
    fprintf(stderr, "[NV2A-VK] %s (0x%X)\n", what, value);
}

/* ---- device memory: blocks, each with a sorted list of free ranges ------ */

#define BLOCK_BYTES (64u << 20)
typedef struct { VkDeviceSize at, bytes; } Range;
typedef struct {
    VkDeviceMemory mem;
    uint32_t type, count, room;
    Range *free;
} Block;
static Block *s_blocks;
static uint32_t s_block_count;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < g.mem.memoryTypeCount; i++)
        if ((bits >> i & 1u) && (g.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int block_take(Block *b, VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize *at)
{
    uint32_t i;

    for (i = 0; i < b->count; i++) {
        Range r = b->free[i];
        VkDeviceSize start = (r.at + align - 1) / align * align;
        VkDeviceSize end = start + bytes;

        if (end > r.at + r.bytes)
            continue;
        *at = start;
        if (start > r.at && end < r.at + r.bytes) {         /* split */
            if (b->count == b->room) {
                Range *more = (Range *)realloc(b->free, (b->room * 2 + 16) * sizeof *more);
                if (!more)
                    return 0;
                b->free = more;
                b->room = b->room * 2 + 16;
            }
            memmove(&b->free[i + 2], &b->free[i + 1], (b->count - i - 1) * sizeof *b->free);
            b->count++;
            b->free[i].bytes = start - r.at;
            b->free[i + 1].at = end;
            b->free[i + 1].bytes = r.at + r.bytes - end;
        } else if (start > r.at) {
            b->free[i].bytes = start - r.at;
        } else if (end < r.at + r.bytes) {
            b->free[i].at = end;
            b->free[i].bytes = r.at + r.bytes - end;
        } else {
            memmove(&b->free[i], &b->free[i + 1], (b->count - i - 1) * sizeof *b->free);
            b->count--;
        }
        return 1;
    }
    return 0;
}

static int mem_alloc(const VkMemoryRequirements *req, Mem *out)
{
    uint32_t type = memory_type(req->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), i;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    Block *b;

    if (type == UINT32_MAX)
        return 0;
    out->bytes = req->size;
    for (i = 0; i < s_block_count; i++)
        if (s_blocks[i].type == type
            && block_take(&s_blocks[i], req->size, req->alignment, &out->at)) {
            out->block = i;
            return 1;
        }
    b = (Block *)realloc(s_blocks, (s_block_count + 1) * sizeof *b);
    if (!b)
        return 0;
    s_blocks = b;
    b = &s_blocks[s_block_count];
    memset(b, 0, sizeof *b);
    b->type = type;
    b->free = (Range *)malloc(16 * sizeof *b->free);
    if (!b->free)
        return 0;
    b->room = 16;
    b->count = 1;
    b->free[0].at = 0;
    b->free[0].bytes = req->size > BLOCK_BYTES ? req->size : BLOCK_BYTES;
    ai.allocationSize = b->free[0].bytes;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g.dev, &ai, NULL, &b->mem) != VK_SUCCESS) {
        free(b->free);
        return 0;
    }
    out->block = s_block_count++;
    return block_take(b, req->size, req->alignment, &out->at);
}

static void mem_free(const Mem *m)
{
    Block *b = &s_blocks[m->block];
    uint32_t i = 0;

    while (i < b->count && b->free[i].at < m->at)
        i++;
    if (i > 0 && b->free[i - 1].at + b->free[i - 1].bytes == m->at) {
        b->free[i - 1].bytes += m->bytes;
        if (i < b->count && b->free[i - 1].at + b->free[i - 1].bytes == b->free[i].at) {
            b->free[i - 1].bytes += b->free[i].bytes;
            memmove(&b->free[i], &b->free[i + 1], (b->count - i - 1) * sizeof *b->free);
            b->count--;
        }
        return;
    }
    if (i < b->count && m->at + m->bytes == b->free[i].at) {
        b->free[i].at = m->at;
        b->free[i].bytes += m->bytes;
        return;
    }
    if (b->count == b->room) {
        Range *more = (Range *)realloc(b->free, (b->room * 2) * sizeof *more);
        if (!more)
            return;                     /* leaked: the block keeps it */
        b->free = more;
        b->room *= 2;
    }
    memmove(&b->free[i + 1], &b->free[i], (b->count - i) * sizeof *b->free);
    b->free[i].at = m->at;
    b->free[i].bytes = m->bytes;
    b->count++;
}

static void image_destroy(Image *i)
{
    if (i->view)
        vkDestroyImageView(g.dev, i->view, NULL);
    if (i->image) {
        vkDestroyImage(g.dev, i->image, NULL);
        mem_free(&i->mem);
    }
    memset(i, 0, sizeof *i);
}

/* Which channel of the image each of r, g, b and a reads; 1 is one. */
static VkComponentMapping components(const char *swizzle)
{
    VkComponentSwizzle c[4];
    VkComponentMapping m;
    uint32_t i;

    for (i = 0; i < 4; i++)
        c[i] = swizzle[i] == 'r' ? VK_COMPONENT_SWIZZLE_R
             : swizzle[i] == 'g' ? VK_COMPONENT_SWIZZLE_G
             : swizzle[i] == 'b' ? VK_COMPONENT_SWIZZLE_B
             : swizzle[i] == 'a' ? VK_COMPONENT_SWIZZLE_A : VK_COMPONENT_SWIZZLE_ONE;
    m.r = c[0];
    m.g = c[1];
    m.b = c[2];
    m.a = c[3];
    return m;
}

static int image_make(Image *out, VkFormat format, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, uint32_t w, uint32_t h,
                      uint32_t d, uint32_t layers, uint32_t levels, uint32_t samples,
                      VkImageCreateFlags flags, const char *swizzle)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;

    memset(out, 0, sizeof *out);
    ci.flags = flags;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = d;
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (vkCreateImage(g.dev, &ci, NULL, &out->image) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(g.dev, out->image, &req);
    if (!mem_alloc(&req, &out->mem)) {
        vkDestroyImage(g.dev, out->image, NULL);
        memset(out, 0, sizeof *out);
        return 0;
    }
    vkBindImageMemory(g.dev, out->image, s_blocks[out->mem.block].mem, out->mem.at);
    vi.image = out->image;
    vi.viewType = flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT ? VK_IMAGE_VIEW_TYPE_CUBE
                : d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.components = components(swizzle);
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = layers;
    if (vkCreateImageView(g.dev, &vi, NULL, &out->view) != VK_SUCCESS) {
        image_destroy(out);
        return 0;
    }
    return 1;
}

static int image_create(Image *out, VkFormat format, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, uint32_t w, uint32_t h,
                        uint32_t d, uint32_t layers, uint32_t levels,
                        VkImageCreateFlags flags, const char *swizzle)
{
    return image_make(out, format, usage, aspect, w, h, d, layers, levels, 1, flags, swizzle);
}

/* Destroyed once the GPU is past everything recorded so far. */
static void retire(Image *img, uint32_t kind, uint32_t slot)
{
    Frame *f = g.f;

    if (f->retired_count == f->retired_room) {
        uint32_t room = f->retired_room ? f->retired_room * 2 : 64;
        Retired *more = (Retired *)realloc(f->retired, room * sizeof *more);
        if (!more)
            return;
        f->retired = more;
        f->retired_room = room;
    }
    f->retired[f->retired_count].img = *img;
    f->retired[f->retired_count].kind = kind;
    f->retired[f->retired_count++].slot = slot;
    memset(img, 0, sizeof *img);
}

/* ---- visibility tests ---------------------------------------------------- */

/* For the answer thread, in the title's order. */
enum { ANSWER_NONE, ANSWER_CLEAR, ANSWER_REPORT };
typedef struct {
    int      kind;
    uint64_t at;                /* the submission that has what it counts */
    uint32_t upto;              /* queries begun before it */
    uint8_t *to;
    uint32_t va, pixels;
    uint32_t each;              /* samples counted for one of the title's pixels */
    uint64_t time;
} Answer;

static Answer s_answers[ANSWERS];
static volatile LONG s_answer_head, s_answer_tail;      /* executor's, thread's */
static volatile LONG s_counts_read;                     /* of g.counted */
static uint32_t s_count_samples[COUNTS];                /* of a pixel, in each */
static SRWLOCK s_answer_lock = SRWLOCK_INIT;
static HANDLE s_answer_ready, s_answer_done;

static void flush(void);

static DWORD WINAPI answer_thread(LPVOID unused)
{
    uint64_t total = 0, base = 0;
    uint32_t read = 0;

    (void)unused;
    for (;;) {
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        Answer *a;

        WaitForSingleObject(s_answer_ready, INFINITE);  /* one for each */
        a = &s_answers[(uint32_t)s_answer_tail % ANSWERS];
        wi.semaphoreCount = 1;
        wi.pSemaphores = &g.timeline;
        wi.pValues = &a->at;
        vkWaitSemaphores(g.dev, &wi, UINT64_MAX);
        while (read != a->upto) {
            uint64_t v[64] = {0};
            uint32_t first = read % COUNTS, n = COUNTS - first, i;

            if (n > 64)
                n = 64;
            if (n > a->upto - read)
                n = a->upto - read;
            vkGetQueryPoolResults(g.dev, g.counts, first, n, n * sizeof v[0], v, sizeof v[0],
                                  VK_QUERY_RESULT_64_BIT);
            for (i = 0; i < n; i++)
                total += v[i] * (NV2A_VK_SAMPLES / s_count_samples[first + i]);
            read += n;
        }
        InterlockedExchange(&s_counts_read, (LONG)read);
        AcquireSRWLockExclusive(&s_answer_lock);
        if (a->kind == ANSWER_CLEAR)
            base = total;
        else if (a->kind == ANSWER_REPORT)
            nv2a_batch_report(a->to, a->time,
                              (uint32_t)((total - base + a->each - 1) / a->each) + a->pixels);
        InterlockedIncrement(&s_answer_tail);
        ReleaseSRWLockExclusive(&s_answer_lock);
        SetEvent(s_answer_done);
    }
}

static int answers_start(void)
{
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    static int tried;
    HANDLE th;

    if (tried)
        return g.counts != VK_NULL_HANDLE;
    tried = 1;
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = COUNTS;
    s_answer_ready = CreateSemaphoreA(NULL, 0, ANSWERS, NULL);
    s_answer_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    th = s_answer_ready && s_answer_done
            && vkCreateQueryPool(g.dev, &qi, NULL, &g.counts) == VK_SUCCESS
        ? CreateThread(NULL, 0, answer_thread, NULL, 0, NULL) : NULL;
    if (!th) {
        g.counts = VK_NULL_HANDLE;
        say_once(11, "visibility tests are answered with 0 pixels", 0);
        return 0;
    }
    CloseHandle(th);
    if (!g.has_exact_counts)
        say_once(12, "visibility tests say seen or not, without a count", 0);
    return 1;
}

/* Until `left` are unanswered. What they need has been submitted. */
static void answers_wait(uint32_t left)
{
    while (!g.lost && (uint32_t)(s_answer_head - s_answer_tail) > left) {
        int phase = nv2a_perf_phase(NV2A_PERF_GPU);

        WaitForSingleObject(s_answer_done, 100);
        nv2a_perf_phase(phase);
    }
}

static int answer_push(int kind, const Nv2aReport *r)
{
    Answer *a;

    if (!answers_start())
        return 0;
    if ((uint32_t)(s_answer_head - s_answer_tail) >= ANSWERS) {
        flush();
        answers_wait(ANSWERS / 2);
    }
    if (g.lost)
        return 0;
    a = &s_answers[(uint32_t)s_answer_head % ANSWERS];
    memset(a, 0, sizeof *a);
    a->kind = kind;
    a->at = g.recording ? g.submits + 1 : g.submits;
    a->upto = g.counted;
    if (r) {
        a->to = guest(r->va);
        a->va = r->va;
        a->time = r->time;
        a->pixels = r->pixels;
        a->each = g.scale * g.scale * NV2A_VK_SAMPLES;
        g.answer_at = a->at;
        s_stats.reports++;
    }
    InterlockedIncrement(&s_answer_head);
    ReleaseSemaphore(s_answer_ready, 1, NULL);
    return 1;
}

/* Reports into memory the title freed are not wanted. Any thread. */
static void answers_freed(uint32_t va, uint32_t bytes)
{
    LONG i;

    AcquireSRWLockExclusive(&s_answer_lock);
    for (i = s_answer_tail; i != s_answer_head; i++) {
        Answer *a = &s_answers[(uint32_t)i % ANSWERS];

        if (a->kind == ANSWER_REPORT && a->va < (uint64_t)va + bytes
                && va < (uint64_t)a->va + 16)
            a->kind = ANSWER_NONE;
    }
    ReleaseSRWLockExclusive(&s_answer_lock);
}

static void count_end(void)
{
    if (!g.counting)
        return;
    vkCmdEndQuery(g.cmd, g.counts, (g.counted - 1) % COUNTS);
    g.counting = 0;
}

/* Room for another query: may flush, so before rendering is open. */
static void count_room(void)
{
    if (!answers_start() || g.counted - (uint32_t)s_counts_read < COUNTS)
        return;
    if (answer_push(ANSWER_NONE, NULL)) {       /* to have them read */
        flush();
        answers_wait(0);
    }
}

/* What is drawn now counts. Rendering is open. */
static void count_begin(void)
{
    uint32_t q = g.counted % COUNTS;

    if (g.counting || !g.counts || g.lost)
        return;
    vkCmdResetQueryPool(g.up, g.counts, q, 1);
    vkCmdBeginQuery(g.cmd, g.counts, q,
                    g.has_exact_counts ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_count_samples[q] = g.bound_many ? g.samples : 1;
    g.counted++;
    g.counting = 1;
}

/* ---- recording ---------------------------------------------------------- */

static void begin_recording(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    if (g.recording)
        return;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!g.f->used)
        vkResetCommandPool(g.dev, g.f->pool, 0);
    g.up = g.f->list[g.f->used][0];
    g.cmd = g.f->list[g.f->used][1];
    vkBeginCommandBuffer(g.up, &bi);
    vkBeginCommandBuffer(g.cmd, &bi);
    if (!g.f->used)
        g.f->perf = nv2a_perf_frame_open();
    if (g.f->clock) {
        if (!g.f->used)
            vkCmdResetQueryPool(g.up, g.f->clock, 0, 2 * FRAME_SUBMITS);
        vkCmdWriteTimestamp2(g.up, VK_PIPELINE_STAGE_2_NONE, g.f->clock, 2 * g.f->used);
    }
    g.recording = 1;
    g.bound_pipe = VK_NULL_HANDLE;
    g.last_valid = 0;
}

static void end_rendering(void)
{
    if (!g.rendering)
        return;
    count_end();
    vkCmdEndRendering(g.cmd);
    g.rendering = 0;
    g.bound_surf = NULL;
    g.bound_zeta = NULL;
}

/* The recorded commands, to the GPU. The frame's last carry its fence, which
 * is signalled after its earlier ones too. */
static void submit_commands(int last)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkTimelineSemaphoreSubmitInfo ti = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    uint64_t number = g.submits + 1;
    VkResult r;

    if (g.recording) {
        end_rendering();
        if (g.f->clock)
            vkCmdWriteTimestamp2(g.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.f->clock,
                                 2 * g.f->used + 1);
        vkEndCommandBuffer(g.up);
        vkEndCommandBuffer(g.cmd);
        si.commandBufferCount = 2;
        si.pCommandBuffers = g.f->list[g.f->used++];
        g.recording = 0;
    }
    si.pNext = &ti;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.timeline;
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &number;
    nv2a_vk_present_lock(0);
    r = vkQueueSubmit(g.queue, 1, &si, last ? g.f->fence : VK_NULL_HANDLE);
    nv2a_vk_present_unlock(0);
    if (r != VK_SUCCESS) {
        say_once(1, "the device is lost", 0);
        g.lost = 1;
        return;
    }
    g.submits = number;
    g.f->open = !last;
    g.f->submitted = last;
}

static void submit_frame(void) { submit_commands(1); }

/* The GPU's time for a frame it is done with; called in the order they were
 * submitted. A submission that began before the one ahead of it ended did not
 * have the GPU to itself: that part counts once. */
static void frame_clock(Frame *f)
{
    uint64_t t[2 * FRAME_SUBMITS], ticks = 0;
    double ms;
    uint32_t i;

    if (!f->clock || !f->used
        || vkGetQueryPoolResults(g.dev, f->clock, 0, 2 * f->used, sizeof t, t, sizeof t[0],
                                 VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (i = 0; i < f->used; i++) {
        uint64_t from = t[2 * i] > g.clocked ? t[2 * i] : g.clocked;

        if (t[2 * i + 1] > from) {
            ticks += t[2 * i + 1] - from;
            g.clocked = t[2 * i + 1];
        }
    }
    ms = (double)ticks * (double)g.props.limits.timestampPeriod / 1e6;
    s_stats.gpu_ms += ms;
    nv2a_perf_gpu(f->perf, ms);
}

/* Until the GPU is done with a frame; what it kept alive goes. */
static void frame_wait(Frame *f)
{
    uint32_t i;

    if (f->submitted) {
        double from = now_ms();
        int phase = nv2a_perf_phase(NV2A_PERF_GPU);

        if (vkWaitForFences(g.dev, 1, &f->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            say_once(1, "the device is lost", 0);
        nv2a_perf_phase(phase);
        s_stats.wait_ms += now_ms() - from;
        vkResetFences(g.dev, 1, &f->fence);
        frame_clock(f);
        f->submitted = 0;
    }
    f->used = 0;
    f->open = 0;
    for (i = 0; i < f->retired_count; i++) {
        uint32_t k = f->retired[i].kind;        /* the array its slot is in */

        image_destroy(&f->retired[i].img);
        if (k)
            s_free_slots[k][s_free_count[k]++] = f->retired[i].slot;
    }
    f->retired_count = 0;
}

static void stream_start(void)
{
    g.stream_at = g.stream_from;
    g.stream_round++;
}

/* All of them, the oldest first. */
static void frames_wait(void)
{
    uint32_t i;

    for (i = 1; i <= g.frame_count; i++)
        frame_wait(&g.frames[((uint32_t)(g.f - g.frames) + i) % g.frame_count]);
}

/* The same frame goes on, with the GPU past all of it so far. */
static void flush(void)
{
    if (!g.recording && !g.f->open)
        return;
    submit_frame();
    frames_wait();
    s_stats.flushes++;
    stream_start();
}

static void frame_use(uint32_t n)
{
    VkDeviceSize part = STREAM_BYTES / g.frame_count;

    g.f = &g.frames[n];
    g.stream_from = part * n;
    g.stream_end = g.stream_from + part;
    stream_start();
}

/* The frame is all recorded: on to the next, once the GPU is done with what
 * that one held before. */
static void frame_end(void)
{
    double from = now_ms();

    if (g.recording || g.f->open)
        submit_frame();
    frame_use((uint32_t)(g.f - g.frames + 1) % g.frame_count);
    frame_wait(g.f);
    s_stats.frame_wait_ms += now_ms() - from;
}

/* Room in the mapped buffer the GPU reads from. Flushes when it is full, so a
 * caller that needs several pieces takes them together. */
static uint8_t *stream(VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize *at)
{
    VkDeviceSize start = (g.stream_at + align - 1) / align * align;

    if (start + bytes > g.stream_end) {
        flush();
        begin_recording();
        start = (g.stream_from + align - 1) / align * align;
        if (start + bytes > g.stream_end)
            return NULL;
    }
    g.stream_at = start + bytes;
    *at = start;
    return g.stream_map + start;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                          VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst)
{
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

    b.srcStageMask = src_stage;
    b.srcAccessMask = src;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

#define ALL_GRAPHICS VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT
#define TRANSFER     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
#define ANY_WRITE    VK_ACCESS_2_MEMORY_WRITE_BIT
#define ANY_ACCESS   (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)
#define COLOR_BIT    VK_IMAGE_ASPECT_COLOR_BIT
#define ZS_BITS      (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)

/* In the title's order: outside rendering, in the main command buffer. */
static void to_layout(Image *img, VkImageAspectFlags aspect, VkImageLayout *now,
                      VkImageLayout want)
{
    image_barrier(g.cmd, img->image, aspect, *now, want,
                  ALL_GRAPHICS | TRANSFER, ANY_WRITE,
                  ALL_GRAPHICS | TRANSFER, ANY_ACCESS);
    *now = want;
}

/* ---- guest memory ------------------------------------------------------- */

/* Memory the guest owns (the executor's guest_span_ok). */
static int guest_span_ok(uint64_t va, uint64_t bytes)
{
    uint64_t end = va + bytes;

    if (!va || !bytes || bytes > 0x10000000u)
        return 0;
    if (end <= (uint64_t)XBOX_CONTIG_SIZE)
        return 1;
    return va >= XBOX_CONTIG_BASE && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* And that may be written: not the title's image, nor a stack. */
static int guest_writable(uint32_t va, uint64_t bytes)
{
    if (!guest_span_ok(va, bytes))
        return 0;
    if (g_xbox_image_hi && va < g_xbox_image_hi && va + bytes > g_xbox_image_lo)
        return 0;
    return !(va < XBOX_HEAP_BASE && va + bytes > XBOX_STACK_BASE);
}

/* ---- colour surfaces ---------------------------------------------------- */

static void write_descriptor(uint32_t binding, uint32_t slot, VkDescriptorType type,
                             VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

    w.dstSet = g.set;
    w.dstBinding = binding;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
}

/* The title's pixels as the card keeps them, bit for bit. */
static const struct { VkFormat format; uint32_t bytes; Nv2aVkTexView view; } s_classes[CLASSES] = {
    { VK_FORMAT_B8G8R8A8_UNORM,        4, NV2A_VK_VIEW_8888 },
    { VK_FORMAT_R5G6B5_UNORM_PACK16,   2, NV2A_VK_VIEW_565 },
    { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, NV2A_VK_VIEW_1555 },
    { VK_FORMAT_R8_UNORM,              1, NV2A_VK_VIEW_8 },
    { VK_FORMAT_R8G8_UNORM,            2, NV2A_VK_VIEW_88 },
};

static uint32_t class_of(uint32_t color)
{
    switch (color) {
    case 0x1: case 0x2: return C_1555;
    case 0x3:           return C_565;
    case 0x9:           return C_8;                         /* B8 */
    case 0xA:           return C_88;                        /* G8B8 */
    default:            return C_8888;
    }
}

/* Blue is in the red channel, where Vulkan's formats of one and two have it. */
static int blue_first(uint32_t cls) { return cls == C_8 || cls == C_88; }

static VkFormat view_format(Nv2aVkTexView v)
{
    switch (v) {
    case NV2A_VK_VIEW_565:  return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case NV2A_VK_VIEW_1555: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case NV2A_VK_VIEW_4444: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case NV2A_VK_VIEW_5551: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case NV2A_VK_VIEW_8:    return VK_FORMAT_R8_UNORM;
    case NV2A_VK_VIEW_88:   return VK_FORMAT_R8G8_UNORM;
    default:                return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

/* 0xAARRGGBB to the surface's pixel, and that pixel's channels from 0 to 1. */
static uint32_t pixel_out(uint32_t color, uint32_t c)
{
    switch (color) {
    case 0x1: case 0x2:                                     /* X1R5G5B5 */
        return ((c >> 9) & 0x7C00u) | ((c >> 6) & 0x03E0u) | ((c >> 3) & 0x1Fu)
             | (color == 0x2 ? 0x8000u : 0u);
    case 0x3:                                               /* R5G6B5 */
        return ((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x1Fu);
    case 0x9:  return c & 0xFFu;                            /* B8 */
    case 0xA:  return c & 0xFFFFu;                          /* G8B8 */
    default:   return c;
    }
}

static void pixel_floats(uint32_t color, uint32_t v, float out[4])
{
    switch (class_of(color)) {
    case C_1555:
        out[0] = (float)((v >> 10) & 0x1F) / 31.0f;
        out[1] = (float)((v >> 5) & 0x1F) / 31.0f;
        out[2] = (float)(v & 0x1F) / 31.0f;
        out[3] = (float)((v >> 15) & 1);
        break;
    case C_565:
        out[0] = (float)((v >> 11) & 0x1F) / 31.0f;
        out[1] = (float)((v >> 5) & 0x3F) / 63.0f;
        out[2] = (float)(v & 0x1F) / 31.0f;
        out[3] = 1.0f;
        break;
    default:
        out[0] = (float)((v >> 16) & 0xFF) / 255.0f;
        out[1] = (float)((v >> 8) & 0xFF) / 255.0f;
        out[2] = (float)(v & 0xFF) / 255.0f;
        out[3] = (float)(v >> 24) / 255.0f;
        break;
    }
}

static void put(uint8_t *p, uint32_t bpp, uint32_t v) { memcpy(p, &v, bpp); }

/* Where row y's pixels are: at[x] from the start of the surface. */
static void row_offsets(const Surface *s, uint32_t y, uint32_t *at)
{
    uint32_t x, mx, my, ox = 0, oy;

    if (!s->swizzled) {
        for (x = 0; x < s->w; x++)
            at[x] = y * s->pitch + x * s->bpp;
        return;
    }
    xbox_swizzle_masks(s->w, s->h, &mx, &my);
    oy = swizzle_deposit(y, my);
    for (x = 0; x < s->w; x++) {
        at[x] = (ox | oy) * s->bpp;
        ox = (ox - mx) & mx;
    }
}

static int overlaps(const Surface *s, uint32_t va, uint64_t bytes)
{
    return s->used && s->va < va + bytes && (uint64_t)s->va + s->bytes > va;
}

/* Guarded with TOUCH. Elsewhere than in the window, a surface is copied back at
 * each flip. */
static int tracked(const Surface *s)
{
    uint32_t at = s->va - XBOX_CONTIG_BASE;

    return g.mode == NV2A_VK_COPYBACK_TOUCH && at < XBOX_CONTIG_SIZE
        && s->bytes <= XBOX_CONTIG_SIZE - at;
}

/* A page has the strictest guard of the surfaces on it. */
static void pages_guard(uint32_t va, uint32_t bytes)
{
    uint32_t page = va & ~0xFFFu, end = (va + bytes + 0xFFFu) & ~0xFFFu, from, i;
    GuestGuard run = GUEST_OPEN, now;

    for (from = page; page < end; page += 0x1000u) {
        now = GUEST_OPEN;
        for (i = 0; i < MAX_SURFACES; i++)
            if (s_surfaces[i].guard > now && overlaps(&s_surfaces[i], page, 0x1000u))
                now = s_surfaces[i].guard;
        if (now != run && page > from) {
            guest_guard_set(from, page - from, run);
            from = page;
        }
        run = now;
    }
    guest_guard_set(from, end - from, run);
}

static void surface_guard(Surface *s, GuestGuard to)
{
    if (s->guard == to)
        return;
    s->guard = to;
    if (to == GUEST_OPEN)
        s->seen = 0;
    pages_guard(s->va, s->bytes);
}

/* Before its guest memory is compared, so that nothing is missed. */
static void surface_watch(Surface *s)
{
    GuestGuard want = s->dirty ? GUEST_CLOSED : GUEST_READ_ONLY;

    if (tracked(s) && s->guard < want)
        surface_guard(s, want);
}

/* It is being drawn into: newer than guest memory, which closes. */
static void surface_drawn(Surface *s)
{
    if (!s->dirty) {
        s->dirty = 1;
        surface_watch(s);
    }
}

/* And has been. */
static void surface_changed(Surface *s)
{
    s->version++;
    s->drawn = ++g.drawn;
}

/* Guest memory is as it was at the last copy either way. */
static int unchanged(Surface *s)
{
    if (s->guard != GUEST_OPEN && s->seen)
        return 1;
    surface_watch(s);
    if (memcmp(guest(s->va), s->shadow, s->bytes) != 0)
        return 0;
    s->seen = s->guard != GUEST_OPEN;
    return 1;
}

/* Guest memory changed: look again at what else is over it. */
static void memory_changed(uint32_t va, uint32_t bytes, const Surface *except)
{
    uint32_t i;
    Texture *t;

    for (i = 0; i < TEX_BUCKETS; i++)
        for (t = s_tex[i]; t; t = t->next)
            if (t->offset < (uint64_t)va + bytes
                && (uint64_t)t->offset + t->info.guest_bytes > va)
                t->frame = 0;
    for (i = 0; i < MAX_SURFACES; i++)
        if (&s_surfaces[i] != except && overlaps(&s_surfaces[i], va, bytes)) {
            s_surfaces[i].checked = 0;
            s_surfaces[i].seen = 0;
        }
}

/* What its samples come to, if they are newer. */
static void surface_resolve(Surface *s)
{
    VkImageResolve r = {0};

    if (s->newest != IN_MANY)
        return;
    end_rendering();
    begin_recording();
    to_layout(&s->many, COLOR_BIT, &s->many_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(&s->img, COLOR_BIT, &s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = COLOR_BIT;
    r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
    r.extent.width = s->w * g.scale;
    r.extent.height = s->h * g.scale;
    r.extent.depth = 1;
    vkCmdResolveImage(g.cmd, s->many.image, s->many_layout, s->img.image, s->layout, 1, &r);
    s->newest = IN_BOTH;
    s_stats.resolved++;
}

/* Its image, to be changed but for drawing into it with many samples. */
static void surface_one(Surface *s)
{
    surface_resolve(s);
    s->newest = IN_ONE;
}

static void surface_layout(Surface *s, VkImageLayout want)
{
    if (want == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        surface_one(s);
    else
        surface_resolve(s);
    if (s->layout == want)
        return;
    end_rendering();
    begin_recording();
    to_layout(&s->img, COLOR_BIT, &s->layout, want);
}

/* The surface at the title's own size, for buffers. */
static int surface_flat(Surface *s)
{
    if (s->flat.image)
        return 1;
    s->flat_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return image_create(&s->flat, s_classes[s->cls].format,
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT, COLOR_BIT, s->w, s->h, 1, 1, 1, 0, "rgba");
}

/* Part of a picture between two images, `up` or down by the scale. */
static VkImageBlit scaled_blit(int up, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    int32_t k = (int32_t)g.scale, from = up ? 1 : k, to = up ? k : 1;
    VkImageBlit b;

    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = COLOR_BIT;
    b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
    b.srcOffsets[0].x = x * from;
    b.srcOffsets[0].y = y * from;
    b.srcOffsets[1].x = (x + (int32_t)w) * from;
    b.srcOffsets[1].y = (y + (int32_t)h) * from;
    b.dstOffsets[0].x = x * to;
    b.dstOffsets[0].y = y * to;
    b.dstOffsets[1].x = (x + (int32_t)w) * to;
    b.dstOffsets[1].y = (y + (int32_t)h) * to;
    b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
    return b;
}

/* Parts of the stream buffer over the surface's pixels. */
static void surface_put(Surface *s, uint32_t n, const VkBufferImageCopy *c)
{
    uint32_t i;

    surface_one(s);
    if (g.scale == 1) {
        to_layout(&s->img, COLOR_BIT, &s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(g.cmd, g.stream, s->img.image, s->layout, n, c);
        return;
    }
    if (!surface_flat(s))
        return;
    to_layout(&s->flat, COLOR_BIT, &s->flat_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(g.cmd, g.stream, s->flat.image, s->flat_layout, n, c);
    to_layout(&s->flat, COLOR_BIT, &s->flat_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(&s->img, COLOR_BIT, &s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (i = 0; i < n; i++) {
        VkImageBlit b = scaled_blit(1, c[i].imageOffset.x, c[i].imageOffset.y,
                                    c[i].imageExtent.width, c[i].imageExtent.height);

        vkCmdBlitImage(g.cmd, s->flat.image, s->flat_layout, s->img.image, s->layout, 1, &b,
                       VK_FILTER_NEAREST);
    }
}

/* All of its pixels into a buffer. */
static void surface_get(Surface *s, VkBuffer to, VkDeviceSize at)
{
    VkBufferImageCopy c = {0};
    Image *from = &s->img;
    VkImageLayout *layout = &s->layout;

    surface_resolve(s);
    to_layout(&s->img, COLOR_BIT, &s->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (g.scale > 1) {
        VkImageBlit b = scaled_blit(0, 0, 0, s->w, s->h);

        if (!surface_flat(s))
            return;
        from = &s->flat;
        layout = &s->flat_layout;
        to_layout(from, COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(g.cmd, s->img.image, s->layout, from->image, *layout, 1, &b,
                       VK_FILTER_LINEAR);
        to_layout(from, COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    c.bufferOffset = at;
    c.imageSubresource.aspectMask = COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = s->w;
    c.imageExtent.height = s->h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(g.cmd, from->image, *layout, to, 1, &c);
}

/* All of it, as guest memory has it. */
static void surface_upload(Surface *s)
{
    VkDeviceSize at;
    VkBufferImageCopy c = {0};
    uint8_t *px;

    end_rendering();
    begin_recording();
    px = stream((VkDeviceSize)s->w * s->h * s->bpp, 16, &at);
    if (!px)
        return;
    memcpy(s->shadow, guest(s->va), s->bytes);
    nv2a_vk_tex_raw(s->shadow, s->bpp, (int)s->swizzled, s->w, s->h, s->pitch, px);
    c.bufferOffset = at;
    c.imageSubresource.aspectMask = COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = s->w;
    c.imageExtent.height = s->h;
    c.imageExtent.depth = 1;
    surface_put(s, 1, &c);
    s->dirty = 0;
    s->valid = 1;
    s->seen = s->guard != GUEST_OPEN;
    s->version++;
    s_stats.surface_uploads++;
}

/* The title rewrote more than half of it. Pixels it happened to write as they
 * were in stale guest memory do not show as written, so all are taken as its. */
static int mostly_rewritten(const Surface *s)
{
    const uint8_t *mem = guest(s->va);
    uint32_t i, n = 0;

    for (i = 0; i + s->bpp <= s->bytes; i += s->bpp)
        n += memcmp(mem + i, s->shadow + i, s->bpp) != 0;
    return n > s->bytes / s->bpp / 2;
}

/* The pixels the title wrote since the last copy, over the card's. */
static void surface_patch(Surface *s)
{
    static VkBufferImageCopy *c;
    static uint32_t room;
    uint32_t offs[4096], x, y, n = 0;
    VkDeviceSize at, used = 0;
    const uint8_t *mem = guest(s->va);
    uint8_t *px;

    end_rendering();
    begin_recording();
    px = stream((VkDeviceSize)s->w * s->h * s->bpp, 16, &at);
    if (!px)
        return;
    for (y = 0; y < s->h; y++) {
        row_offsets(s, y, offs);
        for (x = 0; x < s->w; x++) {
            VkBufferImageCopy *r;
            uint32_t from = x;

            while (x < s->w && memcmp(mem + offs[x], s->shadow + offs[x], s->bpp) != 0) {
                memcpy(s->shadow + offs[x], mem + offs[x], s->bpp);
                memcpy(px + used, s->shadow + offs[x], s->bpp);
                used += s->bpp;
                x++;
            }
            if (x == from)
                continue;
            r = n ? &c[n - 1] : NULL;
            if (r && r->imageOffset.x == (int32_t)from && r->imageExtent.width == x - from
                && (uint32_t)r->imageOffset.y + r->imageExtent.height == y) {
                r->imageExtent.height++;        /* the same columns, next row */
                continue;
            }
            if (n == room) {
                uint32_t more = room ? room * 2 : 256;
                VkBufferImageCopy *p = (VkBufferImageCopy *)realloc(c, more * sizeof *c);
                if (!p)
                    return;
                c = p;
                room = more;
            }
            r = &c[n++];
            memset(r, 0, sizeof *r);
            r->bufferOffset = at + used - (VkDeviceSize)(x - from) * s->bpp;
            r->imageSubresource.aspectMask = COLOR_BIT;
            r->imageSubresource.layerCount = 1;
            r->imageOffset.x = (int32_t)from;
            r->imageOffset.y = (int32_t)y;
            r->imageExtent.width = x - from;
            r->imageExtent.height = r->imageExtent.depth = 1;
        }
    }
    s->seen = s->guard != GUEST_OPEN;
    if (!n)
        return;                         /* between the rows of a wider pitch */
    surface_put(s, n, c);
    s->version++;
    s_stats.surface_uploads++;
}

/* The GPU's pixels into guest memory. A pixel the title wrote itself since
 * the last copy is kept, and goes to the card when the surface is next used. */
static void surface_store(Surface *s, const uint8_t *px)
{
    uint8_t *mem = guest(s->va);
    uint32_t offs[4096], x, y, kept = 0;
    int untouched = (s->guard != GUEST_OPEN && s->seen)
                 || memcmp(mem, s->shadow, s->bytes) == 0;

    for (y = 0; y < s->h; y++) {
        row_offsets(s, y, offs);
        if (untouched && !s->swizzled) {
            memcpy(mem + offs[0], px, (size_t)s->w * s->bpp);
            memcpy(s->shadow + offs[0], px, (size_t)s->w * s->bpp);
            px += (size_t)s->w * s->bpp;
            continue;
        }
        for (x = 0; x < s->w; x++, px += s->bpp) {
            if (!untouched && memcmp(mem + offs[x], s->shadow + offs[x], s->bpp) != 0)
                kept++;
            else
                memcpy(mem + offs[x], px, s->bpp);
            memcpy(s->shadow + offs[x], px, s->bpp);
        }
    }
    s->dirty = 0;
    if (s->guard == GUEST_CLOSED)
        surface_guard(s, GUEST_READ_ONLY);      /* a read needs no more */
    memory_changed(s->va, s->bytes, kept ? NULL : s);
}

/* A mapped buffer the GPU writes and the CPU reads. */
static int create_readback(VkDeviceSize bytes, VkBuffer *buffer, VkDeviceMemory *mem,
                           uint8_t **map, int *coherent)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    uint32_t type;

    ci.size = bytes;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g.dev, &ci, NULL, buffer) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(g.dev, *buffer, &req);
    type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                         | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(g.dev, &ai, NULL, mem) != VK_SUCCESS)
        return 0;
    vkBindBufferMemory(g.dev, *buffer, *mem, 0);
    if (vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map) != VK_SUCCESS)
        return 0;
    *coherent = (g.mem.memoryTypes[type].propertyFlags
                 & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return 1;
}

static int readback_room(VkDeviceSize bytes)
{
    if (bytes <= g.back_bytes)
        return 1;
    flush();                                /* nothing recorded uses the old one */
    if (g.back) {
        vkDestroyBuffer(g.dev, g.back, NULL);
        vkFreeMemory(g.dev, g.back_mem, NULL);
        g.back = VK_NULL_HANDLE;
        g.back_bytes = 0;
    }
    bytes += bytes / 2;
    if (!create_readback(bytes, &g.back, &g.back_mem, &g.back_map, &g.back_coherent))
        return 0;
    g.back_bytes = bytes;
    return 1;
}

/* Copy every dirty surface that meets [va, va + bytes) into guest memory.
 * Returns how many there were. */
static unsigned store_range(uint32_t va, uint64_t bytes, const Surface *except)
{
    Surface *list[MAX_SURFACES];
    VkDeviceSize at[MAX_SURFACES], total = 0;
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    unsigned n = 0, i;
    double from;
    int phase;

    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surfaces[i];
        if (s != except && s->dirty && overlaps(s, va, bytes)) {
            at[n] = total;
            total += ((VkDeviceSize)s->w * s->h * s->bpp + 15) & ~(VkDeviceSize)15;
            list[n++] = s;
        }
    }
    if (!n || !readback_room(total))
        return 0;
    phase = nv2a_perf_phase(NV2A_PERF_BACK);
    from = now_ms();
    end_rendering();
    begin_recording();
    for (i = 0; i < n; i++)
        surface_get(list[i], g.back, at[i]);
    mb.srcStageMask = TRANSFER;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(g.cmd, &di);
    flush();
    if (!g.back_coherent) {
        VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        r.memory = g.back_mem;
        r.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(g.dev, 1, &r);
    }
    for (i = 0; i < n; i++)
        surface_store(list[i], g.back_map + at[i]);
    s_stats.readbacks += n;
    s_stats.readback_ms += now_ms() - from;
    nv2a_perf_phase(phase);
    return n;
}

static void surface_drop(Surface *s)
{
    uint32_t i;

    if (s->dirty)
        store_range(s->va, s->bytes, NULL);
    end_rendering();
    for (i = 0; i < s->view_count; i++) {
        Image v;

        memset(&v, 0, sizeof v);
        v.view = s->views[i].view;
        retire(&v, 1, s->views[i].slot);
    }
    retire(&s->img, 0, 0);
    if (s->flat.image)
        retire(&s->flat, 0, 0);
    if (s->many.image)
        retire(&s->many, 0, 0);
    free(s->shadow);
    if (g.surf == s)
        g.surf = NULL;
    surface_guard(s, GUEST_OPEN);
    memset(s, 0, sizeof *s);
}

/* A view of the surface the shader can sample, its channels as `swizzle` says
 * (Nv2aVkTexNative). 0: no such view. */
static int surface_view(Surface *s, Nv2aVkTexView format, const char *swizzle, uint32_t *slot)
{
    VkImageViewUsageCreateInfo use = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    uint32_t key = (uint32_t)format << 24, i;

    for (i = 0; i < 4; i++)             /* the low five bits tell them apart */
        key |= ((uint32_t)swizzle[i] & 0x1Fu) << (i * 5);
    for (i = 0; i < s->view_count; i++)
        if (s->views[i].key == key) {
            *slot = s->views[i].slot;
            return 1;
        }
    if (s->view_count == MAX_VIEWS || !s_free_count[1] || !g.view_ok[format])
        return 0;
    use.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    vi.pNext = &use;
    vi.image = s->img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = view_format(format);
    vi.components = components(swizzle);
    vi.subresourceRange.aspectMask = COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g.dev, &vi, NULL, &s->views[i].view) != VK_SUCCESS)
        return 0;
    s->views[i].key = key;
    s->views[i].slot = *slot = s_free_slots[1][--s_free_count[1]];
    s->view_count++;
    write_descriptor(1, *slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, s->views[i].view,
                     VK_NULL_HANDLE);
    return 1;
}

/* A new record, with no pixels yet. */
static Surface *surface_new(uint32_t va, uint32_t pitch, uint32_t color, uint32_t swizzled,
                            uint32_t w, uint32_t h)
{
    static uint32_t serial;
    uint32_t i, cls = class_of(color);
    Surface *s = NULL;

    for (i = 0; i < MAX_SURFACES; i++)
        if (!s_surfaces[i].used)
            s = &s_surfaces[i];
    if (!s) {
        for (i = 0; i < MAX_SURFACES; i++)
            if (&s_surfaces[i] != g.surf && (!s || s_surfaces[i].last_use < s->last_use))
                s = &s_surfaces[i];
        surface_drop(s);
    }
    s->va = va;
    s->pitch = pitch;
    s->color = color;
    s->cls = cls;
    s->swizzled = swizzled;
    s->bpp = s_classes[cls].bytes;
    s->w = w;
    s->h = h;
    s->bytes = swizzled ? w * h * s->bpp : pitch * h;
    s->serial = ++serial;
    s->last_use = g.frame;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s->shadow = (uint8_t *)malloc(s->bytes);
    if (!s->shadow
        || !image_create(&s->img, s_classes[cls].format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         COLOR_BIT, w * g.scale, h * g.scale, 1, 1, 1,
                         s->bpp == 2 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, "rgba")) {
        free(s->shadow);
        memset(s, 0, sizeof *s);
        return NULL;
    }
    s->used = 1;
    if (g.drawn_lo >= g.drawn_hi) {
        g.drawn_lo = s->va;
        g.drawn_hi = s->va + s->bytes;
    }
    if (s->va < g.drawn_lo)
        g.drawn_lo = s->va;
    if (s->va + s->bytes > g.drawn_hi)
        g.drawn_hi = s->va + s->bytes;
    return s;
}

/* All of a picture drawn by `frag`. */
static VkPipeline picture_pipeline(VkPipeline *made, VkFormat format, VkShaderModule frag,
                                   uint32_t samples)
{
    static const VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR };
    int phase;
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi =
        { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia =
        { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp =
        { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs =
        { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms =
        { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState at;
    VkPipelineColorBlendStateCreateInfo cb =
        { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy =
        { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };

    if (*made)
        return *made;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = g.rearrange_vert;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = frag;
    st[0].pName = st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
    memset(&at, 0, sizeof at);
    at.colorWriteMask = 0xF;
    cb.attachmentCount = 1;
    cb.pAttachments = &at;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamic;
    rc.colorAttachmentCount = 1;
    rc.pColorAttachmentFormats = &format;
    ci.pNext = &rc;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = g.layout;
    phase = nv2a_perf_phase(NV2A_PERF_PIPELINES);
    if (vkCreateGraphicsPipelines(g.dev, g.cache, 1, &ci, NULL, made) != VK_SUCCESS)
        say_once(10, "cannot make a pipeline", format);
    nv2a_perf_phase(phase);
    return *made;
}

static VkPipeline rearrange_pipeline(uint32_t cls, uint32_t samples)
{
    static VkPipeline all[CLASSES][NV2A_VK_SAMPLES + 1];

    return picture_pipeline(&all[cls][samples], s_classes[cls].format, g.rearrange_frag,
                            samples);
}

static uint32_t log2_of(uint32_t v)
{
    uint32_t n = 0;

    while (v >> (n + 1))
        n++;
    return n;
}

/* The same memory, laid out differently: every pixel of one is a pixel of the
 * other. */
static int same_memory(const Surface *a, const Surface *b)
{
    return a->va == b->va && a->bytes == b->bytes && a->cls == b->cls
        && !g.textures_via_ram && (a->swizzled || a->pitch == a->w * a->bpp)
        && (b->swizzled || b->pitch == b->w * b->bpp);
}

/* `view`, so large without the scale, becomes what the push constants say. */
static void picture_into(VkImageView view, uint32_t w, uint32_t h, VkPipeline pipe,
                         const uint32_t push[5])
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp;

    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = w * g.scale;
    ri.renderArea.extent.height = h * g.scale;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(g.cmd, &ri);
    memset(&vp, 0, sizeof vp);
    vp.width = (float)(w * g.scale);
    vp.height = (float)(h * g.scale);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &ri.renderArea);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set,
                            0, NULL);
    vkCmdPushConstants(g.cmd, g.layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       5 * sizeof *push, push);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(g.cmd);
}

/* The image for its samples. 0: none. */
static int surface_many(Surface *s)
{
    if (s->many.image)
        return 1;
    s->many_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s->newest = IN_ONE;
    return image_make(&s->many, s_classes[s->cls].format,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      COLOR_BIT, s->w * g.scale, s->h * g.scale, 1, 1, 1, g.samples, 0, "rgba");
}

/* Its image's pixels for all samples of each. 0: not done. */
static int surface_spread(Surface *s)
{
    VkPipeline pipe = rearrange_pipeline(s->cls, g.samples);
    uint32_t push[5] = {0}, slot;

    if (!pipe || !surface_view(s, s_classes[s->cls].view, "rgba", &slot))
        return 0;
    end_rendering();
    begin_recording();
    surface_layout(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(&s->many, COLOR_BIT, &s->many_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    push[0] = slot;
    push[1] = 1u << 1 | 1u << 4;
    push[2] = push[4] = s->w * g.scale;
    picture_into(s->many.view, s->w, s->h, pipe, push);
    s->newest = IN_BOTH;
    s_stats.spread++;
    return 1;
}

/* `to` becomes what `from` is, on the card. */
static int rearrange(Surface *to, Surface *from)
{
    VkPipeline pipe = rearrange_pipeline(to->cls, 1);
    uint32_t push[5], slot;

    if (!pipe || !surface_view(from, s_classes[from->cls].view, "rgba", &slot))
        return 0;
    end_rendering();
    begin_recording();
    surface_layout(from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to->newest = IN_ONE;
    to_layout(&to->img, COLOR_BIT, &to->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    push[0] = slot;
    push[1] = to->swizzled | g.scale << 1 | log2_of(to->w) << 8 | log2_of(to->h) << 16;
    push[2] = to->w;
    push[3] = from->swizzled | log2_of(from->w) << 8 | log2_of(from->h) << 16;
    push[4] = from->w;
    picture_into(to->img.view, to->w, to->h, pipe, push);
    memcpy(to->shadow, from->shadow, to->bytes);
    to->valid = 1;
    to->dirty = 0;
    to->seen = to->guard != GUEST_OPEN && from->seen;
    to->checked = from->checked;
    to->version++;
    to->copy_of = from->serial;
    to->copy_version = from->version;
    s_stats.rearranged++;
    return 1;
}

/* Guest memory's changes onto the card. */
static void surface_refresh(Surface *s)
{
    s->checked = g.frame;
    if (unchanged(s))
        return;
    if (s->dirty && !(g.mode == NV2A_VK_COPYBACK_NEVER && mostly_rewritten(s)))
        surface_patch(s);
    else
        surface_upload(s);
}

/* With guest memory stale for long, a range the title wrote to and then uses as
 * something else than the surface that was there is the title's, whole. */
static void title_owns(uint32_t va, uint64_t bytes, const Surface *except)
{
    uint32_t i;

    if (g.mode != NV2A_VK_COPYBACK_NEVER)
        return;
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surfaces[i];
        uint64_t from, to;

        if (s == except || s == g.surf || !s->dirty || !overlaps(s, va, bytes))
            continue;
        from = va > s->va ? va : s->va;
        to = va + bytes < (uint64_t)s->va + s->bytes ? va + bytes : (uint64_t)s->va + s->bytes;
        if (memcmp(guest((uint32_t)from), s->shadow + (from - s->va), (size_t)(to - from)) != 0)
            s->dirty = s->valid = 0;
    }
}

/* Before the CPU reads what is neither a surface nor a texture. */
static void memory_read(uint32_t va, uint64_t bytes)
{
    if (va < g.drawn_hi && va + bytes > g.drawn_lo)
        s_stats.mid_frame += store_range(va, bytes, NULL);
}

/* [va, va + bytes) as the title's CPU, or the executor, may use it: what was
 * drawn on its pages is copied back, and the pages allow it. */
static void pages_open(uint32_t va, uint64_t bytes, int write)
{
    uint64_t lo = va & ~0xFFFu, end = (va + bytes + 0xFFFu) & ~0xFFFull;
    GuestGuard most = write ? GUEST_OPEN : GUEST_READ_ONLY;
    uint32_t i;

    s_stats.mid_frame += store_range((uint32_t)lo, end - lo, NULL);
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surfaces[i];

        if (s->guard <= most || !overlaps(s, (uint32_t)lo, end - lo))
            continue;
        surface_guard(s, most);
    }
}

/* Before the executor reads guest memory itself. */
static void memory_open(uint32_t va, uint64_t bytes)
{
    if (g.mode != NV2A_VK_COPYBACK_TOUCH)
        memory_read(va, bytes);
    else if (va < g.drawn_hi + 0x1000u && va + bytes + 0x1000u > g.drawn_lo)
        pages_open(va, bytes, 0);
}

/* Its newest pixels on the card, wherever they are now. `draw`: it is about to
 * be drawn into, so a newer one over the same memory hands over. */
static void surface_current(Surface *s, int draw)
{
    uint32_t i;

    s->last_use = g.frame;
    if (draw && tracked(s))             /* its memory is about to be stale */
        surface_guard(s, GUEST_CLOSED);
    else
        surface_watch(s);
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *k = &s_surfaces[i];

        if (k == s || !k->dirty || !overlaps(k, s->va, s->bytes))
            continue;
        if (same_memory(k, s)) {
            surface_refresh(k);
            if ((s->valid && s->copy_of == k->serial && s->copy_version == k->version)
                || rearrange(s, k)) {
                if (draw) {
                    s->dirty = 1;
                    k->dirty = k->valid = 0;
                    surface_guard(k, GUEST_OPEN);       /* s has its pages */
                }
                return;                 /* it was all of it */
            }
        }
        title_owns(s->va, s->bytes, s);
        s_stats.mid_frame += store_range(s->va, s->bytes, s);
        break;
    }
    if (s->valid && !draw && s->checked == g.frame && (s->seen || !tracked(s)))
        return;
    if (!s->valid || (!s->dirty && !unchanged(s)))
        for (i = 0; i < MAX_SURFACES; i++) {    /* from a clean one, if there is */
            Surface *k = &s_surfaces[i];

            if (k != s && k->used && k->valid && !k->dirty && same_memory(k, s)
                && unchanged(k) && rearrange(s, k)) {
                s->checked = g.frame;
                return;
            }
        }
    if (!s->valid) {
        surface_upload(s);
        s->checked = g.frame;
    } else {
        surface_refresh(s);
    }
}

/* The same surface with more rows: what is drawn so far moves over. */
static void surface_grow(Surface *to, Surface *from)
{
    VkImageCopy c = {0};

    if (from->dirty)
        surface_refresh(from);
    surface_upload(to);
    if (from->dirty) {
        surface_resolve(from);
        to_layout(&from->img, COLOR_BIT, &from->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = COLOR_BIT;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.extent.width = from->w * g.scale;
        c.extent.height = from->h * g.scale;
        c.extent.depth = 1;
        vkCmdCopyImage(g.cmd, from->img.image, from->layout, to->img.image, to->layout, 1, &c);
        surface_drawn(to);
        from->dirty = 0;
        s_stats.card_copies++;
    }
    surface_drop(from);
}

/* The surface a batch draws into, its pixels current. NULL: not one this can
 * draw into. */
static Surface *surface_for(const Nv2aTarget *t)
{
    uint32_t color = t->format & 0xF, swizzled = ((t->format >> 8) & 0xF) == 2;
    uint32_t w, h, i, bpp = t->bytes_per_pixel;
    uint64_t bytes;
    Surface *s = NULL, *low = NULL;

    if (!t->color_va || !t->color_pitch || !t->clip_w || !t->clip_h
        || bpp != s_classes[class_of(color)].bytes)
        return NULL;
    if (swizzled) {
        w = 1u << ((t->format >> 16) & 0xF);
        h = 1u << ((t->format >> 24) & 0xF);
        bytes = (uint64_t)w * h * bpp;
    } else {
        w = t->color_pitch / bpp;
        h = t->clip_y + t->clip_h;
        bytes = (uint64_t)t->color_pitch * h;
    }
    if (!w || w > 4096 || h > 4096 || !guest_writable(t->color_va, bytes))
        return NULL;

    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *k = &s_surfaces[i];

        if (!k->used || k->va != t->color_va || k->cls != class_of(color)
            || k->swizzled != swizzled || k->w != w
            || (swizzled ? k->h != h : k->pitch != t->color_pitch))
            continue;
        if (k->h >= h)
            s = k;
        else
            low = k;
    }
    if (s && s == g.surf && (s->guard == GUEST_CLOSED || !tracked(s))) {
        s->color = color;
        return s;                       /* still the one being drawn into */
    }
    if (!s) {
        s = surface_new(t->color_va, t->color_pitch, color, swizzled, w, h);
        if (!s)
            return NULL;
        if (low && low != s)            /* not evicted for it */
            surface_grow(s, low);
    }
    s->color = color;
    surface_current(s, 1);
    return s;
}

/* What a clear leaves, written to guest memory as the GPU will draw it, so
 * that a title which then writes pixels itself writes over the clear. */
static void surface_fill_guest(Surface *s, const VkRect2D *r, uint32_t v)
{
    uint32_t offs[4096], x, y;
    uint8_t *mem = guest(s->va);

    for (y = 0; y < r->extent.height; y++) {
        row_offsets(s, (uint32_t)r->offset.y + y, offs);
        for (x = 0; x < r->extent.width; x++) {
            uint32_t at = offs[(uint32_t)r->offset.x + x];
            put(mem + at, s->bpp, v);
            put(s->shadow + at, s->bpp, v);
        }
        if (!s->swizzled && r->extent.width) {      /* the other rows alike */
            uint32_t at = offs[r->offset.x], n = r->extent.width * s->bpp;
            for (y = 1; y < r->extent.height; y++) {
                memcpy(mem + at + (size_t)y * s->pitch, mem + at, n);
                memcpy(s->shadow + at + (size_t)y * s->pitch, mem + at, n);
            }
        }
    }
    if (r->extent.width == s->w && r->extent.height == s->h)
        s->dirty = 0;
    memory_changed(s->va, s->bytes, s);
}

/* ---- depth and stencil surfaces: on the GPU only ------------------------ */

/* Whether what is drawn with a depth buffer of this size has several samples:
 * not a texture's, which is square or small, nor what the title gives several
 * samples itself. */
static uint32_t samples_for(const Nv2aTarget *t, uint32_t w, uint32_t h)
{
    return w >= 320 && h >= 200 && w != h && t->aa_sx * t->aa_sy == 1 ? g.samples : 1;
}

static int zeta_image(Zeta *z, uint32_t samples, uint32_t w, uint32_t h)
{
    z->samples = samples;
    return image_make(&z->img, VK_FORMAT_D32_SFLOAT_S8_UINT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                      | (samples == 1 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u),
                      ZS_BITS, w * g.scale, h * g.scale, 1, 1, 1, samples, 0, "rgba");
}

/* No room for them: one, from the next flip on. */
static void samples_off(void)
{
    say_once(13, "no memory for multisampling, samples", g.samples);
    InterlockedExchange(&s_samples, 1);
}

/* What is drawn into with no colour surface: a size. */
static Surface s_nothing;

static Surface *nothing_for(const Nv2aTarget *t)
{
    uint32_t bytes = ((t->format >> 4) & 0xF) == 1 ? 2 : 4, w, h;

    if (!t->zeta_va || !t->zeta_pitch || !t->clip_w || !t->clip_h)
        return NULL;
    if (((t->format >> 8) & 0xF) == 2) {
        w = 1u << ((t->format >> 16) & 0xF);
        h = 1u << ((t->format >> 24) & 0xF);
    } else {
        w = t->zeta_pitch / bytes;
        h = t->clip_y + t->clip_h;
    }
    if (!w || w > 4096 || h > 4096)
        return NULL;
    if (s_nothing.w != w || s_nothing.h != h)
        end_rendering();
    s_nothing.w = w;
    s_nothing.h = h;
    s_nothing.color = 0x8;
    s_nothing.cls = C_8888;
    return &s_nothing;
}

static Zeta *zeta_for(const Nv2aTarget *t, const Surface *s)
{
    Zeta *z = NULL, *spare = NULL;
    static uint32_t serials;
    VkClearDepthStencilValue far_away = { 1.0f, 0 };
    VkImageSubresourceRange whole = { ZS_BITS, 0, 1, 0, 1 };
    Image old;
    VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t i, w = s->w, h = s->h, old_w = 0, old_h = 0, old_samples = 1;
    int alone = s == &s_nothing;

    if (!t->zeta_va)
        return NULL;
    for (i = 0; i < MAX_ZETAS; i++) {
        if (!s_zetas[i].used)
            spare = &s_zetas[i];
        else if (s_zetas[i].va == t->zeta_va)
            z = &s_zetas[i];
    }
    if (z && !alone && z->alone) {
        z->alone = 0;
        if (z->samples != samples_for(t, z->w, z->h)) {     /* its depths are lost */
            end_rendering();
            retire(&z->img, 0, 0);
            memset(z, 0, sizeof *z);
            spare = z;
            z = NULL;
        }
    }
    if (z && z->w >= w && z->h >= h) {
        z->last_use = g.frame;
        return z;
    }
    memset(&old, 0, sizeof old);
    end_rendering();
    begin_recording();
    if (z) {                            /* too small: keep what it holds */
        old = z->img;
        old_w = z->w;
        old_h = z->h;
        old_layout = z->layout;
        old_samples = z->samples;
        if (z->one.image)
            retire(&z->one, 0, 0);
        w = w > z->w ? w : z->w;
        h = h > z->h ? h : z->h;
    } else {
        if (!spare) {
            for (i = 0; i < MAX_ZETAS; i++)
                if (!spare || s_zetas[i].last_use < spare->last_use)
                    spare = &s_zetas[i];
            retire(&spare->img, 0, 0);
            if (spare->one.image)
                retire(&spare->one, 0, 0);
        }
        z = spare;
    }
    if (!zeta_image(z, old.image && old_samples > 1 ? old_samples
                       : alone ? 1 : samples_for(t, w, h), w, h)) {
        if (z->samples > 1)
            samples_off();
        if (!zeta_image(z, 1, w, h)) {
            if (old.image)
                retire(&old, 0, 0);
            memset(z, 0, sizeof *z);
            return NULL;
        }
    }
    if (old.image && old_samples != z->samples)     /* cannot be copied */
        retire(&old, 0, 0);
    if (!old.image) {
        z->serial = ++serials;
        z->version = 0;
        z->format.bytes = 0;                /* nothing in it yet */
        z->alone = alone;
        z->reciprocal = 0;
        z->fresh = 1.0f;
    }
    z->used = 1;
    z->va = t->zeta_va;
    z->w = w;
    z->h = h;
    z->last_use = g.frame;
    z->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (z->reciprocal)
        far_away.depth = 0.0f;
    to_layout(&z->img, ZS_BITS, &z->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearDepthStencilImage(g.cmd, z->img.image, z->layout, &far_away, 1, &whole);
    if (old.image) {
        VkImageCopy c = {0};

        to_layout(&old, ZS_BITS, &old_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        to_layout(&z->img, ZS_BITS, &z->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = ZS_BITS;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.extent.width = old_w * g.scale;
        c.extent.height = old_h * g.scale;
        c.extent.depth = 1;
        vkCmdCopyImage(g.cmd, old.image, old_layout, z->img.image, z->layout, 1, &c);
        retire(&old, 0, 0);
    }
    to_layout(&z->img, ZS_BITS, &z->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    return z;
}

/* One sample of each pixel's, in `one`. */
static int zeta_one(Zeta *z)
{
    VkRenderingAttachmentInfo za = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

    if (!z->one.image) {
        z->one_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!image_create(&z->one, VK_FORMAT_D32_SFLOAT_S8_UINT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          ZS_BITS, z->w * g.scale,
                          z->h * g.scale, 1, 1, 1, 0, "rgba"))
            return 0;
    }
    end_rendering();
    begin_recording();
    to_layout(&z->one, ZS_BITS, &z->one_layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    mb.srcStageMask = mb.dstStageMask = ALL_GRAPHICS;
    mb.srcAccessMask = ANY_WRITE;
    mb.dstAccessMask = ANY_ACCESS;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(g.cmd, &di);
    za.imageView = z->img.view;
    za.imageLayout = z->layout;
    za.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    za.resolveImageView = z->one.view;
    za.resolveImageLayout = z->one_layout;
    za.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    za.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = z->w * g.scale;
    ri.renderArea.extent.height = z->h * g.scale;
    ri.layerCount = 1;
    ri.pDepthAttachment = ri.pStencilAttachment = &za;
    vkCmdBeginRendering(g.cmd, &ri);
    vkCmdEndRendering(g.cmd);
    s_stats.resolved++;
    return 1;
}

/* What the batch is drawn with: the title's, if it has as many samples. */
static Zeta *zeta_used(void)
{
    return g.zeta && (g.zeta->samples > 1) == (g.many != 0) ? g.zeta : NULL;
}

/* ---- textures ----------------------------------------------------------- */

static VkFormat vk_format(Nv2aVkTexFormat f)
{
    switch (f) {
    case NV2A_VK_BC1:     return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case NV2A_VK_BC2:     return VK_FORMAT_BC2_UNORM_BLOCK;
    case NV2A_VK_BC3:     return VK_FORMAT_BC3_UNORM_BLOCK;
    case NV2A_VK_RGBA16S: return VK_FORMAT_R16G16B16A16_SNORM;
    case NV2A_VK_DEPTH:   return VK_FORMAT_R32_SFLOAT;
    default:              return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

static uint32_t texture_kind(const Nv2aVkTexInfo *i)
{
    return i->faces == 6 ? 3 : i->depth > 1 ? 2 : 1;
}

/* A place for the shaders to find it. */
static int texture_slot(Texture *t)
{
    t->kind = texture_kind(&t->info);
    if (!s_free_count[t->kind]) {
        retire(&t->img, 0, 0);
        return 0;
    }
    t->slot = s_free_slots[t->kind][--s_free_count[t->kind]];
    write_descriptor(t->kind, t->slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t->img.view,
                     VK_NULL_HANDLE);
    return 1;
}

static void part_copy(VkBufferImageCopy *c, const Nv2aVkTexInfo *i, uint32_t face,
                      uint32_t level, VkDeviceSize at)
{
    memset(c, 0, sizeof *c);
    c->bufferOffset = at;
    c->imageSubresource.aspectMask = COLOR_BIT;
    c->imageSubresource.mipLevel = level;
    c->imageSubresource.baseArrayLayer = face;
    c->imageSubresource.layerCount = 1;
    c->imageExtent.width  = i->width >> level ? i->width >> level : 1;
    c->imageExtent.height = i->height >> level ? i->height >> level : 1;
    c->imageExtent.depth  = i->depth >> level ? i->depth >> level : 1;
}

/* Into the upload command buffer, which runs before everything recorded since
 * the last flush: only for an image nothing recorded samples yet. */
static int texture_upload(Texture *t, uint32_t color, uint32_t pitch)
{
    const Nv2aVkTexInfo *i = &t->info;
    VkBufferImageCopy c[MAX_PARTS];
    VkDeviceSize at;
    uint8_t *map;
    uint32_t face, level, n = 0;

    begin_recording();
    map = stream(i->host_bytes, 16, &at);
    if (!map)
        return 0;
    if (!image_create(&t->img, vk_format(i->format),
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, COLOR_BIT,
                      i->width, i->height, i->depth, i->faces, i->levels,
                      i->faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, "rgba"))
        return 0;
    nv2a_vk_tex_decode(guest(t->offset),
                       t->palette ? (const uint32_t *)(void *)guest(t->palette) : NULL,
                       color, pitch, t->filter, i, map);
    for (face = 0; face < i->faces; face++)
        for (level = 0; level < i->levels; level++, n++)
            part_copy(&c[n], i, face, level, at + nv2a_vk_tex_level_offset(i, face, level));
    image_barrier(g.up, t->img.image, COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                  TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(g.up, g.stream, t->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, c);
    image_barrier(g.up, t->img.image, COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, TRANSFER,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    if (!texture_slot(t))
        return 0;
    s_stats.texture_uploads++;
    s_stats.texture_bytes += i->host_bytes;
    return 1;
}

static void texture_release(Texture *t)
{
    if (t->img.image)
        retire(&t->img, t->kind, t->slot);
    free(t->parts);
    t->parts = NULL;
}

static uint64_t texture_hash(const Texture *t)
{
    uint64_t h = nv2a_vk_hash(guest(t->offset), t->info.guest_bytes);

    s_stats.hashed_bytes += t->info.guest_bytes;
    if (t->palette)
        h ^= nv2a_vk_hash(guest(t->palette), 1024) * 3;
    return h;
}

/* The key's entry, made if there is none. */
static Texture *texture_entry(const Texture *key)
{
    uint64_t h = key->offset * 0x9E3779B97F4A7C15ull ^ key->format_word * 0xC2B2AE3D27D4EB4Full
               ^ key->palette * 0x165667B19E3779F9ull ^ ((uint64_t)key->width << 40)
               ^ key->pitch;
    uint32_t bucket = (uint32_t)(h >> 40) % TEX_BUCKETS;
    Texture *t;

    for (t = s_tex[bucket]; t; t = t->next)
        if (t->offset == key->offset && t->palette == key->palette
            && t->format_word == key->format_word && t->width == key->width
            && t->height == key->height && t->pitch == key->pitch
            && t->filter == key->filter && t->drawn == key->drawn)
            return t;
    t = (Texture *)malloc(sizeof *t);
    if (!t)
        return NULL;
    *t = *key;
    t->next = s_tex[bucket];
    s_tex[bucket] = t;
    return t;
}

/* ---- textures the GPU drew ---------------------------------------------- */

/* Part of a texture: the surface it is in, and where. */
typedef struct { Surface *s; uint32_t x, y, serial; } Source;

/* 1: found. 0: no surface has newer pixels than guest memory, or any. -1: one
 * does, and not in a way that can be copied on the card. */
static int source_find(uint32_t va, const Nv2aVkTexNative *n, uint32_t w, uint32_t h,
                       uint32_t pitch, Source *out)
{
    uint32_t i, bytes = n->swizzled ? w * h * n->bytes : pitch * (h - 1) + w * n->bytes;
    uint32_t dirty = 0;

    memset(out, 0, sizeof *out);
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surfaces[i];
        uint32_t d = va - s->va;

        if (!overlaps(s, va, bytes))
            continue;
        dirty |= (uint32_t)s->dirty;
        if (s->bpp != n->bytes || s->swizzled != (uint32_t)n->swizzled || va < s->va)
            continue;
        if (s->swizzled) {
            if (d || s->w != w || s->h != h)
                continue;
        } else if (s->pitch != pitch || d % pitch % s->bpp || d % pitch / s->bpp + w > s->w
                   || d / pitch + h > s->h) {
            continue;
        }
        if (out->s && !(s->w == w && s->h == h))
            continue;                   /* the exact one, if there is */
        out->s = s;
        out->x = s->swizzled ? 0 : d % pitch / s->bpp;
        out->y = s->swizzled ? 0 : d / pitch;
    }
    if (out->s) {
        out->s->last_use = g.frame;
        out->serial = out->s->serial;
        return 1;
    }
    if (!dirty)
        return 0;
    for (i = 0; i < MAX_SURFACES; i++) {        /* the same bytes, the other layout */
        Surface *s = &s_surfaces[i];
        uint32_t tight = n->swizzled ? w * n->bytes : pitch;

        if (s->used && s->dirty && s->va == va && s->bpp == n->bytes
            && s->bytes == tight * h && tight % s->bpp == 0
            && (s->swizzled || s->pitch == s->w * s->bpp)) {
            /* Making room can evict what is looked at here. */
            uint32_t color = s->color, serial = s->serial;

            out->s = surface_new(va, tight, color, (uint32_t)n->swizzled, tight / n->bytes, h);
            out->serial = out->s ? out->s->serial : 0;
            return out->s && s->used && s->serial == serial ? 1 : -1;
        }
    }
    return -1;
}

static void barrier_parts(Texture *t, VkImageLayout want)
{
    to_layout(&t->img, COLOR_BIT, &t->layout, want);
}

/* A texture made of surfaces, and of guest memory where none is: its own image,
 * in the title's texel format, brought up to date on the card. */
static int texture_compose(Texture *t, const Nv2aVkTexNative *n, uint32_t color,
                           const Source *src)
{
    const Nv2aVkTexInfo *i = &t->info;
    uint32_t face, level, p = 0, look = t->frame != g.frame;
    int moved = 0;

    if (!t->img.image) {
        if (!g.view_ok[n->view]
            || !image_create(&t->img, view_format(n->view),
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             COLOR_BIT, i->width * g.scale, i->height * g.scale, 1, i->faces,
                             i->levels, i->faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                             n->swizzle))
            return 0;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->parts = (Part *)calloc(i->faces * i->levels, sizeof *t->parts);
        if (!t->parts || !texture_slot(t)) {
            image_destroy(&t->img);
            return 0;
        }
    }
    t->frame = t->last_use = g.frame;
    for (face = 0; face < i->faces; face++)
        for (level = 0; level < i->levels; level++, p++) {
            Part *part = &t->parts[p];
            uint32_t w = i->width >> level ? i->width >> level : 1;
            uint32_t h = i->height >> level ? i->height >> level : 1;
            uint32_t to_w = (i->width * g.scale) >> level, to_h = (i->height * g.scale) >> level;
            Surface *s = src[p].s;

            to_w = to_w ? to_w : 1;         /* in the image, where a level one texel */
            to_h = to_h ? to_h : 1;         /* wide may be fewer than the scale */

            if (s) {
                VkImageCopy c = {0};

                if (part->serial == s->serial && part->version == s->version
                    && part->x == src[p].x && part->y == src[p].y)
                    continue;
                surface_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                end_rendering();
                begin_recording();
                barrier_parts(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = COLOR_BIT;
                c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
                c.dstSubresource.mipLevel = level;
                c.dstSubresource.baseArrayLayer = face;
                c.srcOffset.x = (int32_t)(src[p].x * g.scale);
                c.srcOffset.y = (int32_t)(src[p].y * g.scale);
                c.extent.width = to_w;
                c.extent.height = to_h;
                c.extent.depth = 1;
                vkCmdCopyImage(g.cmd, s->img.image, s->layout, t->img.image, t->layout, 1, &c);
                part->serial = s->serial;
                part->version = s->version;
                part->x = src[p].x;
                part->y = src[p].y;
                s_stats.card_copies++;
                moved = 1;
            } else if (look || part->serial) {
                const uint8_t *mem = guest(t->offset)
                                   + nv2a_vk_tex_guest_offset(i, color, face, level);
                uint32_t bytes = n->swizzled ? w * h * n->bytes
                                             : t->pitch * (h - 1) + w * n->bytes;
                uint64_t hash = nv2a_vk_hash(mem, bytes);
                VkDeviceSize room = (VkDeviceSize)w * h * n->bytes;
                VkBufferImageCopy c;
                VkDeviceSize at;
                uint8_t *map;

                s_stats.hashed_bytes += bytes;
                if (part->known && !part->serial && part->hash == hash)
                    continue;
                end_rendering();
                begin_recording();
                if (g.scale > 1)
                    room += (VkDeviceSize)to_w * to_h * n->bytes + 15;
                map = stream(room, 16, &at);
                if (!map)
                    continue;
                nv2a_vk_tex_raw(mem, n->bytes, n->swizzled, w, h, t->pitch, map);
                part_copy(&c, i, face, level, at);
                if (g.scale > 1) {              /* larger, texel by texel */
                    uint32_t x, y, from = w * h * n->bytes, to = (from + 15) & ~15u;

                    for (y = 0; y < to_h; y++)
                        for (x = 0; x < to_w; x++)
                            memcpy(map + to + (y * to_w + x) * n->bytes,
                                   map + (y * h / to_h * w + x * w / to_w) * n->bytes, n->bytes);
                    c.bufferOffset = at + to;
                    c.imageExtent.width = to_w;
                    c.imageExtent.height = to_h;
                }
                barrier_parts(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdCopyBufferToImage(g.cmd, g.stream, t->img.image, t->layout, 1, &c);
                part->hash = hash;
                part->known = 1;
                part->serial = 0;
                s_stats.texture_uploads++;
                s_stats.texture_bytes += w * h * n->bytes;
                moved = 1;
            }
        }
    if (moved)
        barrier_parts(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

/* What a stage samples. */
typedef struct {
    uint32_t kind, slot, width, height, levels;
    int linear, border;
    int reciprocal;                     /* depths, 1 over each */
} Sampled;

static void sampled_texture(Sampled *out, const Texture *t)
{
    out->kind = t->kind;
    out->slot = t->slot;
    out->width = t->info.width;
    out->height = t->info.height;
    out->levels = t->info.levels;
    out->linear = t->info.linear;
    out->border = t->info.border;
}

/* A view of it until what is recorded so far is done, and, to sample, its slot. */
static VkImageView view_for_now(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                uint32_t *slot)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkImageView view;
    Image v;

    if (slot && !s_free_count[1])
        return VK_NULL_HANDLE;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g.dev, &vi, NULL, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (slot) {
        *slot = s_free_slots[1][--s_free_count[1]];
        write_descriptor(1, *slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, VK_NULL_HANDLE);
    }
    memset(&v, 0, sizeof v);
    v.view = view;
    retire(&v, slot ? 1 : 0, slot ? *slot : 0);
    return view;
}

/* A texture of texels of one kind, out of another kind's: depths.frag, for which
 * the push constants are. */
static int texture_convert(Texture *t, int depths, const char *swizzle, const uint32_t push[5])
{
    static VkPipeline pipes[2];
    VkFormat format = depths ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_B8G8R8A8_UNORM;
    VkPipeline pipe = picture_pipeline(&pipes[depths], format, g.depths_frag, 1);
    const Nv2aVkTexInfo *i = &t->info;
    VkImageView view;

    if (!pipe)
        return 0;
    if (!t->img.image) {
        if (!image_create(&t->img, format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          COLOR_BIT, i->width * g.scale, i->height * g.scale, 1, 1, 1, 0,
                          swizzle))
            return 0;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!texture_slot(t)) {
            image_destroy(&t->img);
            return 0;
        }
    }
    view = view_for_now(t->img.image, format, COLOR_BIT, NULL);
    if (!view)
        return 0;
    to_layout(&t->img, COLOR_BIT, &t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    picture_into(view, i->width, i->height, pipe, push);
    to_layout(&t->img, COLOR_BIT, &t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s_stats.card_copies++;
    return 1;
}

/* Pixels that were drawn, as depths. */
static int texture_depths(const Texture *key, const Nv2aVkDepth *depth, const Source *src,
                          Sampled *out)
{
    Texture want = *key, *t;
    Surface *s = src->s;
    uint32_t push[5] = {0};
    uint64_t which = (uint64_t)s->serial << 32 | s->version;

    if (!surface_view(s, NV2A_VK_VIEW_8888, "rgba", &push[0]))
        return 0;
    want.drawn = 4;
    t = texture_entry(&want);
    if (!t)
        return 0;
    t->frame = t->last_use = g.frame;
    if (!t->img.image || t->hash != which) {
        end_rendering();
        begin_recording();
        surface_layout(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        push[1] = 1u | (depth->is_float ? 2u : 0u);
        push[2] = src->x * g.scale;
        push[3] = src->y * g.scale;
        if (!texture_convert(t, 1, "rgba", push))
            return 0;
        t->hash = which;
    }
    sampled_texture(out, t);
    s_stats.sampled++;
    return 1;
}

/* The texture is, or has in it, what the GPU drew, and the card can make it
 * without guest memory. */
static int texture_drawn(const Texture *key, uint32_t color, Sampled *out)
{
    const Nv2aVkTexInfo *i = &key->info;
    Source src[MAX_PARTS];
    Nv2aVkTexNative n = { NV2A_VK_VIEW_8888, "rgb", 4, 0 };
    Nv2aVkDepth depth;
    Texture *t, want;
    uint32_t face, level, p = 0, any = 0, slot;
    int depths = nv2a_vk_tex_depth(color, &depth);

    for (p = 0; p < MAX_SURFACES; p++)
        any |= (uint32_t)overlaps(&s_surfaces[p], key->offset, i->guest_bytes);
    n.swizzled = !i->linear;
    if (!any || i->depth > 1 || i->bordered || g.textures_via_ram
        || (depths ? depth.bytes != 4 || i->faces * i->levels != 1
                   : !nv2a_vk_tex_native(color, key->filter, &n)))
        return 0;
    any = p = 0;
    for (face = 0; face < i->faces; face++)
        for (level = 0; level < i->levels; level++, p++) {
            int found = source_find(key->offset
                                    + nv2a_vk_tex_guest_offset(i, color, face, level), &n,
                                    i->width >> level ? i->width >> level : 1,
                                    i->height >> level ? i->height >> level : 1,
                                    key->pitch, &src[p]);
            if (found < 0)
                return 0;
            any |= (uint32_t)found;
        }
    if (!any)
        return 0;
    for (any = 0; any < p; any++)       /* evicted to make room for another */
        if (src[any].s && src[any].s->serial != src[any].serial)
            return 0;
    while (p-- > 0)
        if (src[p].s)
            surface_current(src[p].s, 0);
    if (depths)
        return texture_depths(key, &depth, &src[0], out);

    /* One whole surface: its own image. Not while it is drawn into. */
    if (i->faces * i->levels == 1 && src[0].s != g.surf && src[0].s->w == i->width
        && src[0].s->h == i->height && surface_view(src[0].s, n.view, n.swizzle, &slot)) {
        surface_layout(src[0].s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        out->kind = 1;
        out->slot = slot;
        out->width = i->width;
        out->height = i->height;
        out->levels = 1;
        out->linear = i->linear;
        out->border = 0;
        s_stats.sampled++;
        return 1;
    }
    want = *key;
    want.drawn = 1;
    t = texture_entry(&want);
    if (!t || !texture_compose(t, &n, color, src))
        return 0;
    sampled_texture(out, t);
    s_stats.sampled++;
    return 1;
}

/* Its depths, or stencils, are not what they were. */
static void zeta_drawn(Zeta *z, const Nv2aVkDepth *format, const Nv2aTarget *t)
{
    z->version++;
    z->drawn = ++g.drawn;
    z->drawn_frame = g.frame;
    z->format = *format;
    z->pitch = t->zeta_pitch;
    z->swizzled = ((t->format >> 8) & 0xF) == 2;
}

/* Its depths and stencils as texels of 32 bits, of which a texture's image has the
 * top left: a picture of them in `t`. */
static int zeta_colours(Zeta *z, Image *from, VkImageLayout *layout, Texture *t,
                        const char *swizzle)
{
    uint32_t push[5] = {0};
    int done;

    push[1] = (z->format.is_float ? 2u : 0u) | (z->reciprocal ? 4u : 0u);
    end_rendering();
    begin_recording();
    if (!view_for_now(from->image, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT,
                      &push[0])
        || !view_for_now(from->image, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT,
                         &push[4]))
        return 0;
    to_layout(from, ZS_BITS, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    done = texture_convert(t, 0, swizzle, push);
    to_layout(from, ZS_BITS, layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    return done;
}

/* A texture of colours where a depth buffer was that long ago is the title's, put
 * there since. */
#define ZETA_FRAMES 60u

/* A texture that is a depth buffer: a copy of it, as the title may go on to draw
 * into the one it samples. */
static int texture_depth(const Texture *key, uint32_t color, Sampled *out)
{
    const Nv2aVkTexInfo *i = &key->info;
    Nv2aVkDepth format;
    Nv2aVkTexNative native;
    Texture want = *key, *t;
    VkImageCopy c = {0};
    Zeta *z = NULL;
    Image *from;
    VkImageLayout *layout;
    uint32_t n;
    int depth = nv2a_vk_tex_depth(color, &format), as_drawn;

    for (n = 0; n < MAX_ZETAS; n++)
        if (s_zetas[n].used && s_zetas[n].format.bytes && s_zetas[n].va == key->offset)
            z = &s_zetas[n];
    if (!z)
        return 0;
    for (n = 0; n < MAX_SURFACES; n++)  /* something else since */
        if (overlaps(&s_surfaces[n], key->offset, i->guest_bytes)
            && s_surfaces[n].drawn > z->drawn)
            return 0;
    as_drawn = z->swizzled != (uint32_t)i->linear && (!i->linear || z->pitch == key->pitch)
            && z->w >= i->width && z->h >= i->height && i->levels * i->faces * i->depth == 1
            && !i->bordered;
    if (!depth && g.frame - z->drawn_frame > ZETA_FRAMES)
        return 0;
    if (!as_drawn || (depth ? z->format.bytes != format.bytes
                              || z->format.is_float != format.is_float
                            : z->format.bytes != 4
                              || !nv2a_vk_tex_native(color, key->filter, &native)
                              || native.view != NV2A_VK_VIEW_8888)) {
        say_once(0x60000u | color, depth ? "depth buffer not sampled as drawn: format"
                                         : "depth buffer sampled as colours: format", color);
        return 0;
    }
    want.drawn = depth ? 2 : 3;
    t = texture_entry(&want);
    if (!t)
        return 0;
    if (depth && !t->img.image) {
        if (!image_create(&t->img, VK_FORMAT_D32_SFLOAT_S8_UINT,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT, i->width * g.scale, i->height * g.scale,
                          1, 1, 1, 0, "rgba"))
            return 0;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!texture_slot(t)) {
            image_destroy(&t->img);
            return 0;
        }
    }
    t->frame = t->last_use = g.frame;
    z->last_use = g.frame;
    if (!t->img.image || t->hash != ((uint64_t)z->serial << 32 | z->version)) {
        from = &z->img;
        layout = &z->layout;
        if (z->samples > 1) {
            if (!zeta_one(z))
                return 0;
            from = &z->one;
            layout = &z->one_layout;
        }
        t->hash = (uint64_t)z->serial << 32 | z->version;
        if (!depth) {
            if (!zeta_colours(z, from, layout, t, native.swizzle)) {
                t->hash = 0;
                return 0;
            }
            sampled_texture(out, t);
            s_stats.sampled++;
            return 1;
        }
        end_rendering();
        begin_recording();
        to_layout(from, ZS_BITS, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        to_layout(&t->img, ZS_BITS, &t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.extent.width = i->width * g.scale;
        c.extent.height = i->height * g.scale;
        c.extent.depth = 1;
        vkCmdCopyImage(g.cmd, from->image, *layout, t->img.image, t->layout, 1, &c);
        to_layout(&t->img, ZS_BITS, &t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        to_layout(from, ZS_BITS, layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        s_stats.card_copies++;
    }
    sampled_texture(out, t);
    out->reciprocal = z->reciprocal;
    s_stats.sampled++;
    return 1;
}

/* The texture a stage samples, its texels current; 0 if it has none. May
 * flush. */
static int texture_for(const Nv2aBatchTexture *b, uint32_t format_word, Sampled *out)
{
    Texture key, *t;
    uint64_t h;
    unsigned stored;

    if (!b->valid)
        return 0;
    memset(&key, 0, sizeof key);
    if (!nv2a_vk_tex_info(b, format_word, b->t.filter, &key.info)) {
        say_once(0x10000u | b->t.color, "texture not decoded, drawn white: format", b->t.color);
        return 0;
    }
    key.offset = b->t.offset;
    key.palette = b->t.color == 0x0B ? b->palette : 0;
    key.format_word = (format_word & ~3u)           /* less the DMA context */
                    | (key.info.border ? 1u : 0u);
    if (key.info.bordered && !key.info.border)
        say_once(10, "border texels not sampled: format", format_word);
    key.width = b->t.width;
    key.height = b->t.height;
    key.pitch = key.info.linear ? b->t.pitch : 0;
    key.filter = b->t.filter & 0xF0000000u;
    if (!guest_span_ok(key.offset, key.info.guest_bytes)
        || (key.palette && !guest_span_ok(key.palette, 1024))) {
        say_once(2, "texture outside guest memory", key.offset);
        return 0;
    }
    if (texture_depth(&key, b->t.color, out) || texture_drawn(&key, b->t.color, out))
        return 1;
    /* Drawn on the GPU, not in memory yet and nothing the card can copy. */
    title_owns(key.offset, key.info.guest_bytes, NULL);
    if (key.palette)
        memory_read(key.palette, 1024);
    stored = store_range(key.offset, key.info.guest_bytes, NULL);
    if (stored) {
        s_stats.mid_frame += stored;
        say_once(0x50000u | b->t.color, "drawn pixels sampled by way of guest memory:"
                 " texture format", b->t.color);
    }
    t = texture_entry(&key);
    if (!t)
        return 0;
    if (t->frame != g.frame) {
        double from = draw_ms();

        t->frame = g.frame;
        h = texture_hash(t);
        if (!t->img.image || h != t->hash) {
            int phase = nv2a_perf_phase(t->img.image ? nv2a_perf_phase_is
                                                     : NV2A_PERF_NEW_TEXTURES);

            texture_release(t);
            t->hash = h;
            if (!texture_upload(t, b->t.color, b->t.pitch)) {
                say_once(3, "no room for a texture", key.info.host_bytes);
                image_destroy(&t->img);
            }
            nv2a_perf_phase(phase);
        }
        s_stats.texture_ms += draw_ms() - from;
    }
    t->last_use = g.frame;
    if (!t->img.image)
        return 0;
    sampled_texture(out, t);
    return 1;
}

/* Textures not used for a while, once a frame. */
static void textures_trim(void)
{
    uint32_t i;

    for (i = 0; i < TEX_BUCKETS; i++) {
        Texture **p = &s_tex[i];
        while (*p) {
            Texture *t = *p;
            if (t->last_use + 120 < g.frame) {
                *p = t->next;
                texture_release(t);
                free(t);
            } else {
                p = &t->next;
            }
        }
    }
}

static VkSamplerAddressMode wrap(uint32_t mode)
{
    switch (mode & 0xF) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

/* SET_TEXTURE_ADDRESS, _CONTROL0, _FILTER and the border colour, as one of the
 * samplers the shader indexes. */
static uint32_t sampler_for(uint32_t address, uint32_t control0, uint32_t filter,
                            uint32_t border, int border_color, uint32_t levels)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSamplerCustomBorderColorCreateInfoEXT bc =
        { VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT };
    uint32_t min = (filter >> 16) & 0xFF, mag = (filter >> 24) & 0xF, i;
    uint32_t key[4], aniso = 1u << ((control0 >> 4) & 3), least = (uint32_t)s_aniso;
    float bias;

    /* Without a border colour BORDER would read the texture's own border
     * texels, which are not kept: clamp, as the CPU rasteriser does. */
    if (!border_color) {
        for (i = 0; i < 24; i += 8)
            if (((address >> i) & 0xF) == 4)
                address = (address & ~(0xFu << i)) | (3u << i);
        border = 0;
    }
    /* Not what is drawn texel for texel, or compared as a depth. */
    if (levels > 1 && mag != 1 && (min == 4 || min == 6) && (control0 & 0x3FFC0u)
        && least > aniso)
        aniso = least;
    key[0] = address & 0x000F0F0Fu;
    key[1] = (control0 & 0x3FFFFFC0u) | aniso;
    key[2] = (filter & 0x0FFF1FFFu) | (levels << 28);
    key[3] = border;
    for (i = 0; i < s_sampler_count; i++)
        if (!memcmp(s_samplers[i].key, key, sizeof key))
            return i;
    if (s_sampler_count == SAMPLERS) {
        say_once(4, "too many samplers", SAMPLERS);
        return 0;
    }
    ci.addressModeU = wrap(address);
    ci.addressModeV = wrap(address >> 8);
    ci.addressModeW = wrap(address >> 16);
    ci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = min == 1 || min == 3 || min == 5 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = min == 5 || min == 6 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                         : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    /* 4.8 fixed point, the bias signed. */
    bias = (float)((int32_t)(filter << 19) >> 19) / 256.0f;
    if (bias > g.props.limits.maxSamplerLodBias)
        bias = g.props.limits.maxSamplerLodBias;
    if (bias < -g.props.limits.maxSamplerLodBias)
        bias = -g.props.limits.maxSamplerLodBias;
    ci.mipLodBias = bias;
    if (min == 1 || min == 2 || levels < 2) {
        /* No mip levels, but both filters: Vulkan magnifies at LOD 0. */
        ci.minLod = 0.0f;
        ci.maxLod = 0.25f;
    } else {
        ci.minLod = (float)((control0 >> 18) & 0xFFF) / 256.0f;
        ci.maxLod = (float)((control0 >> 6) & 0xFFF) / 256.0f;
        if (ci.maxLod > (float)(levels - 1))
            ci.maxLod = (float)(levels - 1);
        if (ci.minLod > ci.maxLod)
            ci.minLod = ci.maxLod;
    }
    if (g.has_aniso && aniso > 1 && ci.minFilter == VK_FILTER_LINEAR) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = (float)aniso;
        if (ci.maxAnisotropy > g.props.limits.maxSamplerAnisotropy)
            ci.maxAnisotropy = g.props.limits.maxSamplerAnisotropy;
    }
    ci.borderColor = (border >> 24) ? ((border & 0xFFFFFF)
                                       ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                       : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                    : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (border_color && g.has_border) {
        bc.customBorderColor.float32[0] = (float)((border >> 16) & 0xFF) / 255.0f;
        bc.customBorderColor.float32[1] = (float)((border >> 8) & 0xFF) / 255.0f;
        bc.customBorderColor.float32[2] = (float)(border & 0xFF) / 255.0f;
        bc.customBorderColor.float32[3] = (float)(border >> 24) / 255.0f;
        bc.format = VK_FORMAT_UNDEFINED;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        ci.pNext = &bc;
    }
    if (vkCreateSampler(g.dev, &ci, NULL, &s_samplers[i].sampler) != VK_SUCCESS) {
        say_once(5, "cannot make a sampler", filter);
        return 0;
    }
    memcpy(s_samplers[i].key, key, sizeof key);
    write_descriptor(4, i, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, s_samplers[i].sampler);
    return s_sampler_count++;
}

/* ---- pipelines ---------------------------------------------------------- */

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

/* A surface with no alpha reads as 1. */
static VkBlendFactor without_alpha(VkBlendFactor f)
{
    return f == VK_BLEND_FACTOR_DST_ALPHA ? VK_BLEND_FACTOR_ONE
         : f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
           || f == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ? VK_BLEND_FACTOR_ZERO : f;
}

static VkBlendOp blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_op(uint32_t f)
{
    switch (f) {
    case 0x200: return VK_COMPARE_OP_NEVER;
    case 0x201: return VK_COMPARE_OP_LESS;
    case 0x202: return VK_COMPARE_OP_EQUAL;
    case 0x203: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 0x204: return VK_COMPARE_OP_GREATER;
    case 0x205: return VK_COMPARE_OP_NOT_EQUAL;
    case 0x206: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default:    return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

enum { TOPO_TRIANGLES, TOPO_LINES, TOPO_POINTS };

/* What Vulkan bakes into a pipeline, as a key. From bit KEY_FILL on, how it fills
 * triangles (fill_mode). */
#define KEY_FILL 30
#define KEY_SMOOTH (1ull << 32)                 /* lines, by how much is covered */
#define KEY_LOGIC  33                           /* one bit, and four of VkLogicOp */
#define KEY_DEPTH_REPLACE (1ull << 38)          /* the fragment's program sets it */
#define KEY_SAMPLES 39                          /* two bits: log2 of them */
#define KEY_CUTOUT (1ull << 41)                 /* alpha says which are covered */
#define KEY_EACH   (1ull << 42)                 /* the fragment's program runs for each */
#define KEY_NOTHING (1ull << 43)                /* no colour surface */
#define KEY_RECIPROCAL (1ull << 44)             /* the depths drawn are 1 over the title's */
static uint64_t pipeline_key(uint32_t topo, uint32_t mask, int blend, uint32_t src,
                             uint32_t dst, uint32_t eq, int zeta, int depth_clamp,
                             int program, uint32_t cls, int two_sided)
{
    uint64_t k = 1u | topo << 1 | (zeta ? 1u << 3 : 0u) | (depth_clamp ? 1u << 4 : 0u)
               | (program ? 1u << 25 : 0u) | cls << 26 | (two_sided ? 1u << 29 : 0u);

    if (blue_first(cls))
        mask = (mask & ~5u) | (mask & 4u) >> 2 | (mask & 1u) << 2;
    if (cls == C_1555)                          /* the bit is not alpha's */
        mask &= 7u;
    k |= (uint64_t)mask << 5;                   /* 4 bits */
    if (blend) {
        VkBlendFactor from = blend_factor(src), to = blend_factor(dst);

        if (cls == C_1555) {
            from = without_alpha(from);
            to = without_alpha(to);
        }
        k |= 1ull << 9 | (uint64_t)from << 10 | (uint64_t)to << 16
           | (uint64_t)blend_op(eq) << 22;
    }
    return k;
}

#define FLAG_RECIPROCAL (1u << 24)
#define FLAG_BY_W       (1u << 25)
#define FLAG_RANGE      (1u << 26)

/* The state's words, in the order of state.glsl's constants; 0 where they decide
 * nothing, so that fewer differ. */
static void specialized_words(const GpuDraw *d, uint32_t *w)
{
    uint32_t i, n = d->control & 0xF, planes = 0, earlier = 0, vectors = 0, depths = 0;

    memset(w, 0, SPECIALIZED_WORDS * sizeof *w);
    for (i = 0; i < 4; i++) {
        uint32_t mode = (d->stage_program >> (5 * i)) & 0x1F;

        planes |= mode == 5;
        earlier |= mode == 6 || mode == 7 || (mode >= 9 && mode <= 18);
        vectors |= (mode >= 9 && mode <= 14) || mode == 17 || mode == 18;
        w[42 + i] = d->stage[i].kind;
        w[46 + i] = d->stage[i].flags;
        depths |= d->stage[i].flags & 4;
    }
    w[1] = d->stage_program & 0xFFFFF;
    w[2] = earlier ? d->other_stage & 0xFF0000 : 0;
    w[3] = planes ? d->clip_plane & 0xFFFF : 0;
    w[4] = d->flags & 0xFEFD;               /* the fragment's program's bits */
    for (i = 0; i < 20; i += 5)
        if (((d->stage_program >> i) & 0x1F) == 10)
            w[4] |= d->flags & FLAG_RECIPROCAL;
    w[4] |= d->flags & FLAG_RANGE;
    w[5] = d->alpha_func;
    w[8] = vectors ? d->dot_mapping & 0x777 : 0;
    w[9] = depths ? d->shadow_func & 7 : 0;
    if (!(d->flags & 1))
        return;
    if (n > 8)
        n = 8;
    w[0] = n | (d->control & 0x11100);
    w[6] = d->final0;
    w[7] = d->final1;
    memcpy(&w[10], d->color_icw, n * 4);
    memcpy(&w[18], d->alpha_icw, n * 4);
    memcpy(&w[26], d->color_ocw, n * 4);
    memcpy(&w[34], d->alpha_ocw, n * 4);
}

/* With the state's words, or for any state: all of it (PART_ALL), what shades the
 * fragments or what else there is, with the part that shades them. Any thread. */
enum { PART_ALL = 0, PART_FRAGMENT, PART_REST };
static VkPipeline pipeline_part(uint64_t k, const uint32_t *words, int part, VkPipeline with)
{
    static const VkPrimitiveTopology topologies[3] = {
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST
    };
    static const VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH
    };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb = { 0, sizeof(Nv2aVshOutput),
                                           VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[11];
    VkPipelineVertexInputStateCreateInfo vi =
        { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia =
        { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp =
        { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs =
        { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineRasterizationLineStateCreateInfoEXT ls =
        { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT };
    VkPipelineMultisampleStateCreateInfo ms =
        { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds =
        { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState at;
    VkPipelineColorBlendStateCreateInfo cb =
        { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy =
        { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineLibraryCreateInfoEXT lib =
        { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT };
    VkPipelineLibraryCreateInfoKHR use = { VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline out = VK_NULL_HANDLE;
    uint32_t constants[2 + SPECIALIZED_WORDS];
    VkSpecializationMapEntry entry[2 + SPECIALIZED_WORDS];
    VkSpecializationInfo si = { words ? 2 + SPECIALIZED_WORDS : 2, entry, sizeof constants,
                                constants };
    uint32_t i;

    constants[0] = (k >> 29) & 1;           /* two-sided */
    constants[1] = words != NULL;
    if (words)
        memcpy(&constants[2], words, SPECIALIZED_WORDS * sizeof *words);
    for (i = 0; i < si.mapEntryCount; i++) {
        entry[i].constantID = i;
        entry[i].offset = i * 4;
        entry[i].size = 4;
    }
    memset(st, 0, sizeof st);
    for (i = 0; i < 2; i++) {
        st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        st[i].module = i ? (k & KEY_EACH ? k & KEY_DEPTH_REPLACE ? g.each_depth_frag : g.each_frag
                            : k & KEY_DEPTH_REPLACE ? g.depth_frag
                            : k & KEY_CUTOUT ? g.cutout_frag : g.frag)
                         : k & KEY_RECIPROCAL ? (k >> 25) & 1 ? g.w_prog : g.w_vert
                         : (k >> 25) & 1 ? g.prog : g.vert;
        st[i].pName = "main";
        st[i].pSpecializationInfo = &si;
    }
    for (i = 0; i < 11; i++) {
        va[i].location = i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = i * 16;
    }
    if (!((k >> 25) & 1)) {             /* program.vert reads the buffer itself */
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 11;
        vi.pVertexAttributeDescriptions = va;
    }
    ia.topology = topologies[(k >> 1) & 3];
    vp.viewportCount = vp.scissorCount = 1;
    rs.depthClampEnable = (k >> 4) & 1;
    rs.polygonMode = (VkPolygonMode)((k >> KEY_FILL) & 3);
    if (k & KEY_SMOOTH) {
        ls.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
        rs.pNext = &ls;
    }
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(1u << ((k >> KEY_SAMPLES) & 3));
    ms.sampleShadingEnable = (k & KEY_EACH) != 0;
    ms.minSampleShading = 1.0f;
    memset(&at, 0, sizeof at);
    at.colorWriteMask = (VkColorComponentFlags)((k >> 5) & 0xF);
    if ((k >> 9) & 1) {
        at.blendEnable = VK_TRUE;
        at.srcColorBlendFactor = at.srcAlphaBlendFactor = (VkBlendFactor)((k >> 10) & 0x3F);
        at.dstColorBlendFactor = at.dstAlphaBlendFactor = (VkBlendFactor)((k >> 16) & 0x3F);
        at.colorBlendOp = at.alphaBlendOp = (VkBlendOp)((k >> 22) & 7);
    }
    cb.attachmentCount = (k & KEY_NOTHING) == 0;
    cb.pAttachments = &at;
    cb.logicOpEnable = (k >> KEY_LOGIC) & 1;
    cb.logicOp = (VkLogicOp)((k >> (KEY_LOGIC + 1)) & 0xF);
    dy.dynamicStateCount = sizeof dynamic / sizeof dynamic[0];
    dy.pDynamicStates = dynamic;
    rc.colorAttachmentCount = (k & KEY_NOTHING) == 0;
    rc.pColorAttachmentFormats = &s_classes[(k >> 26) & 7].format;
    if ((k >> 3) & 1)
        rc.depthAttachmentFormat = rc.stencilAttachmentFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    ci.pNext = &rc;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = g.layout;
    if (part != PART_ALL) {
        lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
        rc.pNext = &lib;
        ci.stageCount = 1;
        if (part == PART_FRAGMENT) {
            ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            ci.pStages = &st[1];
        } else {
            lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
                      | VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                      | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            lib.pNext = &use;
            use.libraryCount = 1;
            use.pLibraries = &with;
        }
    }
    if (vkCreateGraphicsPipelines(g.dev, g.cache, 1, &ci, NULL, &out) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return out;
}

/* Those that read the state differ in little a fragment's program sees: they share
 * it, where the card can, as it takes half a second to compile. */
static VkPipeline pipeline_any(uint64_t k)
{
    static Pipeline shared[128];
    uint64_t of = k & (1ull << 29 | 3ull << KEY_SAMPLES | KEY_DEPTH_REPLACE | KEY_CUTOUT
                       | KEY_EACH);
    VkPipeline out;
    uint32_t i;

    if (!g.has_parts)
        return pipeline_part(k, NULL, PART_ALL, VK_NULL_HANDLE);
    for (i = 0; shared[i].pipeline && shared[i].key != of; i++)
        ;                               /* there are fewer of them */
    if (!shared[i].pipeline) {
        shared[i].pipeline = pipeline_part(of | 1, NULL, PART_FRAGMENT, VK_NULL_HANDLE);
        shared[i].key = of;
    }
    out = shared[i].pipeline ? pipeline_part(k, NULL, PART_REST, shared[i].pipeline)
                             : VK_NULL_HANDLE;
    return out ? out : pipeline_part(k, NULL, PART_ALL, VK_NULL_HANDLE);
}

static void specialized_make(Specialized *s)
{
    s->pipeline = pipeline_part(s->key, s->words, PART_ALL, VK_NULL_HANDLE);
    InterlockedExchange(&s->state, s->pipeline ? SPECIALIZED_READY : SPECIALIZED_FAILED);
    InterlockedIncrement(&s_specialized_done);
}

/* The card's compiled pipelines, from the last time. */
static void cache_open(void)
{
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    Nv2aVkPipelineCacheOf of = { g.props.vendorID, g.props.deviceID, { 0 } };
    void *data;

    memcpy(of.uuid, g.props.pipelineCacheUUID, sizeof of.uuid);
    data = nv2a_vk_pipeline_cache_read(nv2a_vk_pipeline_cache_path(), &of, &ci.initialDataSize);
    ci.pInitialData = data;
    if (vkCreatePipelineCache(g.dev, &ci, NULL, &g.cache) != VK_SUCCESS)
        g.cache = VK_NULL_HANDLE;
    free(data);
}

/* And for the next. */
static void cache_save(void)
{
    size_t n = 0;
    void *data;

    if (!g.cache || !*nv2a_vk_pipeline_cache_path()
        || vkGetPipelineCacheData(g.dev, g.cache, &n, NULL) != VK_SUCCESS)
        return;
    n += 1u << 20;                      /* others go on adding to it */
    data = malloc(n);
    if (data && vkGetPipelineCacheData(g.dev, g.cache, &n, data) == VK_SUCCESS
        && !nv2a_vk_pipeline_cache_write(nv2a_vk_pipeline_cache_path(), data, n))
        fprintf(stderr, "[NV2A-VK] cannot save %s\n", nv2a_vk_pipeline_cache_path());
    free(data);
}

#define CACHE_QUIET_MS 3000             /* nothing compiled for so long: saved */
static volatile LONG s_cache_new;

static DWORD WINAPI specialized_thread(LPVOID unused)
{
    static SRWLOCK saving = SRWLOCK_INIT;

    (void)unused;
    for (;;) {                          /* one for each */
        if (WaitForSingleObject(s_specialized_ready, CACHE_QUIET_MS) == WAIT_OBJECT_0) {
            specialized_make(s_specialized_list[InterlockedIncrement(&s_specialized_taken) - 1]);
            InterlockedExchange(&s_cache_new, 1);
        } else if (s_specialized_done == s_specialized_count
                   && TryAcquireSRWLockExclusive(&saving)) {
            if (InterlockedExchange(&s_cache_new, 0))
                cache_save();
            ReleaseSRWLockExclusive(&saving);
        }
    }
}

/* A quarter of the cores, behind everything else. 0: there are no threads. */
static int specialized_start(void)
{
    static int tried, running;
    SYSTEM_INFO si;
    uint32_t i;

    if (tried)
        return running;
    tried = 1;
    s_specialized_ready = CreateSemaphoreA(NULL, 0, MAX_SPECIALIZED, NULL);
    if (!s_specialized_ready)
        return 0;
    GetSystemInfo(&si);
    for (i = 0; i == 0 || i < si.dwNumberOfProcessors / 4; i++) {
        HANDLE th = CreateThread(NULL, 0, specialized_thread, NULL, 0, NULL);

        if (!th)
            break;
        SetThreadPriority(th, THREAD_PRIORITY_LOWEST);
        CloseHandle(th);
        running++;
    }
    if (!running)
        say_once(17, "no thread to compile with", 0);
    return running;
}

/* 0: not compiled yet. */
static VkPipeline specialized_for(uint64_t k, const GpuDraw *d, int wait)
{
    uint32_t w[SPECIALIZED_WORDS], i, n;
    uint64_t hash = k;
    Specialized *s;

    specialized_words(d, w);
    for (i = 0; i < SPECIALIZED_WORDS; i++) {
        hash = (hash ^ w[i]) * 0x9E3779B97F4A7C15ull;
        hash ^= hash >> 29;
    }
    hash |= 1;
    i = (uint32_t)(hash >> 40) % MAX_SPECIALIZED;
    for (n = 0; n < MAX_SPECIALIZED; n++, i = (i + 1) % MAX_SPECIALIZED) {
        s = &s_specialized[i];
        if (!s->hash)
            break;
        if (s->hash == hash && s->key == k && !memcmp(s->words, w, sizeof w))
            break;
    }
    if (n == MAX_SPECIALIZED) {
        say_once(18, "too many pipelines of a state's own", MAX_SPECIALIZED);
        return VK_NULL_HANDLE;
    }
    if (!s->hash) {
        if (!wait && !specialized_start())
            return VK_NULL_HANDLE;
        s->hash = hash;
        s->key = k;
        memcpy(s->words, w, sizeof w);
        if (wait) {
            int phase = nv2a_perf_phase(NV2A_PERF_PIPELINES);

            InterlockedIncrement(&s_specialized_count);
            specialized_make(s);
            nv2a_perf_phase(phase);
        } else {
            s_specialized_list[s_specialized_queued++] = s;
            InterlockedIncrement(&s_specialized_count);
            ReleaseSemaphore(s_specialized_ready, 1, NULL);
        }
    }
    while (wait && s->state == SPECIALIZED_DUE)
        Sleep(1);
    if (s->state == SPECIALIZED_FAILED)
        say_once(19, "cannot make a pipeline of a state's own", (uint32_t)k);
    return s->state == SPECIALIZED_READY ? s->pipeline : VK_NULL_HANDLE;
}

/* One that reads the state. */
static VkPipeline any_pipeline(uint64_t k)
{
    uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 54) % PIPELINES, n;

    for (n = 0; n < PIPELINES; n++, i = (i + 1) % PIPELINES) {
        if (s_pipes[i].key == k)
            return s_pipes[i].pipeline;
        if (!s_pipes[i].key) {
            int phase = nv2a_perf_phase(NV2A_PERF_PIPELINES);

            s_pipes[i].pipeline = pipeline_any(k);
            nv2a_perf_phase(phase);
            if (s_pipes[i].pipeline)
                s_pipes[i].key = k;
            else
                say_once(6, "cannot make a pipeline", (uint32_t)k);
            return s_pipes[i].pipeline;
        }
    }
    say_once(7, "too many pipelines", PIPELINES);
    return VK_NULL_HANDLE;
}

static VkPipeline pipeline_for(uint64_t k, const GpuDraw *d)
{
    LONG how = s_shader_compile;
    VkPipeline p = how ? specialized_for(k, d, how == NV2A_VK_COMPILE_SYNC) : VK_NULL_HANDLE;

    if (p)
        s_stats.specialized_draws++;
    else
        p = any_pipeline(k);
    return p;
}

void nv2a_vk_shader_compile(Nv2aVkShaderCompile how)
{
    if ((int)how >= 0 && how < NV2A_VK_COMPILE_MODES)
        InterlockedExchange(&s_shader_compile, how);
}

static void shader_compile_named(const char *e)
{
    static const char *const names[NV2A_VK_COMPILE_MODES] = { "off", "async", "sync" };
    int i;

    for (i = 0; i < NV2A_VK_COMPILE_MODES; i++)
        if (strcmp(e, names[i]) == 0) {
            nv2a_vk_shader_compile((Nv2aVkShaderCompile)i);
            return;
        }
    fprintf(stderr, "[NV2A-VK] no shader compile %s: off, async or sync\n", e);
}

/* ---- drawing ------------------------------------------------------------ */

/* Open rendering on the current surfaces, if it is not. */
static void begin_rendering(void)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo za = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    VkViewport vp;
    VkDeviceSize zero = 0;

    begin_recording();
    if (g.rendering && g.bound_surf == g.surf && g.bound_zeta == g.zeta
        && g.bound_many == g.many)
        return;
    end_rendering();
    if (g.surf == &s_nothing) {
        ;
    } else if (g.many) {
        if (g.surf->newest == IN_ONE && !surface_spread(g.surf))
            say_once(14, "cannot spread a surface over its samples", g.surf->cls);
        to_layout(&g.surf->many, COLOR_BIT, &g.surf->many_layout,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        g.surf->newest = IN_MANY;
    } else {
        surface_layout(g.surf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    /* Rendering that follows rendering on the same images. */
    mb.srcStageMask = mb.dstStageMask = ALL_GRAPHICS;
    mb.srcAccessMask = ANY_WRITE;
    mb.dstAccessMask = ANY_ACCESS;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(g.cmd, &di);
    ca.imageView = g.many ? g.surf->many.view : g.surf->img.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = g.surf->w * g.scale;
    ri.renderArea.extent.height = g.surf->h * g.scale;
    ri.layerCount = 1;
    ri.colorAttachmentCount = g.surf != &s_nothing;
    ri.pColorAttachments = &ca;
    if (zeta_used()) {
        za.imageView = g.zeta->img.view;
        za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        za.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        za.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = ri.pStencilAttachment = &za;
    }
    vkCmdBeginRendering(g.cmd, &ri);
    g.rendering = 1;
    g.bound_surf = g.surf;
    g.bound_zeta = g.zeta;
    g.bound_many = g.many;
    g.bound_pipe = VK_NULL_HANDLE;
    memset(&vp, 0, sizeof vp);
    vp.width = (float)(g.surf->w * g.scale);
    vp.height = (float)(g.surf->h * g.scale);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetLineWidth(g.cmd, 1.0f);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set,
                            0, NULL);
    vkCmdBindVertexBuffers(g.cmd, 0, 1, &g.stream, &zero);
}

/* ---- memory the title freed --------------------------------------------- */

static struct { uint32_t va, bytes; } s_freed[256];
static volatile LONG s_freed_count;
static SRWLOCK s_freed_lock = SRWLOCK_INIT;

void nv2a_vk_memory_freed(uint32_t va, uint32_t bytes)
{
    answers_freed(va, bytes);
    AcquireSRWLockExclusive(&s_freed_lock);
    if (s_freed_count < (LONG)(sizeof s_freed / sizeof s_freed[0])) {
        s_freed[s_freed_count].va = va;
        s_freed[s_freed_count].bytes = bytes;
        s_freed_count++;
    }
    ReleaseSRWLockExclusive(&s_freed_lock);
}

/* What was drawn there is nobody's: not to be stored over what comes next. */
static void freed_surfaces(void)
{
    LONG n;
    uint32_t i;

    if (!s_freed_count)
        return;
    AcquireSRWLockExclusive(&s_freed_lock);
    for (n = 0; n < s_freed_count; n++)
        for (i = 0; i < MAX_SURFACES; i++) {
            Surface *s = &s_surfaces[i];

            if (overlaps(s, s_freed[n].va, s_freed[n].bytes)) {
                s->dirty = 0;
                surface_drop(s);
            }
        }
    s_freed_count = 0;
    ReleaseSRWLockExclusive(&s_freed_lock);
}

static float as_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* The current surfaces. 0: cannot draw here. */
static int bind_target(const Nv2aTarget *t)
{
    Surface *s;

    freed_surfaces();
    s = t->color_va ? surface_for(t) : nothing_for(t);
    if (s) {
        g.surf = s;
        g.zeta = zeta_for(t, s);
    }
    if (!s || (s == &s_nothing && !g.zeta)) {
        say_once(0x20000u | (t->format & 0xFFF), "surface not drawn into: format", t->format);
        g.surf = NULL;
        return 0;
    }
    g.many = 0;
    return 1;
}

/* Between one sample and many, so often in a frame; then it stays with many. */
#define TURNS 8u

/* After bind_target(): whether to draw with several samples. What is in depth,
 * or has the same edges as the depths: the title's pictures keep their pixels.
 * Rendering from `fresh` on needs none of what is there. */
static void choose_many(int in_depth, int fresh)
{
    Surface *s = g.surf;

    g.many = 0;
    if (!g.zeta || g.zeta->samples == 1)
        return;
    if (s == &s_nothing) {
        g.many = 1;
        return;
    }
    if (s->turn_frame != g.frame) {
        s->turn_frame = g.frame;
        s->turns = 0;
    }
    if (!in_depth && (s->newest != IN_MANY || s->turns < TURNS))
        return;
    if (!surface_many(s)) {
        samples_off();
        return;
    }
    g.many = 1;
    if (s->newest == IN_ONE) {
        s->turns++;
        if (fresh)
            s->newest = IN_BOTH;        /* nothing to spread */
    }
}

/* Whether depths or stencils decide what is drawn, or are. */
static int in_depth(const Nv2aRenderState *r)
{
    return r->stencil_enable
        || (r->depth_test_enable && (r->depth_write || r->depth_func != 0x207));
}

/* SET_SURFACE_FORMAT and SET_CONTROL0's Z_FORMAT: the title's depths are in its
 * buffer's own units, and the image holds them over the largest. Below 4, the
 * 24-bit floating-point ones are all 0 in the image. */
static int depth_is_float(const uint32_t *regs) { return (regs[0x0290 / 4] >> 12) & 1; }

static Nv2aVkDepth depth_of(const Nv2aTarget *t, const uint32_t *regs)
{
    Nv2aVkDepth d = { ((t->format >> 4) & 0xF) == 1 ? 2 : 4, regs && depth_is_float(regs) };
    return d;
}

/* SET_CONTROL0's Z_PERSPECTIVE_ENABLE: a w-buffer, the depth is the position's w. */
static int by_w(const uint32_t *regs) { return (regs[0x0290 / 4] >> 16) & 1; }

/* Depths over the largest as the image has them. 1 over each where the title draws
 * a w-buffer: 1 / w is what the GPU interpolates, and what is near is precise
 * enough in a float; the comparisons turn. Below 1 a depth is 1. */
static float zeta_holds(const Zeta *z, const Nv2aVkDepth *d, float depth)
{
    double v = depth * nv2a_vk_depth_most(d);

    return z->reciprocal ? (float)(1.0 / (v < 1.0 ? 1.0 : v)) : depth;
}

/* Before a batch that is drawn in depth: the first one after a clear says what its
 * depths are like. */
static void zeta_for_batch(const Nv2aRawBatch *b)
{
    const Nv2aRenderState *r = b->state;
    Nv2aVkDepth depth = depth_of(b->target, b->regs);
    Zeta *z = zeta_used();
    int w = by_w(b->regs);

    if (!z || !r->depth_test_enable || (!r->depth_write && r->depth_func == 0x207))
        return;
    if (z->reciprocal != w && (z->fresh >= 0.0f || w)) {
        VkClearAttachment at;
        VkClearRect cr;

        if (z->fresh < 0.0f)
            say_once(15, "a w-buffer where other depths are, which are lost", b->target->zeta_va);
        z->reciprocal = w;
        memset(&at, 0, sizeof at);
        memset(&cr, 0, sizeof cr);
        at.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        at.clearValue.depthStencil.depth = zeta_holds(z, &depth, z->fresh < 0.0f ? 1.0f : z->fresh);
        cr.rect.extent.width = g.surf->w * g.scale;
        cr.rect.extent.height = g.surf->h * g.scale;
        cr.layerCount = 1;
        begin_rendering();
        vkCmdClearAttachments(g.cmd, 1, &at, 1, &cr);
        z->version++;
    }
    if (r->depth_write)
        z->fresh = -1.0f;
}

static void clip_rect(const Nv2aTarget *t, const uint32_t *regs, VkRect2D *out)
{
    uint32_t x0 = t->clip_x, y0 = t->clip_y, x1 = x0 + t->clip_w, y1 = y0 + t->clip_h;

    /* SET_WINDOW_CLIP_*: up to eight rectangles, inclusive, drawing inside
     * their union (type 0). Their bounding box here, which is exact for one.
     * Scaled like the surface clip, in logical pixels: no title with an
     * anti-aliased surface has shown whether that is right. */
    if (regs && !(regs[0x02B4 / 4] & 1)) {
        uint32_t i, l = UINT32_MAX, r = 0, u = UINT32_MAX, d = 0;
        for (i = 0; i < 8; i++) {
            uint32_t h = regs[0x02C0 / 4 + i], v = regs[0x02E0 / 4 + i];
            if (!h && !v)
                continue;
            if ((h & 0xFFF) < l) l = h & 0xFFF;
            if (((h >> 16) & 0xFFF) + 1 > r) r = ((h >> 16) & 0xFFF) + 1;
            if ((v & 0xFFF) < u) u = v & 0xFFF;
            if (((v >> 16) & 0xFFF) + 1 > d) d = ((v >> 16) & 0xFFF) + 1;
        }
        if (r && d) {
            l *= t->aa_sx; r *= t->aa_sx;
            u *= t->aa_sy; d *= t->aa_sy;
            if (l > x0) x0 = l;
            if (u > y0) y0 = u;
            if (r < x1) x1 = r;
            if (d < y1) y1 = d;
        }
    }
    if (x1 > g.surf->w) x1 = g.surf->w;
    if (y1 > g.surf->h) y1 = g.surf->h;
    if (x0 > x1) x0 = x1;
    if (y0 > y1) y0 = y1;
    out->offset.x = (int32_t)x0;
    out->offset.y = (int32_t)y0;
    out->extent.width = x1 - x0;
    out->extent.height = y1 - y0;
}

/* SET_COLOR_MASK's bytes to Vulkan's write mask. */
static uint32_t write_mask(uint32_t m)
{
    return (m & 0x00010000u ? 1u : 0u) | (m & 0x00000100u ? 2u : 0u)
         | (m & 0x00000001u ? 4u : 0u) | (m & 0x01000000u ? 8u : 0u);
}

/* Triangles, lines or points out of the batch's primitive: vertex numbers, the
 * provoking vertex of each primitive first (Vulkan's convention), the winding
 * the title's. Returns how many; *topo says of what. */
static uint32_t expand(uint32_t prim, uint32_t n, int last, uint32_t *out, uint32_t *topo)
{
    uint32_t i, k = 0;

#define TRI(a, b, c) do { out[k++] = (a); out[k++] = (b); out[k++] = (c); } while (0)
    *topo = TOPO_TRIANGLES;
    switch (prim) {
    case 1:
        *topo = TOPO_POINTS;
        for (i = 0; i < n; i++)
            out[k++] = i;
        break;
    case 2:
        *topo = TOPO_LINES;
        for (i = 0; i + 1 < n; i += 2) {
            out[k++] = last ? i + 1 : i;
            out[k++] = last ? i : i + 1;
        }
        break;
    case 3: case 4:
        *topo = TOPO_LINES;
        for (i = 0; i + 1 < n; i++) {
            out[k++] = last ? i + 1 : i;
            out[k++] = last ? i : i + 1;
        }
        if (prim == 3 && n > 2) {
            out[k++] = last ? 0 : n - 1;
            out[k++] = last ? n - 1 : 0;
        }
        break;
    case 5:
        for (i = 0; i + 2 < n; i += 3)
            if (last) TRI(i + 2, i, i + 1); else TRI(i, i + 1, i + 2);
        break;
    case 6:
        for (i = 0; i + 2 < n; i++)
            if (!(i & 1)) {
                if (last) TRI(i + 2, i, i + 1); else TRI(i, i + 1, i + 2);
            } else {
                if (last) TRI(i + 2, i + 1, i); else TRI(i, i + 2, i + 1);
            }
        break;
    case 7: case 10:
        for (i = 1; i + 1 < n; i++)
            if (prim == 10) TRI(0, i, i + 1);       /* a polygon has one colour */
            else if (last) TRI(i + 1, 0, i);
            else TRI(i, i + 1, 0);
        break;
    case 8:
        for (i = 0; i + 3 < n; i += 4)
            if (last) {
                TRI(i + 3, i, i + 1);
                TRI(i + 3, i + 1, i + 2);
            } else {
                TRI(i, i + 1, i + 2);
                TRI(i, i + 2, i + 3);
            }
        break;
    case 9:
        for (i = 0; i + 3 < n; i += 2)
            if (last) {
                TRI(i + 3, i, i + 1);
                TRI(i + 3, i + 2, i);
            } else {
                TRI(i, i + 1, i + 3);
                TRI(i, i + 3, i + 2);
            }
        break;
    default:
        break;
    }
#undef TRI
    return k;
}

/* The edges of quads (8), of a strip of them (9) or of a polygon, as lines. */
static uint32_t outline(uint32_t prim, uint32_t n, uint32_t *out)
{
    static const uint32_t strip[4] = { 0, 1, 3, 2 };
    uint32_t i, j, k = 0, step = prim == 8 ? 4 : prim == 9 ? 2 : n, of = prim == 10 ? n : 4;

    for (i = 0; n > 2 && i + of <= n; i += step)
        for (j = 0; j < of; j++) {
            uint32_t next = (j + 1) % of;

            out[k++] = i + (prim == 9 ? strip[j] : j);
            out[k++] = i + (prim == 9 ? strip[next] : next);
        }
    return k;
}

/* Scratch for one batch, kept. */
static Nv2aVshOutput *s_verts;
static uint32_t *s_index, s_verts_room, s_index_room;

static int scratch(uint32_t verts)
{
    uint32_t indices = verts * 4 + 8;

    if (verts > s_verts_room) {
        Nv2aVshOutput *v = (Nv2aVshOutput *)realloc(s_verts, (size_t)verts * sizeof *v);
        if (!v)
            return 0;
        s_verts = v;
        s_verts_room = verts;
    }
    if (indices > s_index_room) {
        uint32_t *x = (uint32_t *)realloc(s_index, (size_t)indices * sizeof *x);
        if (!x)
            return 0;
        s_index = x;
        s_index_room = indices;
    }
    return 1;
}

/* The program and the constants of the last batch that had them, kept in the
 * buffer until it starts over. */
static struct {
    Nv2aVkProgram info;
    uint32_t revision, known, round, at;        /* at: in words */
} s_program;
static struct { uint32_t revision, round, at; } s_constants;

/* The CPU is about to read the batch's arrays. */
static void arrays_read(const Nv2aRawBatch *b)
{
    uint32_t i, lo = UINT32_MAX, hi = 0;

    if (b->inline_data || g.drawn_lo >= g.drawn_hi)
        return;
    for (i = 0; i < b->index_count; i++) {
        if (b->indices[i] < lo) lo = b->indices[i];
        if (b->indices[i] > hi) hi = b->indices[i];
    }
    for (i = 0; i < 16; i++)
        if (b->attrs[i].size)
            memory_open(b->attrs[i].offset + lo * b->attrs[i].stride,
                        (uint64_t)(hi - lo) * b->attrs[i].stride + 16);
}

/* Where the arrays are read from, as guest address 0: the window's unguarded
 * view if they are all in the window. */
static const uint8_t *vertices_memory(const Nv2aVkVertices *v)
{
    uint32_t i;

    for (i = 0; i < v->ranges; i++)
        if (!g.window || v->range[i].from - XBOX_CONTIG_BASE >= XBOX_CONTIG_SIZE)
            return (const uint8_t *)xbox_GetMemoryOffset();
    return g.window - XBOX_CONTIG_BASE;
}

/* How the batch's vertices get transformed: 1 on the GPU, 0 on the CPU, -1 not
 * at all. */
static int vertices_on_gpu(const Nv2aRawBatch *b, Nv2aVkVertices *v)
{
    uint32_t i;

    if (b->transform != NV2A_TRANSFORM_PROGRAM || g.cpu_vertices)
        return 0;
    if (!s_program.known || s_program.revision != b->program_revision) {
        nv2a_vk_program_scan(b->program, b->program_start, &s_program.info);
        s_program.revision = b->program_revision;
        s_program.known = 1;
        s_program.round = g.stream_round - 1;
    }
    if (!s_program.info.complete)
        return -1;
    /* Every vertex reads the constants the one before left. */
    if (s_program.info.writes_constants || !nv2a_vk_vertices_plan(b, s_program.info.inputs, v))
        return 0;
    if (!b->inline_data)
        for (i = 0; i < v->ranges; i++)
            if (!guest_span_ok(v->range[i].from, v->range[i].bytes))
                return 0;
    if (!b->inline_data) {
        int guarded = vertices_memory(v) == (const uint8_t *)xbox_GetMemoryOffset();

        for (i = 0; i < v->ranges; i++)
            if (guarded)
                memory_open(v->range[i].from, v->range[i].bytes);
            else
                memory_read(v->range[i].from, v->range[i].bytes);
    }
    return 1;
}

/* What batch.vert places a fixed-function batch's vertices with: the composite
 * matrix, the viewport's offset, the anti-aliasing scale. */
typedef struct {
    float matrix[16], offset[4], scale[4];
} Fixed;

static struct {
    Fixed is;
    uint32_t round, at;                         /* at: in words */
} s_fixed;

/* Its vertices' positions are the title's then: with no image, all 0. */
static int fixed_on_gpu(const Nv2aRawBatch *b, Fixed *f)
{
    static float (*from)[4];
    static uint32_t room;
    uint32_t i, n = b->index_count;

    if (b->transform != NV2A_TRANSFORM_FIXED || g.cpu_vertices)
        return 0;
    if (n > room) {
        float (*more)[4] = (float (*)[4])realloc(from, (size_t)n * sizeof *from);
        if (!more)
            return 0;
        from = more;
        room = n;
    }
    if (!nv2a_batch_positions(0, n, from))
        return 0;
    memset(f, 0, sizeof *f);
    memcpy(f->matrix, b->regs + 0x0680 / 4, sizeof f->matrix);
    memcpy(f->offset, b->constants[59], sizeof f->offset);
    f->scale[0] = (float)b->target->aa_sx;
    f->scale[1] = (float)b->target->aa_sy;
    for (i = 0; i < n; i++)
        if (s_verts[i].pos[3] != 0.0f)
            memcpy(s_verts[i].pos, from[i], sizeof *from);
    return 1;
}

static uint32_t stream_fixed(const Fixed *f)
{
    if (s_fixed.round != g.stream_round || memcmp(&s_fixed.is, f, sizeof *f) != 0) {
        VkDeviceSize at;
        uint8_t *map = stream(sizeof *f, 16, &at);

        if (!map)
            return UINT32_MAX;
        memcpy(map, f, sizeof *f);
        s_fixed.is = *f;
        s_fixed.at = (uint32_t)(at / 4);
        s_fixed.round = g.stream_round;
    }
    return s_fixed.at;
}

#define PROGRAM_BYTES   (NV2A_VSH_SLOTS * 16u)
#define CONSTANT_BYTES  (NV2A_VSH_CONSTANTS * 16u)

/* Into the buffer, where there is room: the caller has seen to that. Returns
 * the header's place in words, UINT32_MAX if there was none. */
static uint32_t stream_vertices(const Nv2aRawBatch *b, Nv2aVkVertices *v)
{
    VkDeviceSize at;
    uint8_t *map;
    uint32_t i, data;

    if (s_program.round != g.stream_round) {
        map = stream((VkDeviceSize)s_program.info.count * 16, 16, &at);
        if (!map)
            return UINT32_MAX;
        memcpy(map, b->program[b->program_start], (size_t)s_program.info.count * 16);
        s_program.at = (uint32_t)(at / 4);
        s_program.round = g.stream_round;
    }
    if (s_constants.round != g.stream_round || s_constants.revision != b->constants_revision) {
        map = stream(CONSTANT_BYTES, 16, &at);
        if (!map)
            return UINT32_MAX;
        memcpy(map, b->constants, CONSTANT_BYTES);
        s_constants.at = (uint32_t)(at / 4);
        s_constants.round = g.stream_round;
        s_constants.revision = b->constants_revision;
    }
    map = stream(sizeof v->header + v->bytes, 16, &at);
    if (!map)
        return UINT32_MAX;
    data = (uint32_t)(at + sizeof v->header);
    v->header[0] = s_program.at;
    v->header[1] = s_program.info.count;
    v->header[2] = s_constants.at;
    {
        float aa[2];
        aa[0] = (float)b->target->aa_sx;
        aa[1] = (float)b->target->aa_sy;
        memcpy(&v->header[4], aa, sizeof aa);
    }
    for (i = 0; i < 16; i++)
        if (v->header[3] >> i & 1u)
            v->header[8 + 4 * i] += data;
    memcpy(map, v->header, sizeof v->header);
    nv2a_vk_vertices_copy(v, b, b->inline_data ? (const uint8_t *)b->inline_data
                                               : vertices_memory(v),
                          map + sizeof v->header);
    s_stats.vertex_bytes += v->bytes;
    return (uint32_t)(at / 4);
}

/* Put the batch's vertices, indices and state where the GPU reads them, and
 * draw. Rendering is opened here, after anything that may flush. `verts`
 * vertices are the CPU's, and `vertex` is what batch.vert places them with, or
 * UINT32_MAX; with 0 of them it is program.vert's header. */
static VkRect2D scaled_rect(const VkRect2D *r)
{
    VkRect2D out = *r;

    out.offset.x *= (int32_t)g.scale;
    out.offset.y *= (int32_t)g.scale;
    out.extent.width *= g.scale;
    out.extent.height *= g.scale;
    return out;
}

/* `scissor` in the title's pixels. */
static int submit(const GpuDraw *d, uint32_t verts, uint32_t vertex, uint32_t indices,
                  uint64_t key, const VkRect2D *scissor)
{
    VkRect2D clip = scaled_rect(scissor);
    VkDeviceSize vbytes = (VkDeviceSize)verts * sizeof(Nv2aVshOutput);
    VkDeviceSize ibytes = (VkDeviceSize)indices * 4, at, vat, iat, dat;
    VkPipeline pipe = pipeline_for(key, d);
    uint32_t push[2];
    uint8_t *map;

    if (!pipe)
        return 0;
    begin_recording();
    map = stream(vbytes + ibytes + 3 * sizeof *d, sizeof(Nv2aVshOutput), &at);
    if (!map)
        return 0;
    vat = at;
    iat = vat + vbytes;
    memcpy(g.stream_map + vat, s_verts, (size_t)vbytes);
    memcpy(g.stream_map + iat, s_index, (size_t)ibytes);
    if (!g.last_valid || memcmp(&g.last_draw, d, sizeof *d) != 0) {
        dat = (iat + ibytes + sizeof *d - 1) / sizeof *d * sizeof *d;
        memcpy(g.stream_map + dat, d, sizeof *d);
        g.last_draw = *d;
        g.last_index = (uint32_t)(dat / sizeof *d);
        g.last_valid = 1;
        g.stream_at = dat + sizeof *d;
    } else {
        g.stream_at = iat + ibytes;
    }
    begin_rendering();
    if (g.bound_pipe != pipe) {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g.bound_pipe = pipe;
    }
    vkCmdSetScissor(g.cmd, 0, 1, &clip);
    push[0] = g.last_index;
    push[1] = vertex;
    vkCmdPushConstants(g.cmd, g.layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof push, push);
    vkCmdBindIndexBuffer(g.cmd, g.stream, iat, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g.cmd, indices, 1, 0,
                     verts ? (int32_t)(vat / sizeof(Nv2aVshOutput)) : 0, 0);
    return 1;
}

static void fixed_state(void)
{
    VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
    static const float zero[4];

    vkCmdSetBlendConstants(g.cmd, zero);
    vkCmdSetCullMode(g.cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(g.cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(g.cmd, VK_FALSE);
    vkCmdSetDepthWriteEnable(g.cmd, VK_FALSE);
    vkCmdSetDepthCompareOp(g.cmd, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBiasEnable(g.cmd, VK_FALSE);
    vkCmdSetDepthBias(g.cmd, 0.0f, 0.0f, 0.0f);
    vkCmdSetStencilTestEnable(g.cmd, VK_FALSE);
    vkCmdSetStencilOp(g.cmd, both, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(g.cmd, both, 0xFF);
    vkCmdSetStencilWriteMask(g.cmd, both, 0xFF);
    vkCmdSetStencilReference(g.cmd, both, 0);
}

/* SET_FRONT_POLYGON_MODE's values to VkPolygonMode's. */
static uint32_t fill_mode(uint32_t v)
{
    return v == 0x1B00 ? VK_POLYGON_MODE_POINT : v == 0x1B01 ? VK_POLYGON_MODE_LINE
                                                             : VK_POLYGON_MODE_FILL;
}

/* For triangles filled as `fill`, without the faces in `skip` and the ones the
 * title culls. */
static const uint32_t offset_on[3] = { 0x0338, 0x0334, 0x0330 };

static void fill_state(const Nv2aRawBatch *b, uint32_t fill, VkCullModeFlags skip)
{
    const Nv2aRenderState *r = b->state;

    if (r->cull_enable)
        skip |= r->cull_face == 0x404 ? VK_CULL_MODE_FRONT_BIT
              : r->cull_face == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
    vkCmdSetCullMode(g.cmd, skip);
    /* SET_POLY_OFFSET_FILL_ENABLE, or _LINE_ or _POINT_. */
    vkCmdSetDepthBiasEnable(g.cmd, b->regs[offset_on[fill] / 4] & 1);
}

/* SET_LINE_WIDTH is in eighths of a pixel. */
static float line_width(const Nv2aRawBatch *b)
{
    float w = (float)(b->regs[0x0380 / 4] & 0x1FF) / 8.0f;

    if (b->target->aa_sx == 2 && b->target->aa_sy == 2)
        w *= 2.0f;
    w *= (float)g.scale;
    if (!g.has_wide_lines || w < 1.0f)
        return 1.0f;
    return w < g.props.limits.lineWidthRange[1] ? w : g.props.limits.lineWidthRange[1];
}

/* As compared with the numbers the other way round. */
static VkCompareOp turned(VkCompareOp op)
{
    return op == VK_COMPARE_OP_LESS ? VK_COMPARE_OP_GREATER
         : op == VK_COMPARE_OP_GREATER ? VK_COMPARE_OP_LESS
         : op == VK_COMPARE_OP_LESS_OR_EQUAL ? VK_COMPARE_OP_GREATER_OR_EQUAL
         : op == VK_COMPARE_OP_GREATER_OR_EQUAL ? VK_COMPARE_OP_LESS_OR_EQUAL : op;
}

static void batch_state(const Nv2aRawBatch *b)
{
    const Nv2aRenderState *r = b->state;
    VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
    uint32_t i;
    float c[4];

    for (i = 0; i < 3; i++)
        c[i] = (float)((r->blend_color >> (16 - 8 * i)) & 0xFF) / 255.0f;
    c[3] = (float)(r->blend_color >> 24) / 255.0f;
    if (blue_first(g.surf->cls)) {
        float red = c[0];
        c[0] = c[2];
        c[2] = red;
    }
    vkCmdSetBlendConstants(g.cmd, c);
    vkCmdSetFrontFace(g.cmd, r->front_face == 0x900 ? VK_FRONT_FACE_CLOCKWISE
                                                    : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    /* GL's depth write needs the depth test on; so does Vulkan's. */
    vkCmdSetDepthTestEnable(g.cmd, zeta_used() && r->depth_test_enable);
    vkCmdSetDepthWriteEnable(g.cmd, zeta_used() && r->depth_test_enable && r->depth_write);
    if (zeta_used() && zeta_used()->reciprocal) {
        vkCmdSetDepthCompareOp(g.cmd, turned(compare_op(r->depth_func)));
        /* The units are the vertex shader's: no constant of the host's. */
        vkCmdSetDepthBias(g.cmd, 0.0f, 0.0f,
                          -as_float(b->regs[0x0384 / 4]) * (float)g.scale);
    } else {
        vkCmdSetDepthCompareOp(g.cmd, compare_op(r->depth_func));
        /* SET_POLYGON_OFFSET_*. A unit is one step of the title's depth buffer,
         * about the host's smallest at 24 bits. */
        vkCmdSetDepthBias(g.cmd, as_float(b->regs[0x0388 / 4])
                                 * (((b->target->format >> 4) & 0xF) == 1
                                    ? (depth_is_float(b->regs) ? 2048.0f : 256.0f)
                                    : depth_is_float(b->regs) ? 128.0f : 1.0f),
                          0.0f, as_float(b->regs[0x0384 / 4]) * (float)g.scale);
    }
    vkCmdSetStencilTestEnable(g.cmd, zeta_used() && r->stencil_enable);
    vkCmdSetStencilOp(g.cmd, both, stencil_op(r->stencil_fail), stencil_op(r->stencil_zpass),
                      stencil_op(r->stencil_zfail), compare_op(r->stencil_func));
    vkCmdSetStencilCompareMask(g.cmd, both, r->stencil_func_mask & 0xFF);
    vkCmdSetStencilWriteMask(g.cmd, both, r->stencil_write_mask & 0xFF);
    vkCmdSetStencilReference(g.cmd, both, r->stencil_ref & 0xFF);
}

static void surface_state(GpuDraw *d, const Nv2aTarget *t)
{
    Nv2aVkDepth z = depth_of(t, NULL);

    if (blue_first(g.surf->cls))
        d->flags |= 8;
    if (g.scale > 1)
        d->flags |= g.scale << 16;
    if (g.many)
        d->flags |= log2_of(g.samples) << 12;
    d->surface[0] = 2.0f / (float)g.surf->w;
    d->surface[1] = 2.0f / (float)g.surf->h;
    d->surface[2] = (float)(1.0 / nv2a_vk_depth_most(&z));
    d->surface[3] = 1.0f;
}

/* 1 over the largest, of which the 24-bit one's is no normal float: 1 if it is
 * to be taken 2^-64 again. */
static int depth_scale(const Nv2aVkDepth *d, float *out)
{
    double most = nv2a_vk_depth_most(d);

    *out = (float)((most > 1e38 ? 18446744073709551616.0 : 1.0) / most);
    return most > 1e38;
}

/* The stage's texture and how to sample it, by its program. May flush. */
static void fill_stage(GpuDraw *d, const Nv2aRawBatch *b, uint32_t st, uint32_t mode)
{
    const uint32_t *reg = b->regs + (0x1B00 + 0x40 * st) / 4;
    const Nv2aBatchTexture *bt = &b->textures[st];
    GpuStage *s = &d->stage[st];
    Sampled t = {0};
    Nv2aVkDepth depth;
    uint32_t want, filter = bt->t.filter;
    int part, found;

    switch (mode) {
    case 1: case 6: case 7: case 9: case 15: case 16: want = 1; break;
    case 2: case 13:                                  want = 2; break;
    case 3: case 11: case 12: case 14: case 18:       want = 3; break;
    case 0: case 4: case 5: case 10: case 17:         return;
    default:
        say_once(0x30000u | mode, "stage program not done, reads zero: mode", mode);
        return;
    }
    part = nv2a_perf_part(NV2A_PERF_TEXTURES);
    found = texture_for(bt, reg[1], &t);
    nv2a_perf_part(part);
    if (!found)
        return;
    if (t.kind != want && !(want == 2 && t.kind == 1) && !(want == 3 && t.kind == 1)) {
        say_once(0x40000u | want << 4 | t.kind, "stage program and texture disagree",
                 want << 4 | t.kind);
        return;
    }
    s->kind = t.kind;
    s->image = t.slot;
    s->scale[0] = t.linear ? 1.0f / (float)t.width : 1.0f;
    s->scale[1] = t.linear ? 1.0f / (float)t.height : 1.0f;
    if (t.border) {
        s->scale[0] = (float)(t.width - 2 * t.border) / (float)t.width;
        s->scale[1] = (float)(t.height - 2 * t.border) / (float)t.height;
        s->flags = 2;
    }
    if (nv2a_vk_tex_depth(bt->t.color, &depth)) {
        s->flags |= 4;
        if (((filter >> 24) & 0xFF) != 1)
            s->flags |= 8;
        if (t.reciprocal)
            s->flags |= 16;
        filter = (filter & 0xF000FFFFu) | 0x01010000u;
        s->lum[0] = 1.0f;
        s->lum[1] = depth_scale(&depth, &s->lum[0]) ? 1.0f / 18446744073709551616.0f : 1.0f;
    }
    s->smp = sampler_for(reg[0x08 / 4], reg[0x0C / 4], filter, bt->border, bt->border_color,
                         t.levels);
    s->flags |= (reg[0x0C / 4] >> 2) & 1;           /* ALPHA_KILL_ENABLE */
    /* SET_TEXTURE_SET_BUMP_ENV_MAT holds D3D's 00, 01, 11, 10: a title's
     * plain (0.5, 0.5) scaling arrives as 0.5, 0, 0.5, 0. */
    s->bump[0] = as_float(reg[0x28 / 4]);
    s->bump[1] = as_float(reg[0x2C / 4]);
    s->bump[2] = as_float(reg[0x34 / 4]);
    s->bump[3] = as_float(reg[0x30 / 4]);
    if (!(s->flags & 4)) {
        s->lum[0] = as_float(reg[0x38 / 4]);
        s->lum[1] = as_float(reg[0x3C / 4]);
    }
}

/* SET_POINT_SIZE is in eighths of a pixel, at most 0x1FF, and a program's own
 * point size counts only with SET_POINT_PARAMS_ENABLE, as it is
 * (nxdk_pgraph_tests' Point_size results, taken on the hardware). */
static void fill_points(GpuDraw *d, const Nv2aRawBatch *b)
{
    uint32_t size = b->regs[0x043C / 4];

    if (size && size <= 0x1FF)
        d->surface[3] = (float)size / 8.0f;
    if (b->target->aa_sx == 2 && b->target->aa_sy == 2)
        d->surface[3] *= 2.0f;
    d->surface[3] *= (float)g.scale;
    if (b->regs[0x0318 / 4] & 1) {
        if (b->transform == NV2A_TRANSFORM_PROGRAM)
            d->flags |= 256;
        else
            say_once(11, "points not scaled by distance", b->regs[0x0318 / 4]);
    }
    if (b->regs[0x031C / 4] & 1)                    /* SET_POINT_SMOOTH_ENABLE */
        d->flags |= 512;
}

static void fill_draw(GpuDraw *d, const Nv2aRawBatch *b)
{
    const Nv2aCombiner *rc = b->combiner;
    const uint32_t *reg = b->regs;
    uint32_t st;

    memset(d, 0, sizeof *d);
    surface_state(d, b->target);
    if (depth_is_float(reg)) {
        Nv2aVkDepth z = depth_of(b->target, reg);

        if (depth_scale(&z, &d->surface[2]))
            d->flags |= 1024;
    }
    if (rc) {
        memcpy(d->color_icw, rc->color_icw, sizeof d->color_icw);
        memcpy(d->alpha_icw, rc->alpha_icw, sizeof d->alpha_icw);
        memcpy(d->color_ocw, rc->color_ocw, sizeof d->color_ocw);
        memcpy(d->alpha_ocw, rc->alpha_ocw, sizeof d->alpha_ocw);
        memcpy(d->factor0, rc->factor0, sizeof d->factor0);
        memcpy(d->factor1, rc->factor1, sizeof d->factor1);
        d->final0 = rc->final0;
        d->final1 = rc->final1;
        d->final_c0 = rc->final_c0;
        d->final_c1 = rc->final_c1;
        d->control = rc->control;
        d->stage_program = rc->stage_program;
        d->flags |= 1;
    } else if (b->textures[0].valid) {
        d->stage_program = 1;
    }
    d->other_stage = reg[0x1E78 / 4];
    d->clip_plane = reg[0x17F8 / 4];
    d->dot_mapping = reg[0x1E74 / 4];
    for (st = 0; st < 12; st += 4)
        if (((d->dot_mapping >> st) & 7) > 4)
            say_once(9, "dot mapping not done", d->dot_mapping);
    memcpy(d->eye, &reg[0x181C / 4], 12);           /* SET_EYE_VECTOR */
    d->shadow_func = reg[0x1E6C / 4];
    if (reg[0x02A4 / 4])
        d->flags |= 2;
    if (reg[0x037C / 4] == 0x1D00)                  /* SET_SHADE_MODE: flat */
        d->flags |= 4;
    if (reg[0x0310 / 4] && g.surf->cls == C_1555)   /* SET_DITHER_ENABLE */
        d->flags |= 16;
    if (reg[0x0310 / 4] && g.surf->cls == C_565)
        d->flags |= 16 | 32;
    if (!(reg[0x03B8 / 4] & 1))                     /* SET_SPECULAR_ENABLE */
        d->flags |= 64;
    if (!(reg[0x0294 / 4] & 0x20000))               /* SET_LIGHT_CONTROL */
        d->flags |= 128;
    if (b->prim == 1)
        fill_points(d, b);
    d->fog_mode = reg[0x029C / 4];
    d->fog_color = reg[0x02A8 / 4];
    d->fog0 = as_float(reg[0x09C0 / 4]);
    d->fog1 = as_float(reg[0x09C4 / 4]);
    if (b->state->alpha_test_enable) {
        d->alpha_func = b->state->alpha_func;
        d->alpha_ref = b->state->alpha_ref & 0xFF;
    }
    for (st = 0; st < 4; st++)
        fill_stage(d, b, st, (d->stage_program >> (5 * st)) & 0x1F);
}

static int finite_pos(const Nv2aVshOutput *v)
{
    float s = v->pos[0] + v->pos[1] + v->pos[2] + v->pos[3];
    return s - s == 0.0f;
}

/* The CPU rasteriser draws it, from guest memory and into it. */
static int refused(void)
{
    uint32_t i;

    store_range(0, 0x100000000ull, NULL);
    for (i = 0; i < MAX_SURFACES; i++)          /* its threads write there */
        surface_guard(&s_surfaces[i], GUEST_OPEN);
    s_stats.refused++;
    return 0;
}

static int draw(const Nv2aRawBatch *b)
{
    const Nv2aRenderState *r = b->state;
    GpuDraw d;
    VkRect2D scissor;
    uint32_t n = b->index_count, indices, topo, i, vertex = UINT32_MAX, fill[2], sides;
    static Nv2aVkVertices plan;
    Fixed place;
    uint64_t key;
    double from;
    int gpu, fixed = 0, part, clamp, replace = 0, reciprocal;

    if (!bind_target(b->target))
        return refused();
    choose_many(in_depth(r), 0);
    if (!scratch(n))
        return 0;
    part = nv2a_perf_part(NV2A_PERF_VERTICES);
    from = draw_ms();
    gpu = vertices_on_gpu(b, &plan);
    s_stats.vertex_ms += draw_ms() - from;
    if (gpu < 0)
        return 1;                       /* nothing to run: as on the CPU path */
    if (!gpu) {
        arrays_read(b);
        from = draw_ms();
        if (!nv2a_batch_transform(0, n, s_verts))
            return 1;
        s_stats.transform_ms += draw_ms() - from;
        for (i = 0; i < n; i++)
            if (!finite_pos(&s_verts[i]))   /* no image: clipped away */
                memset(s_verts[i].pos, 0, sizeof s_verts[i].pos);
        fixed = fixed_on_gpu(b, &place);
    }
    /* SET_PROVOKING_VERTEX: 1 is D3D's, the first vertex. */
    indices = expand(b->prim, n, !(b->regs[0x09FC / 4] & 1), s_index, &topo);
    fill[0] = fill_mode(b->regs[0x038C / 4]);
    fill[1] = fill_mode(b->regs[0x0390 / 4]);
    if (topo != TOPO_TRIANGLES || !g.has_fill_modes)
        fill[0] = fill[1] = VK_POLYGON_MODE_FILL;
    else if (b->prim >= 8 && fill[0] == VK_POLYGON_MODE_LINE && fill[1] == fill[0]
             && !r->cull_enable && !(b->regs[0x0334 / 4] & 1)) {
        indices = outline(b->prim, n, s_index);     /* without the diagonals */
        topo = TOPO_LINES;
        fill[0] = fill[1] = VK_POLYGON_MODE_FILL;
    }
    if (gpu && !plan.gather)
        for (i = 0; i < indices; i++)
            s_index[i] = b->indices[s_index[i]] - plan.first;
    nv2a_perf_part(part);
    clip_rect(b->target, b->regs, &scissor);
    s_stats.draws++;
    s_stats.vertices += n;
    if (!indices || !scissor.extent.width || !scissor.extent.height)
        return 1;
    zeta_for_batch(b);
    fill_draw(&d, b);
    clamp = ((b->regs[0x1D78 / 4] >> 4) & 0xF) == 1;
    for (i = 0; i < 20; i += 5)
        if (((d.stage_program >> i) & 0x1F) == 10)
            replace = 1;
    if (replace && clamp)
        d.flags |= 2048;
    reciprocal = by_w(b->regs) || (zeta_used() && zeta_used()->reciprocal && in_depth(r));
    if (reciprocal) {
        d.flags |= FLAG_RECIPROCAL;
        if (by_w(b->regs)) {
            /* Fragment by fragment: what is cut at a limit, and has others as
             * corners, has other depths than a pass with another limit. */
            d.flags |= FLAG_BY_W | (clamp ? 0u : FLAG_RANGE);
            d.range[0] = as_float(b->regs[0x0394 / 4]);     /* SET_CLIP_MIN, _MAX */
            d.range[1] = as_float(b->regs[0x0398 / 4]);
        }
        if (b->regs[offset_on[fill[0]] / 4] & 1)
            d.offset = as_float(b->regs[0x0388 / 4]);
        if (zeta_used() && !zeta_used()->reciprocal)        /* no depths to replace */
            d.flags &= ~FLAG_RECIPROCAL;
    }
    key = pipeline_key(topo, write_mask(r->color_mask), r->blend_enable != 0, r->blend_src,
                       r->blend_dst, r->blend_eq, zeta_used() != NULL,
                       clamp, gpu, g.surf->cls,
                       topo == TOPO_TRIANGLES && (b->regs[0x17C4 / 4] & 1));
    if (reciprocal)
        key |= KEY_RECIPROCAL;
    if (g.surf == &s_nothing)
        key |= KEY_NOTHING;
    if (g.many)
        key |= (uint64_t)log2_of(g.samples) << KEY_SAMPLES;
    if (replace)
        key |= KEY_DEPTH_REPLACE;
    if (g.many && s_shade_samples && !g.each_frag)
        say_once(16, "the card shades no samples: MSAA", g.samples);
    if (g.many && s_shade_samples && g.each_frag)
        key |= KEY_EACH;
    if (g.many && !replace && topo == TOPO_TRIANGLES && !r->blend_enable
        && (d.alpha_func == 0x201 || d.alpha_func == 0x203 || d.alpha_func == 0x204
            || d.alpha_func == 0x206)) {
        LONG how = s_alpha_test_aa;

        if (how == NV2A_VK_ALPHA_TEST_AA_PIXEL) /* all of it once for a pixel */
            key &= ~KEY_EACH;
        else if (how == NV2A_VK_ALPHA_TEST_AA_SAMPLE && g.each_frag)
            key |= KEY_EACH;
        else if (!(key & KEY_EACH))
            key |= KEY_CUTOUT;
        d.flags |= (uint32_t)how << 14;
    }
    if (b->regs[0x17BC / 4] & 1) {
        /* SET_LOGIC_OP's values are OpenGL's, from CLEAR (0x1500) to SET, in
         * Vulkan's order; with one there is no blending in any of them. */
        if (g.has_logic_ops)
            key |= 1ull << KEY_LOGIC | (uint64_t)(b->regs[0x17C0 / 4] & 0xF) << (KEY_LOGIC + 1);
        else
            say_once(8, "logic op ignored", b->regs[0x17C0 / 4]);
    }
    /* Rendering must be open for the state, and submit() opens it after its
     * last chance to flush; so the state goes in between. */
    {
        VkDeviceSize need = (VkDeviceSize)indices * 4 + 4 * sizeof d + 4096
            + (gpu ? plan.bytes + sizeof plan.header + PROGRAM_BYTES + CONSTANT_BYTES
                   : (VkDeviceSize)n * sizeof(Nv2aVshOutput) + sizeof place);

        if (fill[0] != fill[1])
            need *= 2;
        if (g.stream_at + need > g.stream_end)
            flush();
    }
    if (b->regs[0x17CC / 4])
        count_room();
    begin_rendering();
    if (b->regs[0x17CC / 4])
        count_begin();
    else
        count_end();
    batch_state(b);
    if (topo == TOPO_LINES || fill[0] == VK_POLYGON_MODE_LINE || fill[1] == VK_POLYGON_MODE_LINE) {
        vkCmdSetLineWidth(g.cmd, line_width(b));
        /* SET_LINE_SMOOTH_ENABLE: the part of a pixel that is covered scales its
         * alpha, as in OpenGL and in nxdk_pgraph_tests' Smoothing_control, from
         * the hardware. Not with SET_ANTI_ALIASING_CONTROL's bit 0. */
        if (g.has_smooth_lines && (b->regs[0x0320 / 4] & 1) && !(b->regs[0x1D7C / 4] & 1))
            key |= KEY_SMOOTH;
    }
    if (gpu) {
        nv2a_perf_part(NV2A_PERF_VERTICES);
        from = draw_ms();
        vertex = stream_vertices(b, &plan);
        s_stats.vertex_ms += draw_ms() - from;
        nv2a_perf_part(part);
        if (vertex == UINT32_MAX)
            return 1;
        s_stats.programs++;
    } else if (fixed) {
        vertex = stream_fixed(&place);
        if (vertex == UINT32_MAX)
            return 1;
    }
    sides = fill[0] != fill[1] ? 2 : 1;             /* back faces after front ones */
    for (i = 0; i < sides; i++) {
        fill_state(b, fill[i], sides == 1 ? VK_CULL_MODE_NONE
                             : i ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
        if (!submit(&d, gpu ? 0 : n, vertex, indices, key | (uint64_t)fill[i] << KEY_FILL,
                    &scissor))
            return 1;
    }
    if (g.surf != &s_nothing) {
        surface_drawn(g.surf);
        surface_changed(g.surf);
    }
    if (g.zeta && ((r->depth_test_enable && r->depth_write) || r->stencil_enable)) {
        Nv2aVkDepth z = depth_of(b->target, b->regs);

        zeta_drawn(g.zeta, &z, b->target);
    }
    return 1;
}

static int clear(const Nv2aTarget *t, const Nv2aRenderState *rs, const Nv2aRawClear *c)
{
    VkClearAttachment at[2];
    VkClearRect cr;
    Nv2aVkDepth depth = depth_of(t, c->regs);
    uint32_t n = 0, color = c->flags & 0xF0, v;
    float f[4];

    (void)rs;
    if (!bind_target(t))
        return refused();
    if (g.surf == &s_nothing)
        color = 0;
    memset(&cr, 0, sizeof cr);
    cr.layerCount = 1;
    clip_rect(t, NULL, &cr.rect);
    if (c->rect_set) {
        uint32_t x0 = c->rect_h & 0xFFFF, x1 = (c->rect_h >> 16) + 1;
        uint32_t y0 = c->rect_v & 0xFFFF, y1 = (c->rect_v >> 16) + 1;

        /* Not bounded by the surface clip (nv2a_pb_exec.c clear_surface), and
         * in logical pixels like it. */
        x0 *= t->aa_sx; x1 *= t->aa_sx;
        y0 *= t->aa_sy; y1 *= t->aa_sy;
        if (x1 > g.surf->w) x1 = g.surf->w;
        if (y1 > g.surf->h) y1 = g.surf->h;
        if (x0 >= x1 || y0 >= y1)
            return 1;
        cr.rect.offset.x = (int32_t)x0;
        cr.rect.offset.y = (int32_t)y0;
        cr.rect.extent.width = x1 - x0;
        cr.rect.extent.height = y1 - y0;
    }
    if (!cr.rect.extent.width || !cr.rect.extent.height)
        return 1;

    /* The card must round as surface_fill_guest does. */
    v = pixel_out(g.surf->color, c->argb);
    pixel_floats(g.surf->color, v, f);
    memset(at, 0, sizeof at);
    if (color == 0xF0) {
        int swap = blue_first(g.surf->cls);

        at[n].aspectMask = COLOR_BIT;
        at[n].clearValue.color.float32[0] = f[swap ? 2 : 0];
        at[n].clearValue.color.float32[1] = f[1];
        at[n].clearValue.color.float32[2] = f[swap ? 0 : 2];
        at[n].clearValue.color.float32[3] = f[3];
        n++;
    }
    if (g.zeta && (c->flags & 3)) {
        at[n].aspectMask = (c->flags & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                         | (c->flags & 2 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        at[n].clearValue.depthStencil.depth
            = zeta_holds(g.zeta, &depth, nv2a_vk_depth_read(&depth, c->zstencil));
        if (c->flags & 1)
            g.zeta->fresh = cr.rect.offset.x || cr.rect.offset.y
                            || cr.rect.extent.width != g.surf->w
                            || cr.rect.extent.height != g.surf->h
                          ? -1.0f : nv2a_vk_depth_read(&depth, c->zstencil);
        at[n].clearValue.depthStencil.stencil = depth.bytes == 2 ? 0 : c->zstencil & 0xFF;
        n++;
        zeta_drawn(g.zeta, &depth, t);
    }
    choose_many(g.zeta && (c->flags & 3),
                color == 0xF0 && cr.rect.extent.width == g.surf->w
                && cr.rect.extent.height == g.surf->h);
    if (n) {
        VkClearRect larger = cr;

        larger.rect = scaled_rect(&cr.rect);
        begin_rendering();
        vkCmdClearAttachments(g.cmd, n, at, 1, &larger);
    }
    if (color == 0xF0) {
        if (tracked(g.surf))
            surface_drawn(g.surf);      /* the title writes over it once it has it */
        else
            surface_fill_guest(g.surf, &cr.rect, v);
        surface_changed(g.surf);
    } else if (color) {
        /* Some channels only: a rectangle drawn with the others masked. */
        float x0 = (float)cr.rect.offset.x, y0 = (float)cr.rect.offset.y;
        float x1 = x0 + (float)cr.rect.extent.width, y1 = y0 + (float)cr.rect.extent.height;
        uint32_t mask = (color & 0x10 ? 1u : 0u) | (color & 0x20 ? 2u : 0u)
                      | (color & 0x40 ? 4u : 0u) | (color & 0x80 ? 8u : 0u), i;
        GpuDraw d;

        if (!scratch(4))
            return 1;
        choose_many(0, 0);
        memset(&d, 0, sizeof d);
        surface_state(&d, t);
        memset(s_verts, 0, 4 * sizeof *s_verts);
        for (i = 0; i < 4; i++) {
            s_verts[i].pos[0] = i == 1 || i == 2 ? x1 : x0;
            s_verts[i].pos[1] = i >= 2 ? y1 : y0;
            s_verts[i].pos[3] = 1.0f;
            memcpy(s_verts[i].d0, f, sizeof f);
        }
        for (i = 0; i < 6; i++)
            s_index[i] = i < 3 ? i : i == 3 ? 0 : i - 2;
        if (g.stream_at + 8192 > g.stream_end)
            flush();
        begin_rendering();
        count_end();
        fixed_state();
        if (submit(&d, 4, UINT32_MAX, 6, pipeline_key(TOPO_TRIANGLES, mask, 0, 0, 0, 0,
                                             zeta_used() != NULL, 0, 0, g.surf->cls, 0)
                                | (uint64_t)(g.many ? log2_of(g.samples) : 0) << KEY_SAMPLES,
                   &cr.rect)) {
            surface_drawn(g.surf);
            surface_changed(g.surf);
        }
    }
    return 1;
}

/* ---- the flip ----------------------------------------------------------- */

static void report(void)
{
    static int on = -1;
    static Nv2aVkStats was;
    static unsigned flips;
    double n = 60.0;

    if (on < 0) {
        const char *e = getenv("RECOMP_NV2A_VK_STATS");
        on = e && strcmp(e, "0") != 0;
    }
    if (!on || ++flips % 60)
        return;
    fprintf(stderr, "[NV2A-VK] a frame: %.0f draws (%.0f programs, %.0f specialized),"
            " %.0f vertices (%.0f KB),"
            " %.1f flushes, %.1f surfaces"
            " back (%.1f mid-frame), %.1f up, %.1f sampled (%.1f copies, %.1f rearranged),"
            " %.1f textures (%.0f KB, %.0f KB hashed), %.1f touched,"
            " %.1f reports (%.1f early);"
            " ms: transform %.2f, vertex copy %.2f, textures %.2f, gpu wait %.2f"
            " (%.2f for an earlier frame), copying back %.2f; gpu %.2f\n",
            (s_stats.draws - was.draws) / n, (s_stats.programs - was.programs) / n,
            (s_stats.specialized_draws - was.specialized_draws) / n,
            (s_stats.vertices - was.vertices) / n,
            (double)(s_stats.vertex_bytes - was.vertex_bytes) / n / 1024.0,
            (s_stats.flushes - was.flushes) / n, (s_stats.readbacks - was.readbacks) / n,
            (s_stats.mid_frame - was.mid_frame) / n,
            (s_stats.surface_uploads - was.surface_uploads) / n,
            (s_stats.sampled - was.sampled) / n, (s_stats.card_copies - was.card_copies) / n,
            (s_stats.rearranged - was.rearranged) / n,
            (s_stats.texture_uploads - was.texture_uploads) / n,
            (double)(s_stats.texture_bytes - was.texture_bytes) / n / 1024.0,
            (double)(s_stats.hashed_bytes - was.hashed_bytes) / n / 1024.0,
            (s_stats.touches - was.touches) / n,
            (s_stats.reports - was.reports) / n, (s_stats.early - was.early) / n,
            (s_stats.transform_ms - was.transform_ms) / n,
            (s_stats.vertex_ms - was.vertex_ms) / n,
            (s_stats.texture_ms - was.texture_ms) / n, (s_stats.wait_ms - was.wait_ms) / n,
            (s_stats.frame_wait_ms - was.frame_wait_ms) / n,
            (s_stats.readback_ms - was.readback_ms) / n, (s_stats.gpu_ms - was.gpu_ms) / n);
    was = s_stats;
}

/* Records a copy of the picture to show for the window's swap chain
 * (nv2a_vk_present.h), from the surface it was drawn in or else from guest
 * memory. -1: not this time, or there is no window. */
static int picture_copy(uint32_t va, uint32_t pitch, uint32_t aa_sy, uint32_t *scale)
{
    uint32_t rows = 480 * aa_sy, i, w, h, cls = C_8888;
    VkDeviceSize at = 0;
    VkBufferImageCopy up = {0};
    VkImageCopy c = {0};
    VkImage image;
    Surface *s = NULL;
    int picture;

    if (!va)
        return -1;
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *q = &s_surfaces[i];

        if (q->used && q->valid && !q->swizzled && q->va == va && q->pitch == pitch
            && pitch == q->w * q->bpp && q->cls <= C_1555 && (!s || q->dirty))
            s = q;
    }
    if (s) {
        if (!s->dirty)                  /* another may be newer */
            store_range(va, s->bytes, s);
        surface_refresh(s);             /* with what the title wrote over it */
        if (!nv2a_vk_present_on())
            return -1;
        w = s->w * g.scale;
        h = (s->h < rows ? s->h : rows) * g.scale;
        cls = s->cls;
        *scale = g.scale;
    } else {
        uint8_t *px;

        /* As the window reads it (fb_present.c): 640 wide, 1280 with AA. */
        if (pitch != 640 * 2 && pitch != 640 * 4 && pitch != 640 * 8)
            return -1;
        if (!guest_span_ok(va, (uint64_t)pitch * rows))
            return -1;
        if (pitch == 640 * 2)
            cls = C_565;
        w = pitch / s_classes[cls].bytes;
        h = rows;
        store_range(va, (uint64_t)pitch * rows, NULL);
        if (!nv2a_vk_present_on())
            return -1;
        begin_recording();
        px = stream((VkDeviceSize)pitch * rows, 16, &at);
        if (!px)
            return -1;
        memcpy(px, guest(va), (size_t)pitch * rows);
    }
    picture = nv2a_vk_present_take(s_classes[cls].format, w, h, &image);
    if (picture < 0)
        return -1;
    InterlockedExchange(&s_picture, (LONG)(*scale << 28 | w / *scale << 14 | h / *scale));
    end_rendering();
    begin_recording();
    if (s)
        surface_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(g.cmd, image, COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, TRANSFER, 0,
                  TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    c.srcSubresource.aspectMask = COLOR_BIT;
    c.srcSubresource.layerCount = 1;
    c.dstSubresource = up.imageSubresource = c.srcSubresource;
    c.extent.width = w;
    c.extent.height = h;
    c.extent.depth = 1;
    up.bufferOffset = at;
    up.imageExtent = c.extent;
    if (s)
        vkCmdCopyImage(g.cmd, s->img.image, s->layout, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    else
        vkCmdCopyBufferToImage(g.cmd, g.stream, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &up);
    image_barrier(g.cmd, image, COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TRANSFER,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, TRANSFER, VK_ACCESS_2_TRANSFER_READ_BIT);
    return picture;
}

/* A flip to the window: for the vblank it will be held until. */
static void present_picture(const Nv2aPresent *p)
{
    uint32_t scale = 1;
    int picture = picture_copy(p->va, p->pitch, p->aa_sy, &scale);

    if (g.mode == NV2A_VK_COPYBACK_ALWAYS) {
        store_range(0, 0x100000000ull, NULL);
        flush();
    } else {
        if (g.recording)
            submit_frame();
        frame_end();
    }
    if (picture >= 0 && g.lost) {
        nv2a_vk_present_drop(picture);
    } else if (picture >= 0) {
        nv2a_vk_present_show(picture, g.submits, 480 * p->aa_sy * scale, p->has_due, p->due,
                             nv2a_perf_frame_open());
    }
}

/* What was drawn where no page can be guarded. */
static void store_untracked(void)
{
    uint32_t i;

    for (i = 0; i < MAX_SURFACES; i++)
        if (s_surfaces[i].dirty && !tracked(&s_surfaces[i])) {
            say_once(10, "a surface outside the contiguous window is copied back at each"
                     " flip", s_surfaces[i].va);
            store_range(s_surfaces[i].va, s_surfaces[i].bytes, NULL);
        }
}

/* Everything drawn is put into memory and drawn anew at another size, or with
 * other samples; depths are lost until the next clear. */
static void rescale(uint32_t scale, uint32_t samples)
{
    uint32_t i;

    begin_recording();
    for (i = 0; i < MAX_SURFACES; i++)
        if (s_surfaces[i].used)
            surface_drop(&s_surfaces[i]);
    for (i = 0; i < MAX_ZETAS; i++)
        if (s_zetas[i].used) {
            retire(&s_zetas[i].img, 0, 0);
            if (s_zetas[i].one.image)
                retire(&s_zetas[i].one, 0, 0);
            memset(&s_zetas[i], 0, sizeof s_zetas[i]);
        }
    for (i = 0; i < TEX_BUCKETS; i++) {
        Texture *t;

        for (t = s_tex[i]; t; t = t->next)
            if (t->drawn && t->img.image) {
                texture_release(t);
                memset(&t->img, 0, sizeof t->img);
                t->hash = 0;
            }
    }
    g.surf = g.bound_surf = NULL;
    g.zeta = g.bound_zeta = NULL;
    g.last_valid = 0;
    g.scale = scale;
    g.samples = samples;
}

void nv2a_vk_scale(int scale)
{
    if (scale >= NV2A_VK_SCALE_FIT && scale <= NV2A_VK_SCALES)
        InterlockedExchange(&s_scale, scale);
}

/* Once a flip. Another scale draws everything anew: not while a window is being
 * dragged to its size. */
#define SETTLED 30u
static uint32_t scale_wanted(void)
{
    static uint32_t seen, flips;
    uint32_t scale = (uint32_t)s_scale;

    if (scale != NV2A_VK_SCALE_FIT)
        return scale;
    scale = (uint32_t)nv2a_vk_present_scale();
    flips = scale == seen ? flips + 1 : 0;
    seen = scale;
    return flips >= SETTLED ? scale : g.scale;
}

int nv2a_vk_scale_is(void) { return (int)s_scale; }

void nv2a_vk_samples(int n)
{
    if (n >= 1 && n <= NV2A_VK_SAMPLES && !(n & (n - 1)))
        InterlockedExchange(&s_samples, n);
}

int nv2a_vk_samples_is(void) { return (int)s_samples; }

void nv2a_vk_shade_samples(int on) { InterlockedExchange(&s_shade_samples, on != 0); }
int nv2a_vk_shade_samples_is(void) { return (int)s_shade_samples; }

static void aa_named(const char *e)
{
    static const char *const names[7] = { "off", "msaa2", "msaa4", "msaa8",
                                          "ssaa2", "ssaa4", "ssaa8" };
    int i;

    for (i = 0; i < 7; i++)
        if (strcmp(e, names[i]) == 0) {
            nv2a_vk_samples(1 << (i > 3 ? i - 3 : i));
            nv2a_vk_shade_samples(i > 3);
            return;
        }
    fprintf(stderr, "[NV2A-VK] no antialiasing %s: off, msaa2, 4 or 8, ssaa2, 4 or 8\n", e);
}

/* As asked for, or what the card has below that. */
static uint32_t samples_wanted(void)
{
    const VkPhysicalDeviceLimits *l = &g.props.limits;
    uint32_t n = (uint32_t)s_samples;

    while (n > 1 && !(l->framebufferColorSampleCounts & l->framebufferDepthSampleCounts
                      & l->framebufferStencilSampleCounts & n))
        n /= 2;
    if (n != (uint32_t)s_samples)
        say_once(15, "the card has fewer samples", n);
    return n;
}

void nv2a_vk_alpha_test_aa(Nv2aVkAlphaTestAa how)
{
    if ((int)how >= 0 && how < NV2A_VK_ALPHA_TEST_AA_MODES)
        InterlockedExchange(&s_alpha_test_aa, how);
}

Nv2aVkAlphaTestAa nv2a_vk_alpha_test_aa_is(void) { return (Nv2aVkAlphaTestAa)s_alpha_test_aa; }

static void alpha_test_aa_named(const char *e)
{
    static const char *const names[NV2A_VK_ALPHA_TEST_AA_MODES] = {
        "pixel", "soft", "sharp", "sample" };
    int i;

    for (i = 0; i < NV2A_VK_ALPHA_TEST_AA_MODES; i++)
        if (strcmp(e, names[i]) == 0) {
            nv2a_vk_alpha_test_aa((Nv2aVkAlphaTestAa)i);
            return;
        }
    fprintf(stderr, "[NV2A-VK] no alpha-test antialiasing %s: pixel, soft, sharp or sample\n", e);
}

void nv2a_vk_anisotropy(int most)
{
    if (most >= 1 && most <= NV2A_VK_ANISOTROPY && !(most & (most - 1)))
        InterlockedExchange(&s_aniso, most);
}

int nv2a_vk_anisotropy_is(void) { return (int)s_aniso; }

const char *nv2a_vk_card(void) { return g.props.deviceName; }

uint32_t nv2a_vk_picture(uint32_t *w, uint32_t *h)
{
    uint32_t v = (uint32_t)s_picture;

    *w = v >> 14 & 0x3FFF;
    *h = v & 0x3FFF;
    return v >> 28;
}

void nv2a_vk_aspect(Nv2aVkAspect aspect)
{
    if ((unsigned)aspect < NV2A_VK_ASPECTS)
        InterlockedExchange(&s_aspect, (LONG)aspect);
}

Nv2aVkAspect nv2a_vk_aspect_is(void) { return (Nv2aVkAspect)s_aspect; }

static void aspect_named(const char *name)
{
    static const char *const names[NV2A_VK_ASPECTS] = { "auto", "4:3", "16:9", "stretch" };
    int i;

    for (i = 0; i < NV2A_VK_ASPECTS; i++)
        if (strcmp(name, names[i]) == 0) {
            nv2a_vk_aspect((Nv2aVkAspect)i);
            return;
        }
    fprintf(stderr, "[NV2A-VK] no aspect ratio %s: auto, 4:3, 16:9 or stretch\n", name);
}

static void present(const Nv2aPresent *p)
{
    uint32_t scale, samples;

    freed_surfaces();
    if (g.mode == NV2A_VK_COPYBACK_TOUCH)
        store_untracked();
    present_picture(p);
    g.surf = NULL;                      /* look at guest memory again */
    g.frame++;
    textures_trim();
    scale = scale_wanted();
    samples = samples_wanted();
    if ((g.scale != scale || g.samples != samples) && !g.lost)
        rescale(scale, samples);
    report();
}

/* ---- the title looks (RECOMP_NV2A_VK_COPYBACK=touch) -------------------- */

/* Who touched which surface, said once each with RECOMP_NV2A_VK_STATS. */
static void touch_list(const GuestTouch *t)
{
    static struct { uintptr_t code; uint32_t va, w, h; int write; } seen[256];
    static uint32_t n;
    const char *e = getenv("RECOMP_NV2A_VK_STATS");
    const Surface *s = NULL;
    char name[128];
    uint32_t i;

    if (!e || strcmp(e, "0") == 0)
        return;
    for (i = 0; i < MAX_SURFACES; i++)          /* the one it is in, or beside */
        if (s_surfaces[i].guard != GUEST_OPEN
            && overlaps(&s_surfaces[i], t->va & ~0xFFFu, t->bytes + 0x1000u)
            && (!s || overlaps(&s_surfaces[i], t->va, t->bytes)))
            s = &s_surfaces[i];
    if (!s)
        return;
    for (i = 0; i < n; i++)
        if (seen[i].code == t->code && seen[i].va == s->va && seen[i].w == s->w
            && seen[i].h == s->h && seen[i].write == t->write)
            return;
    if (n == sizeof seen / sizeof seen[0])
        return;
    seen[n].code = t->code;
    seen[n].va = s->va;
    seen[n].w = s->w;
    seen[n].h = s->h;
    seen[n++].write = t->write;
    guest_guard_name(t->code, name, sizeof name);
    fprintf(stderr, "[NV2A-VK] the title %s 0x%08X, %s the surface 0x%08X (%ux%u, %u bytes a"
            " pixel%s): %s\n", t->write ? "writes" : "reads", t->va,
            overlaps(s, t->va, t->bytes) ? "in" : "beside", s->va, s->w, s->h, s->bpp,
            s->dirty ? "" : ", not drawn since", name);
}

static void touched(const GuestTouch *t)
{
    freed_surfaces();
    touch_list(t);
    s_stats.touches++;
    if (t->bytes == XBOX_CONTIG_SIZE) {         /* opened unasked: nothing is known */
        uint32_t i;
        for (i = 0; i < MAX_SURFACES; i++)
            s_surfaces[i].seen = 0;
    }
    pages_open(t->va, t->bytes, t->write);
}

static int vk_touched(const GuestTouch *t);

/* The mode as asked for: what is guarded, and what is known of it. */
static void mode_settle(void)
{
    uint32_t i;

    if (g.copyback == NV2A_VK_COPYBACK_TOUCH && !g.window) {
        g.window = guest_guard_start(vk_touched);
        if (!g.window) {
            fprintf(stderr, "[NV2A-VK] pages cannot be guarded here: copy-back always\n");
            g.copyback = NV2A_VK_COPYBACK_ALWAYS;
        }
    }
    g.mode = g.copyback;
    for (i = 0; i < MAX_SURFACES; i++)
        if (g.mode != NV2A_VK_COPYBACK_TOUCH)
            surface_guard(&s_surfaces[i], GUEST_OPEN);
        else if (s_surfaces[i].dirty)
            surface_watch(&s_surfaces[i]);
}

/* From the executor: in, and out. */
static void enter(void)
{
    guest_guard_own();
    g.busy = 1;
    g.part = nv2a_perf_part(NV2A_PERF_DRAWS);
    if (g.mode != g.copyback)
        mode_settle();
}

static void leave(void)
{
    nv2a_perf_part(g.part);
    g.busy = 0;
}

static int vk_touched(const GuestTouch *t)
{
    if (g.busy) {
        /* Our own code, in spite of guest(), or the executor for us: no place
         * to copy back. The pages are opened, and looked at again. */
        uint32_t i;
        for (i = 0; i < MAX_SURFACES; i++)
            if (overlaps(&s_surfaces[i], t->va & ~0xFFFu, 0x1000u))
                surface_guard(&s_surfaces[i], GUEST_OPEN);
        return 0;
    }
    enter();
    touched(t);
    leave();
    return 1;
}

static int vk_draw(const Nv2aRawBatch *b)
{
    int drawn;

    enter();
    drawn = draw(b);
    leave();
    return drawn;
}

static int vk_clear(const Nv2aTarget *t, const Nv2aRenderState *rs, const Nv2aRawClear *c)
{
    int cleared;

    enter();
    cleared = clear(t, rs, c);
    leave();
    return cleared;
}

static void vk_report_clear(void)
{
    enter();
    count_end();
    answer_push(ANSWER_CLEAR, NULL);
    leave();
}

static int vk_report(const Nv2aReport *r)
{
    int taken;

    enter();
    count_end();
    taken = guest_writable(r->va, 16) && answer_push(ANSWER_REPORT, r);
    leave();
    return taken;
}

/* The title may be waiting for what has not been submitted. */
static void vk_idle(void)
{
    enter();
    if (g.answer_at > g.submits) {
        if (g.f->used + 1 < FRAME_SUBMITS) {
            submit_commands(0);
            s_stats.early++;
        } else {
            flush();
        }
    }
    leave();
}

/* RECOMP_NV2A_VK_PRESENT=0: the toolkit's window shows the picture from guest
 * memory, where everything drawn is put at each flip whatever the copy-back
 * mode. For timing Vulkan's present against it. */
static int s_window_shows;

static int vk_present(const Nv2aPresent *p)
{
    enter();
    if (s_window_shows) {
        Nv2aPresent none = { 0, 0, 1, 0, 0 };

        if (g.mode != NV2A_VK_COPYBACK_ALWAYS) {
            store_range(0, 0x100000000ull, NULL);
            flush();
        }
        present(&none);
        leave();
        return 0;
    }
    present(p);
    leave();
    return 1;                           /* the window has it, or there is none */
}

void nv2a_vk_store(void)
{
    enter();
    store_range(0, 0x100000000ull, NULL);
    leave();
}

void nv2a_vk_wait(void)
{
    flush();
    nv2a_vk_present_wait();
    nv2a_vk_present_lock(1);
    vkDeviceWaitIdle(g.dev);
    nv2a_vk_present_unlock(1);
    frames_wait();
    answers_wait(0);
}

/* ---- bring-up ----------------------------------------------------------- */

static int has_extension(VkPhysicalDevice phys, const char *name)
{
    VkExtensionProperties *list;
    uint32_t n = 0, i;
    int found = 0;

    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    list = (VkExtensionProperties *)calloc(n + 1, sizeof *list);
    if (!list)
        return 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, list);
    for (i = 0; i < n; i++)
        found |= strcmp(list[i].extensionName, name) == 0;
    free(list);
    return found;
}

static int create_instance(void)
{
    static const char *const layer = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *e = getenv("RECOMP_NV2A_VK_VALIDATE");

    app.apiVersion = VK_API_VERSION_1_3;
    ci.pApplicationInfo = &app;
    ci.ppEnabledExtensionNames = nv2a_vk_present_instance_extensions(&ci.enabledExtensionCount);
    if (e && strcmp(e, "0") != 0) {     /* the layer logs to stdout by itself */
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ci, NULL, &g.instance) == VK_SUCCESS)
        return 1;
    if (!ci.enabledLayerCount)
        return 0;
    fprintf(stderr, "[NV2A-VK] no validation layer on this host\n");
    ci.enabledLayerCount = 0;
    return vkCreateInstance(&ci, NULL, &g.instance) == VK_SUCCESS;
}

typedef struct {
    VkPhysicalDeviceFeatures2 f;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border;
    VkPhysicalDeviceLineRasterizationFeaturesEXT lines;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT parts;
} Features;

typedef struct { int border, smooth_lines, parts; } Extras;

static void features_chain(Features *x)
{
    memset(x, 0, sizeof *x);
    x->f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    x->v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    x->v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    x->border.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
    x->lines.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
    x->parts.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    x->f.pNext = &x->v12;
    x->v12.pNext = &x->v13;
}

/* What the shaders and the recording here need; 0 if one is missing. */
static int usable(VkPhysicalDevice phys, Features *want, Extras *extra)
{
    Features have;
    VkPhysicalDeviceProperties p;

    vkGetPhysicalDeviceProperties(phys, &p);
    if (p.apiVersion < VK_API_VERSION_1_3)
        return 0;
    features_chain(&have);
    extra->border = has_extension(phys, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    extra->smooth_lines = has_extension(phys, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME);
    if (extra->border)
        have.v13.pNext = &have.border;
    if (extra->smooth_lines) {
        have.lines.pNext = have.v13.pNext;
        have.v13.pNext = &have.lines;
    }
    extra->parts = has_extension(phys, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME)
                && has_extension(phys, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    if (extra->parts) {
        have.parts.pNext = have.v13.pNext;
        have.v13.pNext = &have.parts;
    }
    vkGetPhysicalDeviceFeatures2(phys, &have.f);
    extra->parts = extra->parts && have.parts.graphicsPipelineLibrary;
    extra->border = extra->border && have.border.customBorderColors
                  && have.border.customBorderColorWithoutFormat;
    extra->smooth_lines = extra->smooth_lines && have.lines.smoothLines;
    if (!have.v12.runtimeDescriptorArray || !have.v12.descriptorBindingPartiallyBound
        || !have.v12.descriptorBindingSampledImageUpdateAfterBind
        || !have.v12.timelineSemaphore
        || !have.v13.dynamicRendering || !have.v13.synchronization2
        || !have.v13.shaderDemoteToHelperInvocation
        || !have.f.features.shaderSampledImageArrayDynamicIndexing
        || !have.f.features.textureCompressionBC)
        return 0;
    features_chain(want);
    want->v12.runtimeDescriptorArray = VK_TRUE;
    want->v12.descriptorBindingPartiallyBound = VK_TRUE;
    want->v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    want->v12.descriptorIndexing = have.v12.descriptorIndexing;
    want->v12.timelineSemaphore = VK_TRUE;
    want->v13.dynamicRendering = VK_TRUE;
    want->v13.synchronization2 = VK_TRUE;
    want->v13.shaderDemoteToHelperInvocation = VK_TRUE;
    want->f.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    want->f.features.textureCompressionBC = VK_TRUE;
    want->f.features.samplerAnisotropy = have.f.features.samplerAnisotropy;
    want->f.features.sampleRateShading = have.f.features.sampleRateShading;
    want->f.features.depthClamp = have.f.features.depthClamp;
    want->f.features.fillModeNonSolid = have.f.features.fillModeNonSolid;
    want->f.features.logicOp = have.f.features.logicOp;
    want->f.features.wideLines = have.f.features.wideLines;
    want->f.features.largePoints = have.f.features.largePoints;
    want->f.features.occlusionQueryPrecise = have.f.features.occlusionQueryPrecise;
    if (extra->border) {
        want->border.customBorderColors = VK_TRUE;
        want->border.customBorderColorWithoutFormat = VK_TRUE;
        want->v13.pNext = &want->border;
    }
    if (extra->smooth_lines) {
        want->lines.smoothLines = VK_TRUE;
        want->lines.pNext = want->v13.pNext;
        want->v13.pNext = &want->lines;
    }
    if (extra->parts) {
        want->parts.graphicsPipelineLibrary = VK_TRUE;
        want->parts.pNext = want->v13.pNext;
        want->v13.pNext = &want->parts;
    }
    return 1;
}

static int create_device(void)
{
    VkPhysicalDevice list[16];
    VkQueueFamilyProperties fam[32];
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    const char *ext[8];
    const char *const *more;
    uint32_t exts = 0, queues = 1;
    float one[2] = { 1.0f, 1.0f };
    Features want, chosen;
    uint32_t n = 16, i, k, q;
    Extras extra = { 0, 0, 0 };
    int best = -1;

    if (vkEnumeratePhysicalDevices(g.instance, &n, list) < 0)
        return 0;
    memset(&chosen, 0, sizeof chosen);
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        int score;

        if (!usable(list[i], &want, &extra))
            continue;
        more = nv2a_vk_present_device_extensions(&q);
        for (k = 0; k < q && has_extension(list[i], more[k]); k++)
            ;
        if (k < q)
            continue;
        vkGetPhysicalDeviceProperties(list[i], &p);
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        q = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &q, fam);
        for (k = 0; k < q; k++)
            if ((fam[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                && nv2a_vk_present_can(g.instance, list[i], k))
                break;
        if (k == q || score <= best)
            continue;
        best = score;
        g.phys = list[i];
        g.family = k;
        g.props = p;
    }
    if (best < 0 || !usable(g.phys, &chosen, &extra))
        return 0;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    qi.queueFamilyIndex = g.family;
    if (extra.border)
        ext[exts++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    if (extra.smooth_lines)
        ext[exts++] = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
    if (extra.parts) {
        ext[exts++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[exts++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
    }
    more = nv2a_vk_present_device_extensions(&n);
    for (i = 0; i < n; i++)
        ext[exts++] = more[i];
    q = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &q, fam);
    if (fam[g.family].queueCount > 1)
        queues = 2;
    qi.queueCount = queues;
    qi.pQueuePriorities = one;
    ci.pNext = &chosen.f;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = exts;
    ci.ppEnabledExtensionNames = ext;
    if (vkCreateDevice(g.phys, &ci, NULL, &g.dev) != VK_SUCCESS
        || !nv2a_vk_api_load_device(g.dev))
        return 0;
    g.has_border = extra.border;
    g.has_smooth_lines = extra.smooth_lines;
    g.has_parts = extra.parts;
    g.has_exact_counts = chosen.f.features.occlusionQueryPrecise != 0;
    g.has_logic_ops = chosen.f.features.logicOp;
    g.has_sample_shading = chosen.f.features.sampleRateShading;
    g.has_wide_lines = chosen.f.features.wideLines != 0;
    g.has_fill_modes = chosen.f.features.fillModeNonSolid != 0;
    g.has_aniso = chosen.f.features.samplerAnisotropy != 0;
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    g.show_queue = g.queue;
    if (queues > 1)
        vkGetDeviceQueue(g.dev, g.family, 1, &g.show_queue);
    for (i = 0; i < NV2A_VK_VIEWS; i++) {
        VkFormatProperties p;

        vkGetPhysicalDeviceFormatProperties(g.phys, view_format((Nv2aVkTexView)i), &p);
        g.view_ok[i] = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                    && (p.optimalTilingFeatures
                        & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    }
    return 1;
}

static int create_stream(void)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    VkDeviceMemory mem;

    ci.size = STREAM_BYTES;
    ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
             | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(g.dev, &ci, NULL, &g.stream) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(g.dev, g.stream, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(g.dev, &ai, NULL, &mem) != VK_SUCCESS)
        return 0;
    vkBindBufferMemory(g.dev, g.stream, mem, 0);
    return vkMapMemory(g.dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&g.stream_map) == VK_SUCCESS;
}

static int create_descriptors(void)
{
    static const uint32_t counts[6] = { 1, SLOTS_2D, SLOTS_3D, SLOTS_CUBE, SAMPLERS, 1 };
    VkDescriptorSetLayoutBinding b[6];
    VkDescriptorBindingFlags flags[6];
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf =
        { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize ps[3];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorPool pool;      /* bindings 0 and 5 are the same buffer */
    uint32_t i, k;

    memset(b, 0, sizeof b);
    for (i = 0; i < 6; i++) {
        b[i].binding = i;
        b[i].descriptorCount = counts[i];
        b[i].descriptorType = i == 0 || i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                            : i == 4 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[i].stageFlags = (i == 5 ? 0 : VK_SHADER_STAGE_FRAGMENT_BIT)
                        | (i == 0 || i == 5 ? VK_SHADER_STAGE_VERTEX_BIT : 0);
        flags[i] = i == 0 || i == 5 ? 0 : VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                                        | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    bf.bindingCount = 6;
    bf.pBindingFlags = flags;
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 6;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(g.dev, &li, NULL, &g.set_layout) != VK_SUCCESS)
        return 0;
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[0].descriptorCount = 2;
    ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ps[1].descriptorCount = SLOTS_2D + SLOTS_3D + SLOTS_CUBE;
    ps[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    ps[2].descriptorCount = SAMPLERS;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(g.dev, &pi, NULL, &pool) != VK_SUCCESS)
        return 0;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g.set_layout;
    if (vkAllocateDescriptorSets(g.dev, &ai, &g.set) != VK_SUCCESS)
        return 0;
    bi.buffer = g.stream;
    bi.offset = 0;
    bi.range = VK_WHOLE_SIZE;
    w.dstSet = g.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    w.dstBinding = 5;
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(g.dev, &pl, NULL, &g.layout) != VK_SUCCESS)
        return 0;
    for (k = 1; k <= 3; k++) {
        s_free_slots[k] = (uint32_t *)malloc(counts[k] * sizeof(uint32_t));
        if (!s_free_slots[k])
            return 0;
        for (i = 0; i < counts[k]; i++)
            s_free_slots[k][i] = counts[k] - 1 - i;
        s_free_count[k] = counts[k];
    }
    return 1;
}

static int create_shaders(void)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

    ci.codeSize = sizeof nv2a_vk_vert_spv;
    ci.pCode = nv2a_vk_vert_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.vert) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_prog_spv;
    ci.pCode = nv2a_vk_prog_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.prog) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_w_vert_spv;
    ci.pCode = nv2a_vk_w_vert_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.w_vert) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_w_prog_spv;
    ci.pCode = nv2a_vk_w_prog_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.w_prog) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_frag_spv;
    ci.pCode = nv2a_vk_frag_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.frag) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_depth_frag_spv;
    ci.pCode = nv2a_vk_depth_frag_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.depth_frag) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_cutout_frag_spv;
    ci.pCode = nv2a_vk_cutout_frag_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.cutout_frag) != VK_SUCCESS)
        return 0;
    if (g.has_sample_shading) {
        ci.codeSize = sizeof nv2a_vk_each_frag_spv;
        ci.pCode = nv2a_vk_each_frag_spv;
        if (vkCreateShaderModule(g.dev, &ci, NULL, &g.each_frag) != VK_SUCCESS)
            return 0;
        ci.codeSize = sizeof nv2a_vk_each_depth_frag_spv;
        ci.pCode = nv2a_vk_each_depth_frag_spv;
        if (vkCreateShaderModule(g.dev, &ci, NULL, &g.each_depth_frag) != VK_SUCCESS)
            return 0;
    }
    ci.codeSize = sizeof nv2a_vk_rearrange_vert_spv;
    ci.pCode = nv2a_vk_rearrange_vert_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.rearrange_vert) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_rearrange_frag_spv;
    ci.pCode = nv2a_vk_rearrange_frag_spv;
    if (vkCreateShaderModule(g.dev, &ci, NULL, &g.rearrange_frag) != VK_SUCCESS)
        return 0;
    ci.codeSize = sizeof nv2a_vk_depths_frag_spv;
    ci.pCode = nv2a_vk_depths_frag_spv;
    return vkCreateShaderModule(g.dev, &ci, NULL, &g.depths_frag) == VK_SUCCESS;
}

static int create_commands(void)
{
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    const char *e = getenv("RECOMP_NV2A_VK_FRAMES_IN_FLIGHT");
    uint32_t i;

    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    si.pNext = &ti;
    if (vkCreateSemaphore(g.dev, &si, NULL, &g.timeline) != VK_SUCCESS)
        return 0;

    g.frame_count = e ? (uint32_t)atoi(e) : 2;
    if (g.frame_count < 1 || g.frame_count > MAX_FRAMES)
        g.frame_count = 2;
    pi.queueFamilyIndex = g.family;
    for (i = 0; i < g.frame_count; i++) {
        Frame *f = &g.frames[i];

        if (vkCreateCommandPool(g.dev, &pi, NULL, &f->pool) != VK_SUCCESS)
            return 0;
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2 * FRAME_SUBMITS;
        if (vkAllocateCommandBuffers(g.dev, &ai, f->list[0]) != VK_SUCCESS
            || vkCreateFence(g.dev, &fi, NULL, &f->fence) != VK_SUCCESS)
            return 0;
        if (g.props.limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };

            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 * FRAME_SUBMITS;
            if (vkCreateQueryPool(g.dev, &qi, NULL, &f->clock) != VK_SUCCESS)
                f->clock = VK_NULL_HANDLE;
        }
    }
    frame_use(0);
    return 1;
}

static const Nv2aBatchBackend s_batches = {
    NV2A_BATCH_VERSION, vk_draw, vk_clear, vk_report_clear, vk_report, vk_idle,
    nv2a_vk_memory_freed
};
static const Nv2aBackend s_rest;        /* only for present_sync */

int nv2a_vk_init(void)
{
    const char *failed = NULL, *e;

    if (g.frame)
        return 1;
    (void)nv2a_vk_pipeline_cache_path();    /* before there are threads */
    if (!nv2a_vk_api_load())                failed = "no Vulkan loader";
    else if (!create_instance())            failed = "no Vulkan instance";
    else if (!nv2a_vk_api_load_instance(g.instance)) failed = "an incomplete driver";
    else if (!create_device())              failed = "no device with what it needs";
    else if (!create_commands())            failed = "no command buffers";
    else if (!create_stream())              failed = "no memory for vertices";
    else if (!create_descriptors())         failed = "no descriptors";
    else if (!create_shaders())             failed = "no shaders";
    else if (!nv2a_backend_register_batches(&s_batches)) failed = "another contract version";
    if (failed) {
        fprintf(stderr, "[NV2A-VK] not used: %s\n", failed);
        return 0;
    }
    g.frame = 1;
    cache_open();
    e = getenv("RECOMP_NV2A_VK_PRESENT");
    s_window_shows = e && strcmp(e, "0") == 0;
    if (s_window_shows)
        fprintf(stderr, "[NV2A-VK] RECOMP_NV2A_VK_PRESENT=0: the window shows guest memory\n");
    else
        nv2a_vk_present_start(g.instance, g.phys, g.dev, g.family, g.show_queue,
                              g.show_queue == g.queue, g.timeline);
    e = getenv("RECOMP_NV2A_VK_STATS");
    s_draw_clock = e && strcmp(e, "0") != 0;
    e = getenv("RECOMP_NV2A_VK_SCALE");
    if (e)
        nv2a_vk_scale(strcmp(e, "fit") ? atoi(e) : NV2A_VK_SCALE_FIT);
    if (s_scale != NV2A_VK_SCALE_FIT)
        g.scale = (uint32_t)s_scale;
    e = getenv("RECOMP_NV2A_VK_ANISO");
    if (e)
        nv2a_vk_anisotropy(strcmp(e, "title") ? atoi(e) : 1);
    e = getenv("RECOMP_NV2A_VK_ASPECT");
    if (e)
        aspect_named(e);
    e = getenv("RECOMP_NV2A_VK_AA");
    if (e)
        aa_named(e);
    g.samples = samples_wanted();
    e = getenv("RECOMP_NV2A_VK_ALPHA_TEST_AA");
    if (e)
        alpha_test_aa_named(e);
    e = getenv("RECOMP_NV2A_VK_SHADER_COMPILE");
    if (e)
        shader_compile_named(e);
    e = getenv("RECOMP_NV2A_VK_COPYBACK");
    if (e && strcmp(e, "never") == 0)
        g.copyback = NV2A_VK_COPYBACK_NEVER;
    else if (e && strcmp(e, "always") == 0)
        g.copyback = NV2A_VK_COPYBACK_ALWAYS;
    else if (e && strcmp(e, "touch") == 0)
        g.copyback = NV2A_VK_COPYBACK_TOUCH;
    else if (e)
        fprintf(stderr, "[NV2A-VK] no copy-back mode %s: always, touch or never\n", e);
    else if (getenv("RECOMP_FRAME_CAPTURE"))    /* it saves guest memory as it is */
        g.copyback = NV2A_VK_COPYBACK_ALWAYS;
    nv2a_backend_register_with_present_sync(&s_rest, nv2a_vk_store);
    nv2a_backend_set_present(vk_present);
    fprintf(stderr, "[NV2A-VK] drawing on %s\n", g.props.deviceName);
    return 1;
}

void nv2a_vk_init_from_env(void)
{
    const char *e = getenv("RECOMP_NV2A_VK");

    if (!e || strcmp(e, "0") == 0)
        return;
    if (!nv2a_vk_present_possible())
        fprintf(stderr, "[NV2A-VK] not used: no window output on this host\n");
    else
        nv2a_vk_init();
}

void nv2a_vk_cpu_vertices(int on) { g.cpu_vertices = on; }
void nv2a_vk_textures_via_ram(int on) { g.textures_via_ram = on; }

void nv2a_vk_copyback(Nv2aVkCopyback mode) { g.copyback = mode; }

void nv2a_vk_stats(Nv2aVkStats *out)
{
    *out = s_stats;
    out->specialized_pipelines = (unsigned)s_specialized_done;
    out->specialized_due = (unsigned)(s_specialized_count - s_specialized_done);
}
