/* The Vulkan loader, opened at run time (nv2a_vk_api.h). */
#include <stdio.h>
#include "nv2a_vk_api.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#define DEFINE(name) PFN_##name name;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
NV2A_VK_GLOBAL(DEFINE)
NV2A_VK_INSTANCE(DEFINE)
NV2A_VK_DEVICE(DEFINE)

static int missing(const char *name)
{
    fprintf(stderr, "[NV2A-VK] the Vulkan driver has no %s\n", name);
    return 0;
}

int nv2a_vk_api_load(void)
{
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");

    if (!lib)
        return 0;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)
        GetProcAddress(lib, "vkGetInstanceProcAddr");
#endif                                  /* other hosts: not done yet */
    if (!vkGetInstanceProcAddr)
        return 0;
#define GLOBAL(name) \
    if (!(name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        return missing(#name);
    NV2A_VK_GLOBAL(GLOBAL)
    return 1;
}

int nv2a_vk_api_load_instance(VkInstance instance)
{
#define INSTANCE(name) \
    if (!(name = (PFN_##name)vkGetInstanceProcAddr(instance, #name))) \
        return missing(#name);
    NV2A_VK_INSTANCE(INSTANCE)
    return 1;
}

int nv2a_vk_api_load_device(VkDevice device)
{
#define DEVICE(name) \
    if (!(name = (PFN_##name)vkGetDeviceProcAddr(device, #name))) \
        return missing(#name);
    NV2A_VK_DEVICE(DEVICE)
    return 1;
}
