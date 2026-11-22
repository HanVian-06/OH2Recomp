cmake_minimum_required(VERSION 3.22.1)
project(vk_foundation LANGUAGES CXX)

add_library(vk_foundation STATIC
    src/vma_impl.cpp
    src/vk_context.cpp
    src/staging_ring.cpp
    src/astc_texture.cpp
    src/m3_renderer.cpp
)

target_compile_features(vk_foundation PUBLIC cxx_std_17)

target_include_directories(vk_foundation
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_include_directories(vk_foundation
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/../../third_party/vma
)

target_compile_options(vk_foundation
    PRIVATE
        -Wno-nullability-completeness
)

target_compile_definitions(vk_foundation
    PUBLIC
        VMA_VULKAN_VERSION=1001000
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        VK_USE_PLATFORM_ANDROID_KHR
)

target_link_libraries(vk_foundation
    PUBLIC
        vulkan
        android
        log
)
