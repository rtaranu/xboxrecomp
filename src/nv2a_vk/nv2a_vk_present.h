/*
 * The Vulkan rasteriser's window output: each flip's picture is copied on the
 * card into one of a few images kept here, and a thread puts it in the window
 * on the vblank the flip is held for.
 *
 * All but the thread run on the executor's thread.
 */
#ifndef NV2A_VK_PRESENT_H
#define NV2A_VK_PRESENT_H

#include <stdint.h>
#include "nv2a_vk.h"
#include "nv2a_vk_api.h"

/* Whether there is any on this host; without it only tools draw. */
int nv2a_vk_present_possible(void);
/* What it needs of the instance and the device, *n of them. */
const char *const *nv2a_vk_present_instance_extensions(uint32_t *n);
const char *const *nv2a_vk_present_device_extensions(uint32_t *n);
/* Whether queues of this family can show in a window. */
int nv2a_vk_present_can(VkInstance instance, VkPhysicalDevice phys, uint32_t family);

/* `queue` is the thread's to use; `shared`, the rasteriser's too, which then
 * holds the lock below while it submits. `timeline` counts submissions done. */
void nv2a_vk_present_start(VkInstance instance, VkPhysicalDevice phys, VkDevice dev,
                           uint32_t family, VkQueue queue, int shared, VkSemaphore timeline);
void nv2a_vk_present_lock(int always);
void nv2a_vk_present_unlock(int always);

/* Whether there is a window. Opens its swap chain at the first call that finds
 * it. A window that cannot be shown in, then or later, ends the process: said. */
int nv2a_vk_present_on(void);

/* An image to copy a picture into, left in TRANSFER_SRC_OPTIMAL, or -1 when
 * all are in use. Then show, once its copy is in submission `number`: `rows`
 * of the image make the window's whole height; on vblank `due` if there is
 * one; `frame` as nv2a_perf_shown() has it. Or drop. */
int nv2a_vk_present_take(VkFormat format, uint32_t w, uint32_t h, VkImage *image);
void nv2a_vk_present_show(int picture, uint64_t number, uint32_t rows, int has_due, uint32_t due,
                          uint32_t frame);
void nv2a_vk_present_drop(int picture);

/* The scale that fills the title's picture in the window, as it was last shown
 * (1 before that), and for one of this size. */
int nv2a_vk_present_scale(void);
int nv2a_vk_present_fill(uint32_t w, uint32_t h);

/* Until every picture handed over was shown or passed over. */
void nv2a_vk_present_wait(void);

typedef struct { unsigned shown, passed_over, redrawn, chains; } Nv2aVkPresentStats;
void nv2a_vk_present_stats(Nv2aVkPresentStats *out);

/* For tests and dumps: called on the thread with what the window gets,
 * 0xAARRGGBB, every `every` pictures. It waits for the GPU. */
void nv2a_vk_present_watch(void (*fn)(const uint32_t *px, uint32_t w, uint32_t h),
                           unsigned every);

/* RECOMP_PRESENT_VSYNC: 0 mailbox, 1 fifo (the default), 2 adaptive, 3 immediate. And
 * RECOMP_PRESENT_FILTER: 0 linear, 1 point. Any thread, any time. */
void nv2a_vk_present_vsync(int mode);
int  nv2a_vk_present_vsync_is(void);
void nv2a_vk_present_filter(int filter);
int  nv2a_vk_present_filter_is(void);

/* What RECOMP_PRESENT_VSYNC asks for, of a window's modes. */
VkPresentModeKHR nv2a_vk_present_mode(const char *asked, const VkPresentModeKHR *modes,
                                      uint32_t n);

/* 4:3, 16:9 or STRETCH, of a picture now. */
Nv2aVkAspect nv2a_vk_present_aspect(Nv2aVkAspect asked, int wide);

/* Where the picture goes in a window: in that aspect ratio, as large as fits, centred.
 * `rows` as above; an image of fewer ends early, one of more is cut. */
void nv2a_vk_present_fit(uint32_t win_w, uint32_t win_h, Nv2aVkAspect aspect, uint32_t h,
                         uint32_t rows, VkOffset3D to[2], uint32_t *from_h);

#endif
