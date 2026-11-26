/*
 * The window's swap chain (nv2a_vk_present.h): not here yet. The rasteriser
 * has no window of its own; what it draws goes to guest memory at each flip,
 * and the toolkit's window shows it from there.
 */
#include <stdint.h>
#include <string.h>

#include "nv2a_vk_present.h"

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
