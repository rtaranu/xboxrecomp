cmake_minimum_required(VERSION 3.20)
project(nv2a_vk_test C)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(K ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel)
set(V ${CMAKE_CURRENT_SOURCE_DIR}/../../src/nv2a_vk)

# Texture decode: no Vulkan needed.
add_executable(nv2a-vk-tex-test nv2a_vk_tex_test.c ${V}/nv2a_vk_tex.c)
target_include_directories(nv2a-vk-tex-test PRIVATE ${K} ${V})
add_test(NAME nv2a-vk-tex COMMAND nv2a-vk-tex-test)

# Draws through the executor, read back (-DXBOXRECOMP_NV2A_VK=ON): 77, skipped,
# where there is no Vulkan 1.3 device.
include(${V}/nv2a_vk.cmake)
if(XBOXRECOMP_NV2A_VK)
    add_executable(nv2a-vk-draw-test nv2a_vk_draw_test.c ${K}/nv2a_pb_exec.c
        ${K}/nv2a_vsh_interp.c ${K}/nv2a_combiner.c ${K}/nv2a_flip_hold.c ${K}/nv2a_perf.c)
    nv2a_vk_add_to(nv2a-vk-draw-test)
    target_include_directories(nv2a-vk-draw-test PRIVATE ${K} ${K}/..)
    target_compile_definitions(nv2a-vk-draw-test PRIVATE NV2A_PERF_NO_SAMPLER)
    if(NOT WIN32)
        target_link_libraries(nv2a-vk-draw-test PRIVATE m pthread dl)
    endif()
    add_test(NAME nv2a-vk-draw COMMAND nv2a-vk-draw-test)
    set_tests_properties(nv2a-vk-draw PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME nv2a-vk-shader-compile COMMAND nv2a-vk-draw-test)
    set_tests_properties(nv2a-vk-shader-compile PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT RECOMP_NV2A_VK_SHADER_COMPILE=sync)
    add_test(NAME nv2a-vk-window-shows COMMAND nv2a-vk-draw-test shows)
    set_tests_properties(nv2a-vk-window-shows PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "RECOMP_NV2A_VK_PRESENT=0;RECOMP_NV2A_VK_PIPELINE_CACHE=0")
    if(WIN32)                           # opens a window, off the screen
        add_test(NAME nv2a-vk-present COMMAND nv2a-vk-draw-test present)
        set_tests_properties(nv2a-vk-present PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
if(MSVC)
    target_compile_definitions(nv2a-vk-tex-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(TARGET nv2a-vk-draw-test)
        target_compile_definitions(nv2a-vk-draw-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
