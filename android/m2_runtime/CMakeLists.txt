cmake_minimum_required(VERSION 3.22.1)
project(oh2_m2_runtime LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../src/loader
    ${CMAKE_CURRENT_BINARY_DIR}/loader_build
)
add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../src/libc_wrapper
    ${CMAKE_CURRENT_BINARY_DIR}/libc_wrapper_build
)
add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime
    ${CMAKE_CURRENT_BINARY_DIR}/runtime_build
)

# Native executable runner for ADB / gate_m2.ps1 execution
add_executable(oh2_m2_runner
    ${CMAKE_CURRENT_LIST_DIR}/main_m2_runner.c
)
target_include_directories(oh2_m2_runner PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/loader
    ${CMAKE_CURRENT_LIST_DIR}/../../src/libc_wrapper
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime
)
target_compile_options(oh2_m2_runner PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_options(oh2_m2_runner PRIVATE "-static-libstdc++" "-pthread" "-Wl,-z,max-page-size=16384")
target_link_libraries(oh2_m2_runner PRIVATE
    oh2_runtime_static
    log
    dl
    m
    z
)

# Android NativeActivity shared library packaged into build/m2/m2_runtime-debug.apk
set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c")
    set(NATIVE_APP_GLUE_DIR "$ENV{ANDROID_NDK_HOME}/sources/android/native_app_glue")
endif()

add_library(m2_runtime_app SHARED
    ${CMAKE_CURRENT_LIST_DIR}/m2_android_main.cpp
    "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
)
target_include_directories(m2_runtime_app PRIVATE
    "${NATIVE_APP_GLUE_DIR}"
    ${CMAKE_CURRENT_LIST_DIR}/../../src/loader
    ${CMAKE_CURRENT_LIST_DIR}/../../src/libc_wrapper
    ${CMAKE_CURRENT_LIST_DIR}/../../src/runtime
)
target_compile_options(m2_runtime_app PRIVATE -Wall -Wextra -Wno-unused-parameter -pthread)
target_link_options(m2_runtime_app PRIVATE "-static-libstdc++" "-pthread" "-Wl,-u,ANativeActivity_onCreate" "-Wl,-z,max-page-size=16384")
target_link_libraries(m2_runtime_app PRIVATE
    oh2_runtime_static
    android
    log
    dl
    m
    z
)
