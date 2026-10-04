cmake_minimum_required(VERSION 3.16)
project(HDRAGCNext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT AVISYNTH_INCLUDE_DIR)
    message(FATAL_ERROR "Set -DAVISYNTH_INCLUDE_DIR=<path to AviSynthPlus headers>")
endif()

add_library(hdragc_next SHARED src/hdragc_next.cpp)
target_include_directories(hdragc_next PRIVATE ${AVISYNTH_INCLUDE_DIR})
set_target_properties(hdragc_next PROPERTIES PREFIX "")

# M7: optimization (GCC/Clang only: MSVC punya flag sendiri)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" HAS_AVX2)
    if(HAS_AVX2)
        target_compile_options(hdragc_next PRIVATE -O3 -mavx2)
    else()
        target_compile_options(hdragc_next PRIVATE -O3)
    endif()
endif()

add_library(rawsrc SHARED src/rawsrc.cpp)
target_include_directories(rawsrc PRIVATE ${AVISYNTH_INCLUDE_DIR})
set_target_properties(rawsrc PROPERTIES PREFIX "")
