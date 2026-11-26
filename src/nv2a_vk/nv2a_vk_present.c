/*
 * The window's swap chain (nv2a_vk_present.h). The thread owns it and does its
 * own submissions, on a queue of its own where the card has a second one, so
 * the executor never waits for the display.
 */
#if defined(_WIN32)
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_vk_present.h"

Nv2aVkAspect nv2a_vk_present_aspect(Nv2aVkAspect asked, int wide)
{
    if (asked != NV2A_VK_ASPECT_AUTO)
        return asked;
    return wide ? NV2A_VK_ASPECT_16_9 : NV2A_VK_ASPECT_4_3;
}

void nv2a_vk_present_fit(uint32_t win_w, uint32_t win_h, Nv2aVkAspect aspect, uint32_t h,
                         uint32_t rows, VkOffset3D to[2], uint32_t *from_h)
{
    uint32_t w = win_w, full = win_h, x = 4, y = 3;

    if (aspect == NV2A_VK_ASPECT_16_9) {
        x = 16;
        y = 9;
    }
    if (aspect == NV2A_VK_ASPECT_STRETCH)
        ;
    else if ((uint64_t)win_w * y > (uint64_t)win_h * x)
        w = (uint32_t)(((uint64_t)win_h * x + y / 2) / y);
    else
        full = (uint32_t)(((uint64_t)win_w * y + x / 2) / x);
    *from_h = h < rows ? h : rows;
    memset(to, 0, 2 * sizeof *to);
    to[0].x = (int32_t)((win_w - w) / 2);
    to[0].y = (int32_t)((win_h - full) / 2);
    to[1].x = to[0].x + (int32_t)w;
    to[1].y = to[0].y + (int32_t)((uint64_t)full * *from_h / rows);
    to[1].z = 1;
}

int nv2a_vk_present_fill(uint32_t w, uint32_t h)
{
    uint32_t n = w / 640 + (w % 640 != 0), m = h / 480 + (h % 480 != 0);

    n = n > m ? n : m;
    return n < 1 ? 1 : n > NV2A_VK_SCALES ? NV2A_VK_SCALES : (int)n;
}

/* As asked for; -1: the environment has not been looked at. */
static volatile int s_vsync = -1, s_filter = -1;

static const char *const s_vsyncs[4] = { "mailbox", "fifo", "adaptive", "immediate" };

void nv2a_vk_present_vsync(int mode)
{
    if (mode >= 0 && mode < 4)
        s_vsync = mode;
}

int nv2a_vk_present_vsync_is(void)
{
    if (s_vsync < 0) {
        const char *e = getenv("RECOMP_PRESENT_VSYNC");
        int i, mode = e && *e == '0' ? 3 : 1;

        for (i = 0; e && i < 4; i++)
            if (strcmp(e, s_vsyncs[i]) == 0)
                mode = i;
        s_vsync = mode;
    }
    return s_vsync;
}

void nv2a_vk_present_filter(int filter) { s_filter = filter != 0; }

int nv2a_vk_present_filter_is(void)
{
    if (s_filter < 0) {
        const char *e = getenv("RECOMP_PRESENT_FILTER");

        s_filter = e && (*e == 'p' || *e == 'P');
    }
    return s_filter;
}

#if defined(_WIN32)

#include <vulkan/vulkan_win32.h>
#include "kernel.h"
#include "nv2a_perf.h"
#include "platform/host_time.h"
#include "platform/recomp_exit_hook.h"
#include "../video/video_player.h"
#if defined(XBOXRECOMP_IMGUI)
#include "nv2a_vk_menu.h"
#include "nv2a_vk_panel.h"
#endif

#define PICTURES 6
#define SLOTS    2              /* of the thread's, in flight */
#define IMAGES   8              /* of the swap chain, at most */
#define DUE_MS   100u
#define QUIET_MS 8u             /* looks for the video overlay's frames */
#define OVER_W   640u           /* what that is drawn over */
#define OVER_H   480u
#define PANEL_MS 500u           /* the panel is drawn again, with no new picture */
#define MENU_MS  15u            /* and the menu */

