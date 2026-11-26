/*
 * The Vulkan entry points the rasteriser uses, fetched from the loader at run
 * time (nv2a_vk_api_*), so nothing links against Vulkan and a host without it
 * only lacks the rasteriser.
 */
#ifndef NV2A_VK_API_H
#define NV2A_VK_API_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define NV2A_VK_GLOBAL(F) \
    F(vkCreateInstance) F(vkEnumerateInstanceLayerProperties)

#define NV2A_VK_INSTANCE(F) \
    F(vkDestroyInstance) F(vkEnumeratePhysicalDevices) \
    F(vkGetPhysicalDeviceProperties) F(vkGetPhysicalDeviceFeatures2) \
    F(vkGetPhysicalDeviceQueueFamilyProperties) \
    F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceFormatProperties) \
    F(vkEnumerateDeviceExtensionProperties) \
    F(vkCreateDevice) F(vkGetDeviceProcAddr)

#define NV2A_VK_DEVICE(F) \
    F(vkGetDeviceQueue) F(vkDestroyDevice) F(vkDeviceWaitIdle) \
    F(vkAllocateMemory) F(vkFreeMemory) F(vkMapMemory) \
    F(vkInvalidateMappedMemoryRanges) \
    F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) \
    F(vkBindBufferMemory) \
    F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) \
    F(vkBindImageMemory) F(vkCreateImageView) F(vkDestroyImageView) \
    F(vkCreateSampler) \
    F(vkCreateShaderModule) F(vkDestroyShaderModule) \
    F(vkCreateDescriptorSetLayout) F(vkCreateDescriptorPool) \
    F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) \
    F(vkCreatePipelineLayout) F(vkCreateGraphicsPipelines) \
    F(vkCreatePipelineCache) F(vkGetPipelineCacheData) \
    F(vkCreateCommandPool) F(vkResetCommandPool) F(vkAllocateCommandBuffers) \
    F(vkBeginCommandBuffer) F(vkEndCommandBuffer) \
    F(vkCreateQueryPool) F(vkGetQueryPoolResults) F(vkCmdResetQueryPool) \
    F(vkCmdWriteTimestamp2) F(vkCmdBeginQuery) F(vkCmdEndQuery) \
    F(vkCreateSemaphore) F(vkWaitSemaphores) \
    F(vkCreateFence) F(vkWaitForFences) F(vkResetFences) F(vkQueueSubmit) \
    F(vkCmdPipelineBarrier2) F(vkCmdCopyBufferToImage) F(vkCmdCopyImageToBuffer) \
    F(vkCmdCopyImage) F(vkCmdBlitImage) F(vkCmdResolveImage) F(vkCmdClearDepthStencilImage) \
    F(vkCmdBeginRendering) F(vkCmdEndRendering) F(vkCmdClearAttachments) \
    F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) \
    F(vkCmdBindVertexBuffers) F(vkCmdBindIndexBuffer) F(vkCmdDrawIndexed) F(vkCmdDraw) \
    F(vkCmdSetViewport) F(vkCmdSetScissor) F(vkCmdSetBlendConstants) F(vkCmdSetLineWidth) \
    F(vkCmdSetCullMode) F(vkCmdSetFrontFace) F(vkCmdSetDepthTestEnable) \
    F(vkCmdSetDepthWriteEnable) F(vkCmdSetDepthCompareOp) \
    F(vkCmdSetDepthBiasEnable) F(vkCmdSetDepthBias) \
    F(vkCmdSetStencilTestEnable) F(vkCmdSetStencilOp) \
    F(vkCmdSetStencilCompareMask) F(vkCmdSetStencilWriteMask) \
    F(vkCmdSetStencilReference)

#define NV2A_VK_DECLARE(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
NV2A_VK_GLOBAL(NV2A_VK_DECLARE)
NV2A_VK_INSTANCE(NV2A_VK_DECLARE)
NV2A_VK_DEVICE(NV2A_VK_DECLARE)

/* 1 if every entry point was found. */
int nv2a_vk_api_load(void);
int nv2a_vk_api_load_instance(VkInstance instance);
int nv2a_vk_api_load_device(VkDevice device);

#endif
