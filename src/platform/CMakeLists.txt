cmake_minimum_required(VERSION 3.22.1)
project(oh2_platform LANGUAGES CXX)

add_library(oh2_platform STATIC
    touch_bridge.cpp
    choreographer_bridge.cpp
    audio_bridge.cpp
)

target_compile_features(oh2_platform PUBLIC cxx_std_17)

target_include_directories(oh2_platform
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../loader
        ${CMAKE_CURRENT_LIST_DIR}/../libc_wrapper
        ${CMAKE_CURRENT_LIST_DIR}/../runtime
        ${CMAKE_CURRENT_LIST_DIR}/../metal_vk
)

target_compile_options(oh2_platform
    PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
)

target_link_libraries(oh2_platform
    PUBLIC
        oh2_runtime_static
        oh2_metal_vk
        aaudio
        android
        log
)
