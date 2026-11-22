cmake_minimum_required(VERSION 3.22.1)
project(oh2_libc_wrapper LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(OH2_LIBC_WRAPPER_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/errno_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/socket_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/stat_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/open_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/pthread_wrapper.c
)

add_library(oh2_libc_wrapper STATIC ${OH2_LIBC_WRAPPER_SOURCES})
target_include_directories(oh2_libc_wrapper PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(oh2_libc_wrapper PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_options(oh2_libc_wrapper PUBLIC "-pthread")
target_link_libraries(oh2_libc_wrapper PUBLIC log)
