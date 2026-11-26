/*
 * The driver's compiled pipelines in a file, so that the next start need not
 * compile them: nv2a.cache beside the executable (RECOMP_NV2A_VK_PIPELINE_CACHE=<path>, or 0
 * for none).
 */
#ifndef NV2A_VK_PIPELINE_CACHE_H
#define NV2A_VK_PIPELINE_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where it is; "" for none. */
const char *nv2a_vk_pipeline_cache_path(void);

/* What was written there, for free(); NULL if there is nothing, it is damaged or
 * another card's or driver's. */
typedef struct { uint32_t vendor, device; uint8_t uuid[16]; } Nv2aVkPipelineCacheOf;
void *nv2a_vk_pipeline_cache_read(const char *path, const Nv2aVkPipelineCacheOf *of, size_t *bytes);

/* vkGetPipelineCacheData's. The old file stays until the new one is complete. */
int nv2a_vk_pipeline_cache_write(const char *path, const void *data, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
