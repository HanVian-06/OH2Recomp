cmake_minimum_required(VERSION 3.18)
project(oh2_air2spirv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(oh2_air2spirv STATIC
    bitcode_reader.cpp
    air_to_spirv.cpp
    pak_shader_compiler.cpp
)

target_include_directories(oh2_air2spirv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../metal_vk
)

target_compile_options(oh2_air2spirv PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

if(ANDROID)
    target_link_libraries(oh2_air2spirv PUBLIC z log)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(oh2_air2spirv PUBLIC ZLIB::ZLIB)
endif()
