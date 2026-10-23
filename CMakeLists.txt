cmake_minimum_required(VERSION 3.16)
project(HDRAGC-Next LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT AVISYNTH_INCLUDE_DIR)
    message(FATAL_ERROR "Set -DAVISYNTH_INCLUDE_DIR=<path to AviSynthPlus headers>")
endif()

# M1: shared algorithm core (API-agnostic). Both adapters link this.
add_library(hdragc_core STATIC src/core.cpp)
set_target_properties(hdragc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hdragc_core PUBLIC src)

# AviSynth+ adapter
add_library(hdragc_next SHARED src/avisynth.cpp)
target_include_directories(hdragc_next PRIVATE ${AVISYNTH_INCLUDE_DIR})
target_link_libraries(hdragc_next PRIVATE hdragc_core)
set_target_properties(hdragc_next PROPERTIES PREFIX "")

# Test input plugin (raw YV12 reader)
add_library(rawsrc SHARED src/rawsrc.cpp)
target_include_directories(rawsrc PRIVATE ${AVISYNTH_INCLUDE_DIR})
set_target_properties(rawsrc PROPERTIES PREFIX "")

# Optimization (GCC/Clang only: MSVC punya flag sendiri)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" HAS_AVX2)
    foreach(tgt hdragc_core hdragc_next rawsrc)
        if(HAS_AVX2)
            target_compile_options(${tgt} PRIVATE -O3 -mavx2)
        else()
            target_compile_options(${tgt} PRIVATE -O3)
        endif()
    endforeach()
endif()

# M2 (VapourSynth adapter) will add hdragcnext_vs here, linking hdragc_core.
