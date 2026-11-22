cmake_minimum_required(VERSION 3.22.1)
project(oh2_loader LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OH2_LOADER_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/macho_loader.c
    ${CMAKE_CURRENT_LIST_DIR}/dyld_fixups.c
    ${CMAKE_CURRENT_LIST_DIR}/variadic_trampolines.S
    ${CMAKE_CURRENT_LIST_DIR}/variadic_wrappers.c
    ${CMAKE_CURRENT_LIST_DIR}/unwind_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/symbol_resolver.cpp
)

add_library(oh2_loader_static STATIC ${OH2_LOADER_SOURCES})
target_include_directories(oh2_loader_static PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(oh2_loader_static PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_options(oh2_loader_static PUBLIC "-pthread")
target_link_libraries(oh2_loader_static PUBLIC log dl m z)

add_library(oh2_loader SHARED ${OH2_LOADER_SOURCES})
target_include_directories(oh2_loader PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(oh2_loader PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_options(oh2_loader PRIVATE "-static-libstdc++" "-pthread" "-Wl,-z,max-page-size=16384")
target_link_libraries(oh2_loader PUBLIC log dl m z)
