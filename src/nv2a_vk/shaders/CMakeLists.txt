cmake_minimum_required(VERSION 3.20)
project(nv2a_vk_shaders C CXX)

# Regenerates ../nv2a_vk_spirv.h from the GLSL here. Only for whoever edits the
# shaders: the header is committed, so a toolkit build needs no compiler.
#   cmake -S . -B <build> -DCMAKE_PREFIX_PATH=<glslang install>
#   cmake --build <build> --config Release --target nv2a-vk-spirv
set(CMAKE_CXX_STANDARD 17)
find_package(glslang CONFIG REQUIRED)
add_executable(nv2a-vk-compile compile.c link.cpp)
target_link_libraries(nv2a-vk-compile PRIVATE
    glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
add_custom_target(nv2a-vk-spirv
    COMMAND nv2a-vk-compile ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a_vk_spirv.h
    DEPENDS nv2a-vk-compile)
