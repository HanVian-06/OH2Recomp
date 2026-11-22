cmake_minimum_required(VERSION 3.22.1)
project(oh2_android LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../src/loader        ${CMAKE_CURRENT_BINARY_DIR}/loader_build)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../src/libc_wrapper  ${CMAKE_CURRENT_BINARY_DIR}/libc_wrapper_build)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../src/runtime       ${CMAKE_CURRENT_BINARY_DIR}/runtime_build)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../src/vk_foundation ${CMAKE_CURRENT_BINARY_DIR}/vk_foundation_build)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../src/metal_vk      ${CMAKE_CURRENT_BINARY_DIR}/metal_vk_build)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../src/platform      ${CMAKE_CURRENT_BINARY_DIR}/platform_build)

set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c")
    set(NATIVE_APP_GLUE_DIR "$ENV{ANDROID_NDK_HOME}/sources/android/native_app_glue")
endif()

add_library(oh2_android SHARED
    src/oh2_android_main.cpp
    "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
)

target_include_directories(oh2_android PRIVATE
    "${NATIVE_APP_GLUE_DIR}"
    ${CMAKE_CURRENT_LIST_DIR}/../../src/loader
    ${CMAKE_CURRENT_LIST_DIR}/../../src/libc_wrapper
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime
    ${CMAKE_CURRENT_LIST_DIR}/../../src/vk_foundation/include
    ${CMAKE_CURRENT_LIST_DIR}/../../src/metal_vk
    ${CMAKE_CURRENT_LIST_DIR}/../../src/platform
)

target_compile_options(oh2_android PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -pthread
)

target_link_options(oh2_android PRIVATE
    "-static-libstdc++"
    "-pthread"
    "-Wl,-u,ANativeActivity_onCreate"
    "-Wl,-z,max-page-size=16384"
)

target_link_libraries(oh2_android PRIVATE
    oh2_platform
    oh2_metal_vk
    oh2_runtime_static
    vk_foundation
    aaudio
    vulkan
    android
    log
    dl
    m
    z
)
