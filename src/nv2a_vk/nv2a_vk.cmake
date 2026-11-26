# The Vulkan rasteriser (nv2a_vk.h) as part of another target: it calls the
# executor and the executor calls it, so it is not a library of its own.
#   XBOXRECOMP_NV2A_VK          build it (then RECOMP_NV2A_VK=1 turns it on)
#   XBOXRECOMP_VULKAN_INCLUDE   folder with vulkan/vulkan_core.h
option(XBOXRECOMP_NV2A_VK "Build the Vulkan rasteriser for the pushbuffer executor" OFF)
set(NV2A_VK_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(XBOXRECOMP_NV2A_VK)
    find_path(XBOXRECOMP_VULKAN_INCLUDE vulkan/vulkan_core.h
        PATHS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
    if(NOT XBOXRECOMP_VULKAN_INCLUDE)
        message(FATAL_ERROR "XBOXRECOMP_NV2A_VK: set XBOXRECOMP_VULKAN_INCLUDE to the "
                            "folder that holds vulkan/vulkan_core.h (Vulkan-Headers)")
    endif()
endif()

function(nv2a_vk_add_to target)
    target_sources(${target} PRIVATE
        ${NV2A_VK_DIR}/nv2a_vk.c ${NV2A_VK_DIR}/nv2a_vk_api.c ${NV2A_VK_DIR}/nv2a_vk_tex.c
        ${NV2A_VK_DIR}/nv2a_vk_vtx.c ${NV2A_VK_DIR}/nv2a_vk_present.c
        ${NV2A_VK_DIR}/nv2a_vk_pipeline_cache.c)
    target_include_directories(${target} PRIVATE
        ${NV2A_VK_DIR} ${XBOXRECOMP_VULKAN_INCLUDE})
    target_compile_definitions(${target} PRIVATE XBOXRECOMP_NV2A_VK)
endfunction()
