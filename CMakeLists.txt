cmake_minimum_required(VERSION 3.16)
project(HDRAGC-Next LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT AVISYNTH_INCLUDE_DIR)
    message(FATAL_ERROR "Set -DAVISYNTH_INCLUDE_DIR=<path to AviSynthPlus headers>")
endif()

# ---------------------------------------------------------------------------
# Shared algorithm core (API-agnostic). Both interfaces live in ONE binary:
# the file exports AvisynthPluginInit2/3 AND VapourSynthPluginInit2, and each
# host calls its own entry point. No link-time dependency on either host.
# ---------------------------------------------------------------------------
add_library(hdragc_core STATIC src/core.cpp)
target_include_directories(hdragc_core PUBLIC src)
set_target_properties(hdragc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ONE dual-interface plugin: hdragc_next.dll (Windows) / hdragc_next.so (Linux)
add_library(hdragc_dual SHARED
    src/avisynth.cpp
    src/vs.cpp)
target_include_directories(hdragc_dual PRIVATE ${AVISYNTH_INCLUDE_DIR} include/vs)
target_link_libraries(hdragc_dual PRIVATE hdragc_core)
set_target_properties(hdragc_dual PROPERTIES PREFIX "" OUTPUT_NAME "hdragc_next")

# Static C/C++ runtime on MSVC: the DLL must not depend on
# vcruntime140.dll / msvcp140.dll (user copies one file, installs nothing).
if(MSVC)
    set_target_properties(hdragc_dual rawsrc PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Test input plugin (raw YV12 reader; test-only, not distributed)
add_library(rawsrc SHARED src/rawsrc.cpp)
target_include_directories(rawsrc PRIVATE ${AVISYNTH_INCLUDE_DIR})
set_target_properties(rawsrc PROPERTIES PREFIX "")

# Optimization (GCC/Clang only; MSVC uses its own Release flags)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" HAS_AVX2)
    foreach(tgt hdragc_core hdragc_dual rawsrc)
        if(HAS_AVX2)
            target_compile_options(${tgt} PRIVATE -O3 -mavx2)
        else()
            target_compile_options(${tgt} PRIVATE -O3)
        endif()
    endforeach()
endif()