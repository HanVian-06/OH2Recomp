cmake_minimum_required(VERSION 3.22.1)
project(oh2_metal_vk LANGUAGES CXX)

add_library(oh2_metal_vk STATIC
    spirv_cache.cpp
    mtl_vk_device.cpp
    mtl_vk_resources.cpp
    mtl_vk_pipeline.cpp
    mtl_vk_encoder.cpp
    mtl_vk_selftest.cpp
)

target_compile_features(oh2_metal_vk PUBLIC cxx_std_17)

target_include_directories(oh2_metal_vk
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../loader
        ${CMAKE_CURRENT_LIST_DIR}/../libc_wrapper
        ${CMAKE_CURRENT_LIST_DIR}/../runtime
)

target_compile_options(oh2_metal_vk
    PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-nullability-completeness
        -Wno-missing-field-initializers
)

target_link_libraries(oh2_metal_vk
    PUBLIC
        vk_foundation
        oh2_runtime_static
        vulkan
        android
        log
)
