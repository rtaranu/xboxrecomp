cmake_minimum_required(VERSION 3.20)
project(nv2a_capture_test C)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(K ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel)
add_executable(nv2a-capture-test nv2a_capture_test.c ${K}/nv2a_capture.c
    ${K}/nv2a_pb_exec.c ${K}/nv2a_vsh_interp.c ${K}/nv2a_combiner.c
    ${K}/nv2a_flip_hold.c
    ${K}/nv2a_perf.c)
target_include_directories(nv2a-capture-test PRIVATE ${K} ${K}/..)
if(MSVC)
    target_compile_definitions(nv2a-capture-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-capture-test PRIVATE m pthread)
endif()
add_subdirectory(../../tools/nv2a_replay nv2a_replay)

set(P ${CMAKE_CURRENT_BINARY_DIR}/frame)
add_test(NAME nv2a-capture COMMAND nv2a-capture-test ${P})
set_tests_properties(nv2a-capture PROPERTIES FIXTURES_SETUP captured)
# Drawn again from the file alone, every pixel the same; and again in the same
# process, where the first pass's leftovers must not show.
add_test(NAME nv2a-replay COMMAND nv2a-replay ${P}-0.nvcap --expect ${P}.raw)
add_test(NAME nv2a-replay-twice COMMAND nv2a-replay ${P}-0.nvcap --repeat 2 --expect ${P}.raw)
add_test(NAME nv2a-replay-walk COMMAND nv2a-replay ${P}-0.nvcap --mode walk)
add_test(NAME nv2a-replay-transform COMMAND nv2a-replay ${P}-0.nvcap --mode transform)
set_tests_properties(nv2a-replay nv2a-replay-twice nv2a-replay-walk nv2a-replay-transform
    PROPERTIES FIXTURES_REQUIRED captured)
