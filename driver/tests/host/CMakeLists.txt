# Host test harness: runs the driver test suite on Corgi3DS's PICA200 emulation.
#
#   cmake -S driver/tests/host -B build && cmake --build build && build/corgi_pica_tests
cmake_minimum_required(VERSION 3.10)
project(corgi_pica_tests C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
get_filename_component(CORGI_SRC ${DRIVER_DIR}/../src ABSOLUTE)

file(GLOB DRIVER_SOURCES ${DRIVER_DIR}/src/*.c)

add_executable(corgi_pica_tests
    corgi_harness.cpp
    ${DRIVER_DIR}/tests/suite.c
    ${DRIVER_SOURCES}
    ${CORGI_SRC}/core/arm11/gpu.cpp
    ${CORGI_SRC}/core/common/exceptions.cpp
    ${CORGI_SRC}/core/common/bswp.cpp
)
target_include_directories(corgi_pica_tests PRIVATE
    ${DRIVER_DIR}/include ${DRIVER_DIR}/tests ${CORGI_SRC})
target_link_libraries(corgi_pica_tests m)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${DRIVER_SOURCES} ${DRIVER_DIR}/tests/suite.c
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
endif()

enable_testing()
add_test(NAME pica_suite COMMAND corgi_pica_tests)
