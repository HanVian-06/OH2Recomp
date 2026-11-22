cmake_minimum_required(VERSION 3.22.1)
project(oh2_runtime LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OH2_RUNTIME_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/objc_msgSend_arm64.S
    ${CMAKE_CURRENT_LIST_DIR}/objc_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ui_bridges.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ns_bridges.cpp
    ${CMAKE_CURRENT_LIST_DIR}/framework_stubs.cpp
    ${CMAKE_CURRENT_LIST_DIR}/m2_selftest.cpp
)

add_library(oh2_runtime_static STATIC ${OH2_RUNTIME_SOURCES})
target_include_directories(oh2_runtime_static PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../loader
    ${CMAKE_CURRENT_LIST_DIR}/../libc_wrapper
)
target_compile_options(oh2_runtime_static PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_libraries(oh2_runtime_static PUBLIC
    oh2_libc_wrapper
    oh2_loader_static
    log
    dl
    m
    z
)

add_library(oh2_runtime SHARED ${OH2_RUNTIME_SOURCES})
target_include_directories(oh2_runtime PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../loader
    ${CMAKE_CURRENT_LIST_DIR}/../libc_wrapper
)
target_compile_options(oh2_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_options(oh2_runtime PRIVATE "-static-libstdc++" "-pthread" "-Wl,-z,max-page-size=16384")
target_link_libraries(oh2_runtime PUBLIC
    oh2_libc_wrapper
    oh2_loader_static
    log
    dl
    m
    z
)