#define CHAIN_INSTANCE(F) \
    F(vkCreateWin32SurfaceKHR) F(vkDestroySurfaceKHR) F(vkGetPhysicalDeviceSurfaceSupportKHR) \
    F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) F(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    F(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define CHAIN_DEVICE(F) \
    F(vkCreateSwapchainKHR) F(vkDestroySwapchainKHR) F(vkGetSwapchainImagesKHR) \
    F(vkAcquireNextImageKHR) F(vkQueuePresentKHR) F(vkQueueWaitIdle) \
    F(vkCmdClearColorImage) F(vkUnmapMemory) F(vkGetSemaphoreCounterValue)
#define DECLARE(name) static PFN_##name name;
CHAIN_INSTANCE(DECLARE)
CHAIN_DEVICE(DECLARE)

/* FREE: anyone's. TAKEN: the executor's. QUEUED: to be shown. HELD: the
 * thread's. SHOWN: in the window, kept to draw it again. RETIRED: done with
 * once the GPU is past its copy and what showed it. */
enum { FREE, TAKEN, QUEUED, HELD, SHOWN, RETIRED };

typedef struct {
    int      state;
    VkImage  image;
    VkDeviceMemory mem;
    VkFormat format;
    uint32_t w, h, rows, due, frame;
    int      has_due;
    uint64_t number, blit, order;       /* the last of each; blit 0: none */
} Picture;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
} Slot;

static struct {
    int opened;
    volatile LONG closed;       /* the window; the title goes on */
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t family;
    VkQueue queue;
    int shared;
    VkSemaphore timeline;
    VkPhysicalDeviceMemoryProperties mem;

    HWND window;
    VkSurfaceKHR surface;
    VkSurfaceFormatKHR format;
    VkPresentModeKHR mode;
    int vsync;                  /* what the mode was chosen for */
    VkSwapchainKHR chain;
    VkExtent2D size;
    VkImage image[IMAGES];
    VkImageView view[IMAGES];   /* for the panel */
    VkSemaphore done[IMAGES];   /* the picture is in that image */
    uint32_t images;
    int can_read;               /* its images, for watch */

    Slot slot[SLOTS];
    uint32_t next_slot;
    VkSemaphore blits;          /* the number of the last show() done */
    uint64_t blit;
    HANDLE ready, idle;

    /* A picture with the video overlay, put together by the CPU. */
    Picture over;
    VkBuffer over_buffer;
    VkDeviceMemory over_mem;
    uint32_t *over_px;
    Slot over_slot;
    int over_made;              /* -1: cannot be */
} p;

static Picture s_pic[PICTURES];
static uint64_t s_order;
static SRWLOCK s_lock = SRWLOCK_INIT;           /* the pictures and the stats */
static SRWLOCK s_queue_lock = SRWLOCK_INIT;
static Nv2aVkPresentStats s_stats;
static void (*volatile s_watch)(const uint32_t *, uint32_t, uint32_t);
static volatile unsigned s_watch_every;
static volatile LONG s_fill = 1;        /* nv2a_vk_present_scale() */

/* ---- what it needs ------------------------------------------------------- */

int nv2a_vk_present_possible(void) { return 1; }

int nv2a_vk_present_can(VkInstance instance, VkPhysicalDevice phys, uint32_t family)
{
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR can =
        (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");

    return can && can(phys, family);
}

const char *const *nv2a_vk_present_instance_extensions(uint32_t *n)
{
    static const char *const list[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                        VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    *n = 2;
    return list;
}

const char *const *nv2a_vk_present_device_extensions(uint32_t *n)
{
    static const char *const list[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    *n = 1;
    return list;
}

void nv2a_vk_present_lock(int always)
{
    if (always || p.shared)
        AcquireSRWLockExclusive(&s_queue_lock);
}

void nv2a_vk_present_unlock(int always)
{
    if (always || p.shared)
        ReleaseSRWLockExclusive(&s_queue_lock);
}

/* Nothing else shows the pictures: the process ends, as when its window is
 * closed (fb_present.c). */
static void stop(const char *why)
{
    fprintf(stderr, "[NV2A-VK] cannot show the pictures: %s\n", why);
    recomp_exit_hook_run("vulkan window");
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
}

void nv2a_vk_present_watch(void (*fn)(const uint32_t *px, uint32_t w, uint32_t h),
                           unsigned every)
{
    s_watch_every = every ? every : 1;
    s_watch = fn;
}

static void dump(const uint32_t *px, uint32_t w, uint32_t h)
{
    static unsigned n;
    uint32_t head[13] = { 0 };
    char name[512];
    FILE *f;

    snprintf(name, sizeof name, "%s%04u.bmp", getenv("RECOMP_NV2A_VK_PRESENT_DUMP"), n++);
    f = fopen(name, "wb");
    if (!f)
        return;
    head[0] = 54 + w * h * 4;
    head[2] = 54;
    head[3] = 40;
    head[4] = w;
    head[5] = (uint32_t)-(int32_t)h;
    head[6] = 1u | 32u << 16;
    fwrite("BM", 1, 2, f);
    fwrite(head, 4, 13, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* ---- the swap chain ------------------------------------------------------ */

static uint32_t memory_for(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < p.mem.memoryTypeCount; i++)
        if ((bits >> i & 1u) && (p.mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Every slot's work is past. */
static void slots_wait(void)
{
    uint32_t i;

    for (i = 0; i < SLOTS; i++)
        vkWaitForFences(p.dev, 1, &p.slot[i].fence, VK_TRUE, UINT64_MAX);
}

/* The window's size now; 0 when there is nothing to show in. */
static int window_size(VkSurfaceCapabilitiesKHR *c, VkExtent2D *size)
{
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p.phys, p.surface, c) != VK_SUCCESS)
        return -1;
    *size = c->currentExtent;
    if (size->width == UINT32_MAX) {
        RECT r;

        GetClientRect(p.window, &r);
        size->width = (uint32_t)(r.right - r.left);
        size->height = (uint32_t)(r.bottom - r.top);
    }
    return size->width && size->height;
}

/* One of the window's size. 0: none now (minimised). -1: not at all. */
static int chain_make(void)
{
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSurfaceCapabilitiesKHR c;
    VkSwapchainKHR old = p.chain;
    VkExtent2D size;
    VkResult r;
    uint32_t i, n = IMAGES;
    int ok = window_size(&c, &size);

    if (ok <= 0)
        return ok;
    if (old) {
        slots_wait();
        nv2a_vk_present_lock(1);
        vkQueueWaitIdle(p.queue);       /* past the presents' waits too */
        nv2a_vk_present_unlock(1);
    }
    for (i = 0; i < IMAGES; i++) {
        if (p.view[i])
            vkDestroyImageView(p.dev, p.view[i], NULL);
        p.view[i] = VK_NULL_HANDLE;
    }
    ci.surface = p.surface;
    ci.minImageCount = c.minImageCount < 3 ? 3 : c.minImageCount;
    if (c.maxImageCount && ci.minImageCount > c.maxImageCount)
        ci.minImageCount = c.maxImageCount;
    ci.imageFormat = p.format.format;
    ci.imageColorSpace = p.format.colorSpace;
    ci.imageExtent = size;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    p.can_read = (c.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (p.can_read)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.preTransform = c.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = p.mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    p.chain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(p.dev, &ci, NULL, &p.chain);
    if (old)
        vkDestroySwapchainKHR(p.dev, old, NULL);
    if (r != VK_SUCCESS) {
        p.chain = VK_NULL_HANDLE;
        return -1;
    }
    if (vkGetSwapchainImagesKHR(p.dev, p.chain, &n, p.image) != VK_SUCCESS)
        return -1;                      /* more than IMAGES, too */
    p.images = n;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = p.format.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    for (i = 0; i < n; i++) {
        vi.image = p.image[i];
        if (vkCreateImageView(p.dev, &vi, NULL, &p.view[i]) != VK_SUCCESS
            || (!p.done[i] && vkCreateSemaphore(p.dev, &si, NULL, &p.done[i]) != VK_SUCCESS))
            return -1;
    }
    p.size = size;
    AcquireSRWLockExclusive(&s_lock);
    s_stats.chains++;
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

/* ---- the thread ---------------------------------------------------------- */

static VkPipelineStageFlags2 stage_of(VkAccessFlags2 access)
{
    return access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
         ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags2 src, VkAccessFlags2 dst)
{
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

    b.srcStageMask = stage_of(src);
    b.dstStageMask = stage_of(dst);
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

#if defined(XBOXRECOMP_IMGUI)
#define DRAWN (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)

#define MENU 0x100

static int panel_mode(void) { return nv2a_vk_panel_mode() | (nv2a_vk_menu_open() ? MENU : 0); }

/* Over the image, left in COLOR_ATTACHMENT_OPTIMAL. */
static void panel_record(VkCommandBuffer cmd, uint32_t i, VkImageLayout now, VkAccessFlags2 wrote)
{
    VkRenderingAttachmentInfo a = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    Nv2aVkPanel panel;

    panel.instance = p.instance;
    panel.phys = p.phys;
    panel.dev = p.dev;
    panel.family = p.family;
    panel.queue = p.queue;
    panel.format = p.format.format;
    panel.in_flight = SLOTS;
    barrier(cmd, p.image[i], now, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, wrote, DRAWN);
    a.imageView = p.view[i];
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent = p.size;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &a;
    vkCmdBeginRendering(cmd, &ri);
    nv2a_vk_panel_draw(&panel, cmd, p.size.width, p.size.height);
    vkCmdEndRendering(cmd);
}

static void panel_stop(void)
{
    nv2a_vk_present_lock(1);
    vkQueueWaitIdle(p.queue);
    nv2a_vk_present_unlock(1);
    nv2a_vk_panel_stop();
}
#else
#define DRAWN 0
#define MENU  0

static int panel_mode(void) { return 0; }
static void panel_record(VkCommandBuffer cmd, uint32_t i, VkImageLayout now, VkAccessFlags2 wrote)
{
    (void)cmd; (void)i; (void)now; (void)wrote;
}
static void panel_stop(void) {}
#endif

/* What the window gets, for watch. */
typedef struct { VkBuffer buffer; VkDeviceMemory mem; void *map; } Read;

static int read_make(Read *r)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    memset(r, 0, sizeof *r);
    ci.size = (VkDeviceSize)p.size.width * p.size.height * 4;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(p.dev, &ci, NULL, &r->buffer) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(p.dev, r->buffer, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_for(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex != UINT32_MAX
        && vkAllocateMemory(p.dev, &ai, NULL, &r->mem) == VK_SUCCESS) {
        vkBindBufferMemory(p.dev, r->buffer, r->mem, 0);
        if (vkMapMemory(p.dev, r->mem, 0, VK_WHOLE_SIZE, 0, &r->map) == VK_SUCCESS)
            return 1;
        vkFreeMemory(p.dev, r->mem, NULL);
    }
    vkDestroyBuffer(p.dev, r->buffer, NULL);
    r->buffer = VK_NULL_HANDLE;
    return 0;
}

static void read_done(Read *r, Slot *s)
{
    uint32_t *px = (uint32_t *)r->map;
    uint32_t i, n = p.size.width * p.size.height;

    vkWaitForFences(p.dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
    if (p.format.format == VK_FORMAT_R8G8B8A8_UNORM)
        for (i = 0; i < n; i++)
            px[i] = (px[i] & 0xFF00FF00u) | (px[i] >> 16 & 0xFFu) | (px[i] & 0xFFu) << 16;
    s_watch(px, p.size.width, p.size.height);
    vkUnmapMemory(p.dev, r->mem);
    vkDestroyBuffer(p.dev, r->buffer, NULL);
    vkFreeMemory(p.dev, r->mem, NULL);
}

/* ---- the video overlay ---------------------------------------------------- */

static int picture_make(Picture *pic, VkFormat format, uint32_t w, uint32_t h);
static const char *slot_make(Slot *s);
static void choose_mode(void);

static void over_copy(VkBufferImageCopy *c)
{
    memset(c, 0, sizeof *c);
    c->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c->imageSubresource.layerCount = 1;
    c->imageExtent.width = OVER_W;
    c->imageExtent.height = OVER_H;
    c->imageExtent.depth = 1;
}

static int over_make(void)
{
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                     | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    if (p.over_made)
        return p.over_made > 0;
    p.over_made = -1;
    ci.size = OVER_W * OVER_H * 4;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (slot_make(&p.over_slot)
        || !picture_make(&p.over, VK_FORMAT_B8G8R8A8_UNORM, OVER_W, OVER_H)
        || vkCreateBuffer(p.dev, &ci, NULL, &p.over_buffer) != VK_SUCCESS)
        return 0;
    p.over.rows = OVER_H;
    vkGetBufferMemoryRequirements(p.dev, p.over_buffer, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_for(req.memoryTypeBits, need
                                                      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = memory_for(req.memoryTypeBits, need);
    if (ai.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(p.dev, &ai, NULL, &p.over_mem) != VK_SUCCESS
        || vkBindBufferMemory(p.dev, p.over_buffer, p.over_mem, 0) != VK_SUCCESS
        || vkMapMemory(p.dev, p.over_mem, 0, VK_WHOLE_SIZE, 0, (void **)&p.over_px) != VK_SUCCESS)
        return 0;
    p.over_made = 1;
    return 1;
}

/* The picture, or black, with the overlay over it: in over_px, for p.over.
 * 0: not possible, and the overlay is not drawn. */
static int over_draw(const Picture *pic)
{
    static const VkClearColorValue black = { { 0, 0, 0, 1 } };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkTimelineSemaphoreSubmitInfo ti = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkMemoryBarrier2 host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBufferImageCopy c;
    VkImageBlit b;
    Slot *s = &p.over_slot;
    VkResult r;

    if (!over_make()) {
        static uint32_t unseen[OVER_W * OVER_H];
        static int said;

        if (!said++)
            fprintf(stderr, "[NV2A-VK] the video overlay is not shown: no memory\n");
        xbox_FramebufferOverlay(unseen);        /* the title waits for its frames */
        return 0;
    }
    vkResetCommandPool(p.dev, s->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s->cmd, &bi);
    barrier(s->cmd, p.over.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(s->cmd, p.over.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &all);
    if (pic) {
        barrier(s->cmd, p.over.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        nv2a_vk_present_fit(OVER_W, OVER_H, NV2A_VK_ASPECT_4_3, pic->h, pic->rows, b.dstOffsets,
                            (uint32_t *)&b.srcOffsets[1].y);
        b.srcOffsets[1].x = (int32_t)pic->w;
        b.srcOffsets[1].z = 1;
        /* Nearest: the colour key is looked for. */
        vkCmdBlitImage(s->cmd, pic->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.over.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    barrier(s->cmd, p.over.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    over_copy(&c);
    vkCmdCopyImageToBuffer(s->cmd, p.over.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           p.over_buffer, 1, &c);
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(s->cmd, &d);
    vkEndCommandBuffer(s->cmd);
    if (pic) {
        ti.waitSemaphoreValueCount = 1;
        ti.pWaitSemaphoreValues = &pic->number;
        si.pNext = &ti;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &p.timeline;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cmd;
    /* The fence is past what was submitted before it too, which may still copy
     * from the buffer. */
    vkResetFences(p.dev, 1, &s->fence);
    nv2a_vk_present_lock(1);
    r = vkQueueSubmit(p.queue, 1, &si, s->fence);
    nv2a_vk_present_unlock(1);
    if (r != VK_SUCCESS || vkWaitForFences(p.dev, 1, &s->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return 0;                       /* lost: the next submission says so */
    xbox_FramebufferOverlay(p.over_px);
    return 1;
}

static Nv2aVkAspect aspect_now(void)
{
    return nv2a_vk_present_aspect(nv2a_vk_aspect_is(), xbox_DisplayIsWide());
}

int nv2a_vk_present_scale(void) { return p.opened && !p.closed ? (int)s_fill : 1; }

static void note_fill(void)
{
    VkOffset3D to[2];
    uint32_t from;

    nv2a_vk_present_fit(p.size.width, p.size.height,
                        aspect_now(), 480, 480, to, &from);
    InterlockedExchange(&s_fill, nv2a_vk_present_fill((uint32_t)(to[1].x - to[0].x),
                                                      (uint32_t)(to[1].y - to[0].y)));
}

static void record(Slot *s, const Picture *pic, uint32_t image, int panel, Read *read)
{
    VkImage to = p.image[image];
    static const VkClearColorValue black = { { 0, 0, 0, 1 } };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageLayout now = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAccessFlags2 wrote = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkImageBlit b;

    vkResetCommandPool(p.dev, s->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s->cmd, &bi);
    if (pic == &p.over) {
        VkBufferImageCopy c;

        barrier(s->cmd, pic->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_2_TRANSFER_READ_BIT, wrote);
        over_copy(&c);
        vkCmdCopyBufferToImage(s->cmd, p.over_buffer, pic->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(s->cmd, pic->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wrote, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    barrier(s->cmd, to, VK_IMAGE_LAYOUT_UNDEFINED, now, 0, wrote);
    vkCmdClearColorImage(s->cmd, to, now, &black, 1, &all);
    barrier(s->cmd, to, now, now, wrote, wrote);
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
    nv2a_vk_present_fit(p.size.width, p.size.height, aspect_now(), pic->h, pic->rows,
                        b.dstOffsets, (uint32_t *)&b.srcOffsets[1].y);
    b.srcOffsets[1].x = (int32_t)pic->w;
    b.srcOffsets[1].z = 1;
    if (b.dstOffsets[1].x > b.dstOffsets[0].x && b.dstOffsets[1].y > b.dstOffsets[0].y)
        vkCmdBlitImage(s->cmd, pic->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, now, 1, &b,
                       nv2a_vk_present_filter_is() ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (panel) {
        panel_record(s->cmd, image, now, wrote);
        now = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        wrote = DRAWN;
    }
    if (read) {
        VkBufferImageCopy c = {0};

        barrier(s->cmd, to, now, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wrote,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        now = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        wrote = 0;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = p.size.width;
        c.imageExtent.height = p.size.height;
        c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(s->cmd, to, now, read->buffer, 1, &c);
    }
    barrier(s->cmd, to, now, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, wrote, 0);
    vkEndCommandBuffer(s->cmd);
}

/* Puts a picture in the window, with the panel or not and the video overlay or
 * not; with that, it may be none. 0: not now. -1: never again. */
static int show(Picture *pic, int panel, int overlay, int watched)
{
    Slot *s = &p.slot[p.next_slot];
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkTimelineSemaphoreSubmitInfo ti = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkSemaphore waits[2], signals[2];
    VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT };
    uint64_t numbers[2] = { 0, pic ? pic->number : 0 }, after[2] = { 0, p.blit + 1 };
    uint32_t i = 0, tries;
    VkResult r;
    const Picture *from = pic;
    Read read;
    int reading;

    vkWaitForFences(p.dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
    if (overlay && over_draw(pic))
        from = &p.over;
    if (!from)
        return 0;
    for (tries = 0;; tries++) {
        if (!p.chain || tries) {
            int made = tries < 3 ? chain_make() : 0;

            if (made <= 0)
                return made;
        }
        r = vkAcquireNextImageKHR(p.dev, p.chain, 1000000000ull, s->acquired, VK_NULL_HANDLE, &i);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            break;
        if (r == VK_TIMEOUT || r == VK_NOT_READY)
            return 0;
        if (r != VK_ERROR_OUT_OF_DATE_KHR)
            return -1;
    }
    note_fill();
    reading = watched && s_watch && p.can_read && read_make(&read);
    vkResetFences(p.dev, 1, &s->fence);
    record(s, from, i, panel, reading ? &read : NULL);
    waits[0] = s->acquired;
    waits[1] = p.timeline;
    ti.waitSemaphoreValueCount = 2;
    ti.pWaitSemaphoreValues = numbers;
    ti.signalSemaphoreValueCount = 2;
    ti.pSignalSemaphoreValues = after;
    si.pNext = &ti;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cmd;
    signals[0] = p.done[i];
    signals[1] = p.blits;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = signals;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &p.done[i];
    pi.swapchainCount = 1;
    pi.pSwapchains = &p.chain;
    pi.pImageIndices = &i;
    nv2a_vk_present_lock(1);
    r = vkQueueSubmit(p.queue, 1, &si, s->fence);
    if (r == VK_SUCCESS)
        r = vkQueuePresentKHR(p.queue, &pi);
    nv2a_vk_present_unlock(1);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR)
        return -1;
    ++p.blit;
    if (pic)
        pic->blit = p.blit;
    if (reading)
        read_done(&read, s);
    p.next_slot = (p.next_slot + 1) % SLOTS;
    return 1;
}

/* Those the GPU is done with are free. With the lock. */
static void collect(void)
{
    uint64_t copies = 0, blits = 0;
    uint32_t i;

    if (!p.opened || vkGetSemaphoreCounterValue(p.dev, p.timeline, &copies) != VK_SUCCESS
        || vkGetSemaphoreCounterValue(p.dev, p.blits, &blits) != VK_SUCCESS)
        return;
    for (i = 0; i < PICTURES; i++)
        if (s_pic[i].state == RETIRED && s_pic[i].number <= copies && s_pic[i].blit <= blits)
            s_pic[i].state = FREE;
}

/* A vblank count that stopped, until it moves again. */
static int s_stopped;
static uint32_t s_stopped_at;

static int due(const Picture *pic)
{
    uint32_t now = xbox_VblankCount();

    return !pic->has_due || (int32_t)(now - pic->due) >= 0 || (s_stopped && now == s_stopped_at);
}

/* Until its vblank. */
static void wait_due(const Picture *pic)
{
    ULONGLONG from = GetTickCount64();

    while (!due(pic)) {
        if (GetTickCount64() - from >= DUE_MS) {
            s_stopped = 1;
            s_stopped_at = xbox_VblankCount();
            return;
        }
        xbox_HostSleepNs(100000u);
    }
}

static Picture *oldest(void)
{
    Picture *best = NULL;
    uint32_t i;

    for (i = 0; i < PICTURES; i++)
        if (s_pic[i].state == QUEUED && (!best || s_pic[i].order < best->order))
            best = &s_pic[i];
    return best;
}

/* The next to show, once it is time; those a later one has overtaken are
 * passed over. */
static Picture *next_picture(void)
{
    Picture *pic, *later;

    AcquireSRWLockExclusive(&s_lock);
    pic = oldest();
    if (pic)
        pic->state = HELD;
    ReleaseSRWLockExclusive(&s_lock);
    if (!pic)
        return NULL;
    wait_due(pic);
    for (;;) {
        AcquireSRWLockExclusive(&s_lock);
        later = oldest();
        if (!later || !due(later))
            break;
        later->state = HELD;
        pic->state = RETIRED;
        s_stats.passed_over++;
        ReleaseSRWLockExclusive(&s_lock);
        pic = later;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return pic;
}

static DWORD WINAPI present_thread(LPVOID unused)
{
    Picture *shown = NULL;
    unsigned count = 0;
    ULONGLONG at = 0;                   /* of the last show() */
    int was = 0;                        /* and the panel's mode then */
    Nv2aVkAspect aspect = NV2A_VK_ASPECT_AUTO;   /* and what was asked for */
    int filter = 0;

    (void)unused;
    for (;;) {
        Picture *pic;
        VkSurfaceCapabilitiesKHR c;
        VkExtent2D size;
        int ok, quiet, panel, overlay, again;

        AcquireSRWLockExclusive(&s_lock);
        quiet = !oldest();
        ReleaseSRWLockExclusive(&s_lock);
        if (quiet) {
            SetEvent(p.idle);
            WaitForSingleObject(p.ready, QUIET_MS);
        }
        pic = next_picture();
        ok = xbox_FramebufferWindowHandle() ? window_size(&c, &size) : -1;
        panel = panel_mode();
        overlay = xbox_FramebufferOverlayState();
        /* The last one again, with another panel or overlay or in a new size. */
        again = panel != was || overlay == 2
             || (panel && GetTickCount64() - at >= (panel & MENU ? MENU_MS : PANEL_MS))
             || aspect != nv2a_vk_aspect_is() || filter != nv2a_vk_present_filter_is();
        if (p.vsync != nv2a_vk_present_vsync_is()) {
            VkPresentModeKHR now = p.mode;

            choose_mode();
            if (ok > 0 && p.chain && p.mode != now) {
                ok = chain_make();
                again = 1;
            }
        }
        if (ok > 0 && p.chain && (size.width != p.size.width || size.height != p.size.height)) {
            ok = chain_make();
            again = 1;
        }
        if (!pic && (shown || overlay) && again && ok > 0) {
            aspect = nv2a_vk_aspect_is();
            filter = nv2a_vk_present_filter_is();
            ok = show(shown, panel, overlay, s_watch && s_watch_every == 1);
            at = GetTickCount64();
            was = panel;
            AcquireSRWLockExclusive(&s_lock);
            s_stats.redrawn += ok > 0;
            ReleaseSRWLockExclusive(&s_lock);
        }
        if (pic && ok > 0) {
            aspect = nv2a_vk_aspect_is();
            filter = nv2a_vk_present_filter_is();
            ok = show(pic, panel, overlay, s_watch && count++ % s_watch_every == 0);
            if (ok > 0)
                nv2a_perf_shown(pic->frame);
            at = GetTickCount64();
            was = panel;
        }
        if (pic) {
            AcquireSRWLockExclusive(&s_lock);
            if (ok > 0) {
                if (shown)
                    shown->state = RETIRED;
                pic->state = SHOWN;
                shown = pic;
                s_stats.shown++;
            } else {
                pic->state = RETIRED;
                s_stats.passed_over++;
            }
            ReleaseSRWLockExclusive(&s_lock);
        }
        if (ok < 0) {
            if (xbox_FramebufferWindowHandle())
                stop("the window or the card is lost");
            panel_stop();               /* closed, and the title goes on */
            InterlockedExchange(&p.closed, 1);
            SetEvent(p.idle);
            return 0;
        }
    }
}

/* ---- the executor's ------------------------------------------------------ */

void nv2a_vk_present_start(VkInstance instance, VkPhysicalDevice phys, VkDevice dev,
                           uint32_t family, VkQueue queue, int shared, VkSemaphore timeline)
{
    p.instance = instance;
    p.phys = phys;
    p.dev = dev;
    p.family = family;
    p.queue = queue;
    p.shared = shared;
    p.timeline = timeline;
    vkGetPhysicalDeviceMemoryProperties(phys, &p.mem);
    if (getenv("RECOMP_NV2A_VK_PRESENT_DUMP"))
        nv2a_vk_present_watch(dump, 120);
    /* A D3D11 swap chain would be in the way of one of Vulkan's. */
    xbox_FramebufferWindowSetExternal(2);
}

/* RECOMP_PRESENT_VSYNC, `modes` of the window's, n of them. Fifo: each picture
 * has a refresh of its own, so a title as fast as the display is even. */
VkPresentModeKHR nv2a_vk_present_mode(const char *e, const VkPresentModeKHR *modes, uint32_t n)
{
    VkPresentModeKHR want[2] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR };
    uint32_t k, i;

    if (e && (*e == '0' || strcmp(e, "immediate") == 0)) {
        want[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
        want[1] = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (e && strcmp(e, "mailbox") == 0) {
        want[0] = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (e && strcmp(e, "adaptive") == 0) {
        want[0] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    for (k = 0; k < 2; k++)
        for (i = 0; i < n; i++)
            if (modes[i] == want[k])
                return want[k];
    return VK_PRESENT_MODE_FIFO_KHR;
}

static const char *mode_name(VkPresentModeKHR mode)
{
    return mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
         : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
         : mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "adaptive" : "fifo";
}

static void choose_mode(void)
{
    VkPresentModeKHR m[16];
    uint32_t n = 16;

    if (vkGetPhysicalDeviceSurfacePresentModesKHR(p.phys, p.surface, &n, m) < 0)
        n = 0;
    p.vsync = nv2a_vk_present_vsync_is();
    p.mode = nv2a_vk_present_mode(s_vsyncs[p.vsync], m, n);
}

static const char *choose(void)
{
    VkSurfaceFormatKHR f[64];
    VkSurfaceCapabilitiesKHR c;
    uint32_t n = 64, i;

    if (vkGetPhysicalDeviceSurfaceFormatsKHR(p.phys, p.surface, &n, f) < 0 || !n)
        return "the window has no format";
    p.format.format = VK_FORMAT_UNDEFINED;
    for (i = 0; i < n; i++)
        if (f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            && (f[i].format == VK_FORMAT_B8G8R8A8_UNORM
                || (f[i].format == VK_FORMAT_R8G8B8A8_UNORM && !p.format.format)))
            p.format = f[i];
    if (!p.format.format)
        return "the window has no 8-bit format";
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p.phys, p.surface, &c) != VK_SUCCESS
        || !(c.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        || !(c.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        return "the window cannot be copied to";
    choose_mode();
    return NULL;
}

static const char *slot_make(Slot *s)
{
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    pi.queueFamilyIndex = p.family;
    if (vkCreateCommandPool(p.dev, &pi, NULL, &s->pool) != VK_SUCCESS)
        return "no command pool";
    ai.commandPool = s->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkAllocateCommandBuffers(p.dev, &ai, &s->cmd) != VK_SUCCESS
        || vkCreateFence(p.dev, &fi, NULL, &s->fence) != VK_SUCCESS
        || vkCreateSemaphore(p.dev, &si, NULL, &s->acquired) != VK_SUCCESS)
        return "no command buffer";
    return NULL;
}

static const char *open_window(void)
{
    VkWin32SurfaceCreateInfoKHR wi = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    VkBool32 can = VK_FALSE;
    const char *why;
    uint32_t i;
    HANDLE th;

#define NEED_I(name) \
    if (!(name = (PFN_##name)vkGetInstanceProcAddr(p.instance, #name))) \
        return "no " #name;
#define NEED_D(name) \
    if (!(name = (PFN_##name)vkGetDeviceProcAddr(p.dev, #name))) \
        return "no " #name;
    CHAIN_INSTANCE(NEED_I)
    CHAIN_DEVICE(NEED_D)
    wi.hinstance = GetModuleHandleA(NULL);
    wi.hwnd = p.window;
    if (vkCreateWin32SurfaceKHR(p.instance, &wi, NULL, &p.surface) != VK_SUCCESS
        || vkGetPhysicalDeviceSurfaceSupportKHR(p.phys, p.family, p.surface, &can) != VK_SUCCESS
        || !can)
        return "the card cannot draw in this window";
    why = choose();
    if (why)
        return why;
    for (i = 0; i < SLOTS; i++) {
        why = slot_make(&p.slot[i]);
        if (why)
            return why;
    }
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    si.pNext = &ti;
    if (vkCreateSemaphore(p.dev, &si, NULL, &p.blits) != VK_SUCCESS)
        return "no semaphore";
    if (chain_make() < 0)
        return "no swap chain";
    p.ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    p.idle = CreateEventA(NULL, FALSE, FALSE, NULL);
    th = p.ready && p.idle ? CreateThread(NULL, 0, present_thread, NULL, 0, NULL) : NULL;
    if (!th)
        return "no thread";
    CloseHandle(th);
    xbox_FramebufferWindowSetExternal(3);       /* with the video overlay */
    fprintf(stderr, "[NV2A-VK] presenting with Vulkan: %ux%u, %s, %s queue\n", p.size.width,
            p.size.height, mode_name(p.mode), p.shared ? "the only" : "its own");
    return NULL;
}

int nv2a_vk_present_on(void)
{
    const char *why;

    if (p.opened || p.closed)
        return !p.closed;
    p.window = (HWND)xbox_FramebufferWindowHandle();
    if (!p.window)
        return 0;
    p.opened = 1;
    why = open_window();
    if (why)
        stop(why);
    return 1;
}

static int picture_make(Picture *pic, VkFormat format, uint32_t w, uint32_t h)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    if (pic->image) {
        vkDestroyImage(p.dev, pic->image, NULL);
        vkFreeMemory(p.dev, pic->mem, NULL);
        pic->image = VK_NULL_HANDLE;
    }
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(p.dev, &ci, NULL, &pic->image) != VK_SUCCESS) {
        pic->image = VK_NULL_HANDLE;
        return 0;
    }
    vkGetImageMemoryRequirements(p.dev, pic->image, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_for(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(p.dev, &ai, NULL, &pic->mem) != VK_SUCCESS) {
        vkDestroyImage(p.dev, pic->image, NULL);
        pic->image = VK_NULL_HANDLE;
        return 0;
    }
    vkBindImageMemory(p.dev, pic->image, pic->mem, 0);
    pic->format = format;
    pic->w = w;
    pic->h = h;
    return 1;
}

int nv2a_vk_present_take(VkFormat format, uint32_t w, uint32_t h, VkImage *image)
{
    Picture *pic = NULL;
    uint32_t i;

    AcquireSRWLockExclusive(&s_lock);
    collect();
    for (i = 0; i < PICTURES; i++) {
        Picture *q = &s_pic[i];

        if (q->state != FREE)
            continue;
        if (!pic)
            pic = q;
        if (q->image && q->format == format && q->w == w && q->h == h) {
            pic = q;
            break;
        }
    }
    if (!pic) {
        /* The one waiting the longest gives way. Its image may still be copied
         * into, by commands ahead of the ones that will now. */
        pic = oldest();
        if (pic && (pic->format != format || pic->w != w || pic->h != h))
            pic = NULL;
        s_stats.passed_over++;
    }
    if (pic)
        pic->state = TAKEN;
    ReleaseSRWLockExclusive(&s_lock);
    if (!pic)
        return -1;
    if ((!pic->image || pic->format != format || pic->w != w || pic->h != h)
        && !picture_make(pic, format, w, h)) {
        nv2a_vk_present_drop((int)(pic - s_pic));
        return -1;
    }
    *image = pic->image;
    return (int)(pic - s_pic);
}

void nv2a_vk_present_show(int picture, uint64_t number, uint32_t rows, int has_due, uint32_t at,
                          uint32_t frame)
{
    Picture *pic = &s_pic[picture];

    AcquireSRWLockExclusive(&s_lock);
    pic->number = number;
    pic->blit = 0;
    pic->rows = rows ? rows : 1;
    pic->has_due = has_due;
    pic->due = at;
    pic->frame = frame;
    pic->order = ++s_order;
    pic->state = QUEUED;
    ReleaseSRWLockExclusive(&s_lock);
    SetEvent(p.ready);
}

void nv2a_vk_present_drop(int picture)
{
    AcquireSRWLockExclusive(&s_lock);
    s_pic[picture].number = s_pic[picture].blit = 0;
    s_pic[picture].state = FREE;
    ReleaseSRWLockExclusive(&s_lock);
}

void nv2a_vk_present_wait(void)
{
    while (p.opened && !p.closed) {
        int busy = 0;
        uint32_t i;

        AcquireSRWLockExclusive(&s_lock);
        for (i = 0; i < PICTURES; i++)
            busy |= s_pic[i].state == QUEUED || s_pic[i].state == HELD;
        ReleaseSRWLockExclusive(&s_lock);
        if (!busy)
            return;
        WaitForSingleObject(p.idle, 20);
    }
}

void nv2a_vk_present_stats(Nv2aVkPresentStats *out)
{
    AcquireSRWLockExclusive(&s_lock);
    *out = s_stats;
    ReleaseSRWLockExclusive(&s_lock);
}

#else   /* other hosts: not done yet */

int nv2a_vk_present_possible(void) { return 0; }
int nv2a_vk_present_can(VkInstance instance, VkPhysicalDevice phys, uint32_t family)
{
    (void)instance; (void)phys; (void)family;
    return 1;
}
const char *const *nv2a_vk_present_instance_extensions(uint32_t *n) { *n = 0; return NULL; }
const char *const *nv2a_vk_present_device_extensions(uint32_t *n) { *n = 0; return NULL; }
void nv2a_vk_present_start(VkInstance instance, VkPhysicalDevice phys, VkDevice dev,
                           uint32_t family, VkQueue queue, int shared, VkSemaphore timeline)
{
    (void)instance; (void)phys; (void)dev; (void)family; (void)queue; (void)shared;
    (void)timeline;
}
void nv2a_vk_present_lock(int always) { (void)always; }
void nv2a_vk_present_unlock(int always) { (void)always; }
int nv2a_vk_present_on(void) { return 0; }
int nv2a_vk_present_take(VkFormat format, uint32_t w, uint32_t h, VkImage *image)
{
    (void)format; (void)w; (void)h; (void)image;
    return -1;
}
void nv2a_vk_present_show(int picture, uint64_t number, uint32_t rows, int has_due, uint32_t due,
                          uint32_t frame)
{
    (void)picture; (void)number; (void)rows; (void)has_due; (void)due; (void)frame;
}
void nv2a_vk_present_drop(int picture) { (void)picture; }
int nv2a_vk_present_scale(void) { return 1; }
void nv2a_vk_present_wait(void) {}
void nv2a_vk_present_stats(Nv2aVkPresentStats *out) { memset(out, 0, sizeof *out); }
void nv2a_vk_present_watch(void (*fn)(const uint32_t *px, uint32_t w, uint32_t h),
                           unsigned every)
{
    (void)fn; (void)every;
}

#endif
