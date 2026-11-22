cmake_minimum_required(VERSION 3.22.1)
project(oh2_m1_headless LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../src/loader
    ${CMAKE_CURRENT_BINARY_DIR}/loader_build
)

add_executable(oh2_m1_loader
    ${CMAKE_CURRENT_LIST_DIR}/main_headless.c
)

target_compile_options(oh2_m1_loader PRIVATE -Wall -Wextra -pthread)
target_link_options(oh2_m1_loader PRIVATE "-static-libstdc++" "-pthread" "-Wl,-z,max-page-size=16384")
target_link_libraries(oh2_m1_loader PRIVATE
    oh2_loader_static
    log
    dl
    m
    z
)
