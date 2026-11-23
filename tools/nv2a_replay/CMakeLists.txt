cmake_minimum_required(VERSION 3.20)
project(nv2a_replay C)

# Draws a captured frame again offline (nv2a_replay.c). Its own project: a
# title's build does not need it.
set(CMAKE_C_STANDARD 11)
set(NV2A_REPLAY_KERNEL ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel)
add_executable(nv2a-replay nv2a_replay.c
    ${NV2A_REPLAY_KERNEL}/nv2a_pb_exec.c
    ${NV2A_REPLAY_KERNEL}/nv2a_vsh_interp.c
    ${NV2A_REPLAY_KERNEL}/nv2a_combiner.c
    ${NV2A_REPLAY_KERNEL}/nv2a_flip_hold.c)
target_include_directories(nv2a-replay PRIVATE
    ${NV2A_REPLAY_KERNEL} ${NV2A_REPLAY_KERNEL}/..)
if(MSVC)
    target_compile_options(nv2a-replay PRIVATE /O2 /GS-)
    target_compile_definitions(nv2a-replay PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-replay PRIVATE m pthread)
endif()
