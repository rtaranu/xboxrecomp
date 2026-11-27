cmake_minimum_required(VERSION 3.20)
project(nv2a_xf_threads_regression C)
enable_testing()
add_executable(nv2a-xf-threads-test nv2a_xf_threads_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-xf-threads-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-xf-threads-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-xf-threads-test PRIVATE m pthread)
endif()
# One thread writes the pictures, a pool of eight must match them.
set(P ${CMAKE_CURRENT_BINARY_DIR}/nv2a-xf-threads.bin)
add_test(NAME nv2a-xf-one-thread COMMAND nv2a-xf-threads-test write ${P})
set_tests_properties(nv2a-xf-one-thread PROPERTIES
    ENVIRONMENT "RECOMP_RASTER_THREADS=1" FIXTURES_SETUP xf_one)
add_test(NAME nv2a-xf-threads COMMAND nv2a-xf-threads-test check ${P})
set_tests_properties(nv2a-xf-threads PROPERTIES
    ENVIRONMENT "RECOMP_RASTER_THREADS=8" FIXTURES_REQUIRED xf_one)
