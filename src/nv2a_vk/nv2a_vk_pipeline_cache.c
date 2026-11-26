/* The pipeline cache's file (nv2a_vk_pipeline_cache.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "nv2a_vk_pipeline_cache.h"

#define MAGIC 0x4332564Eu               /* NV2C */
#define MOST  (256u << 20)

/* Before the data: a driver checks whose it is, not whether it is whole. */
typedef struct { uint32_t magic, bytes; uint64_t sum; } Head;

static uint64_t sum_of(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;

    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

const char *nv2a_vk_pipeline_cache_path(void)
{
    static char path[1024];
    static int known;
    const char *e = getenv("RECOMP_NV2A_VK_PIPELINE_CACHE"), *s = getenv("RECOMP_NV2A_VK_SETTINGS");

    if (known)
        return path;
    known = 1;
    if (e && *e)
        snprintf(path, sizeof path, "%s", strcmp(e, "0") ? e : "");
    else if (!s || strcmp(s, "0")) {    /* a run that keeps nothing */
#if defined(_WIN32)
        DWORD n = GetModuleFileNameA(NULL, path, sizeof path - sizeof "nv2a.cache");
        char *end;

        path[n < sizeof path ? n : 0] = 0;
        end = strrchr(path, '\\');
        strcpy(end ? end + 1 : path, "nv2a.cache");
#else
        strcpy(path, "nv2a.cache");
#endif
    }
    return path;
}

void *nv2a_vk_pipeline_cache_read(const char *path, const Nv2aVkPipelineCacheOf *of, size_t *bytes)
{
    FILE *f = *path ? fopen(path, "rb") : NULL;
    uint8_t *data = NULL;
    uint32_t first[4];                  /* VkPipelineCacheHeaderVersionOne */
    Head h;

    *bytes = 0;
    if (!f)
        return NULL;
    if (fread(&h, sizeof h, 1, f) == 1 && h.magic == MAGIC && h.bytes >= 32 && h.bytes <= MOST
        && (data = (uint8_t *)malloc(h.bytes)) != NULL) {
        if (fread(data, 1, h.bytes, f) == h.bytes && sum_of(data, h.bytes) == h.sum) {
            memcpy(first, data, sizeof first);
            if (first[0] == 32 && first[1] == 1 && first[2] == of->vendor
                && first[3] == of->device && memcmp(data + 16, of->uuid, 16) == 0)
                *bytes = h.bytes;
        }
    }
    fclose(f);
    if (!*bytes) {
        free(data);
        data = NULL;
    }
    return data;
}

int nv2a_vk_pipeline_cache_write(const char *path, const void *data, size_t bytes)
{
    char tmp[1032];
    Head h = { MAGIC, (uint32_t)bytes, 0 };
    FILE *f;
    int ok;

    if (!*path || bytes < 32 || bytes > MOST)
        return 0;
    h.sum = sum_of((const uint8_t *)data, bytes);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f)
        return 0;
    ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, 1, bytes, f) == bytes;
    ok = fclose(f) == 0 && ok;
#if defined(_WIN32)
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok)
        remove(tmp);
    return ok;
}
