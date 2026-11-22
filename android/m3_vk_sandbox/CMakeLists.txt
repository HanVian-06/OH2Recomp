cmake_minimum_required(VERSION 3.22.1)
project(m3_vk_sandbox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../src/vk_foundation
    ${CMAKE_CURRENT_BINARY_DIR}/vk_foundation_build
)

set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c")
    set(NATIVE_APP_GLUE_DIR "$ENV{ANDROID_NDK_HOME}/sources/android/native_app_glue")
endif()

add_library(m3_vk_sandbox SHARED
    src/android_main.cpp
    "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
)

target_include_directories(m3_vk_sandbox PRIVATE
    "${NATIVE_APP_GLUE_DIR}"
)

target_link_options(m3_vk_sandbox PRIVATE
    "-Wl,-u,ANativeActivity_onCreate"
    "-Wl,-z,max-page-size=16384"
)

target_link_libraries(m3_vk_sandbox PRIVATE
    vk_foundation
    android
    log
    vulkan
)
