cmake_minimum_required(VERSION 3.20)

project(SWE1Racer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# =============================================================================
# Options
# =============================================================================
set(N64_MODERN_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/../N64ModernRuntime" CACHE PATH
    "Path to N64ModernRuntime")
set(RT64_DIR "${CMAKE_SOURCE_DIR}/../rt64" CACHE PATH
    "Path to RT64 renderer")

if(WIN32)
    # Windows: dependencies come from vcpkg.
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE PATH
        "vcpkg toolchain file")
elseif(APPLE)
    # macOS: dependencies come from Homebrew (brew install sdl2 cmake ninja).
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HOMEBREW_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(HOMEBREW_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
    endif()
    # RT64 builds its Metal shaders with xcrun and links its own .mm sources.
    enable_language(OBJC OBJCXX)
endif()

# =============================================================================
# SDL2
# =============================================================================
find_package(SDL2 CONFIG REQUIRED)

# =============================================================================
# RT64 Renderer
# =============================================================================
if(WIN32)
    # RT64 links SDL2 by bare name; ensure vcpkg's lib dir is on the search path
    link_directories("C:/vcpkg/installed/x64-windows/lib")
    link_directories("C:/vcpkg/installed/x64-windows/lib/manual-link")
endif()
set(RT64_STATIC ON CACHE BOOL "Build RT64 as static library" FORCE)
# Note: RT64 builds with Retina support off (it sizes its output in window
# points). SDL's Metal view creates a backing-scale (2x) CAMetalLayer, so
# src/support_apple.mm pins the layer to contentsScale 1 to match. Enabling
# PLUME_APPLE_RETINA_ENABLED instead made RT64 collapse to ~1 fps in the game's
# 640x480 interlaced mode.
# Local fixes to upstream RT64, kept in patches/ so they survive re-clones.
# Each patch is applied once; already-applied patches are skipped.
file(GLOB RT64_PATCHES "${CMAKE_SOURCE_DIR}/patches/rt64-*.patch")
foreach(patch ${RT64_PATCHES})
    execute_process(COMMAND git apply --reverse --check ${patch}
        WORKING_DIRECTORY ${RT64_DIR} RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
        execute_process(COMMAND git apply ${patch}
            WORKING_DIRECTORY ${RT64_DIR} RESULT_VARIABLE failed)
        if(failed)
            message(FATAL_ERROR "Could not apply ${patch} to ${RT64_DIR}")
        endif()
        message(STATUS "Applied RT64 patch ${patch}")
    endif()
endforeach()
add_subdirectory(${RT64_DIR} ${CMAKE_BINARY_DIR}/rt64)

# =============================================================================
# N64ModernRuntime (librecomp + ultramodern)
# =============================================================================
add_subdirectory(${N64_MODERN_RUNTIME_DIR} ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# =============================================================================
# Recompiled Functions Library
# =============================================================================
file(GLOB RECOMP_SOURCES "RecompiledFuncs/*.c")

add_library(RecompiledFuncs STATIC ${RECOMP_SOURCES})

target_include_directories(RecompiledFuncs PRIVATE
    ${N64_MODERN_RUNTIME_DIR}/N64Recomp/include  # recomp.h
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs           # funcs.h
)

# Disable strict aliasing for recompiled code (MIPS memory access patterns)
if(MSVC)
    # MSVC doesn't have strict aliasing by default
else()
    target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

# =============================================================================
# Main Executable
# =============================================================================
set(SWE1R_SOURCES
    src/main.cpp
    src/section_table.cpp
    src/rt64_render_context.cpp
    src/audio.cpp
)
if(APPLE)
    list(APPEND SWE1R_SOURCES src/support_apple.mm)
endif()

# Recompiled RSP audio microcode (generated, not versioned, like RecompiledFuncs):
#   ../N64Recomp/build/RSPRecomp rsp/aspMain.toml
set(SWE1R_RSP_SOURCES ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp)
foreach(rsp_src ${SWE1R_RSP_SOURCES})
    if(NOT EXISTS ${rsp_src})
        message(FATAL_ERROR "${rsp_src} is missing. Generate it with: ../N64Recomp/build/RSPRecomp rsp/aspMain.toml")
    endif()
endforeach()
list(APPEND SWE1R_SOURCES ${SWE1R_RSP_SOURCES})
# sse2neon backs the RSP vector unit emulation on ARM (rsp_vu.hpp).
set_source_files_properties(${SWE1R_RSP_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${N64_MODERN_RUNTIME_DIR}/thirdparty/sse2neon")
if(NOT MSVC)
    set_source_files_properties(${SWE1R_RSP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-label;-Wno-unused-but-set-variable")
endif()

add_executable(SWE1Racer ${SWE1R_SOURCES})

target_include_directories(SWE1Racer PRIVATE
    ${N64_MODERN_RUNTIME_DIR}/N64Recomp/include     # recomp.h
    ${N64_MODERN_RUNTIME_DIR}/librecomp/include      # librecomp headers
    ${N64_MODERN_RUNTIME_DIR}/ultramodern/include    # ultramodern headers
    ${RT64_DIR}/src                                   # RT64 headers
    ${RT64_DIR}/src/contrib                           # RT64 contrib (json, im3d, etc.)
    ${RT64_DIR}/src/contrib/hlslpp/include            # hlsl++
    ${RT64_DIR}/src/contrib/plume                     # plume render interface
    ${RT64_DIR}/src/contrib/imgui                     # imgui
    ${RT64_DIR}/src/contrib/mupen64plus-core/src/api  # mupen64plus API types
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs              # funcs.h
    ${CMAKE_SOURCE_DIR}/include
)

# RT64 requires these defines for its headers (normally set at directory scope)
target_compile_definitions(SWE1Racer PRIVATE NOMINMAX HLSL_CPU FFX_GCC)

target_link_libraries(SWE1Racer PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    SDL2::SDL2
)

if(WIN32)
    target_link_libraries(SWE1Racer PRIVATE SDL2::SDL2main dxgi d3d12 d3dcompiler dbghelp)
    target_link_options(SWE1Racer PRIVATE /MAP)
elseif(APPLE)
    # Export symbols so backtrace()/dladdr can name recompiled func_XXXXXXXX frames in crash traces.
    target_link_options(SWE1Racer PRIVATE "-Wl,-export_dynamic")
    target_link_libraries(SWE1Racer PRIVATE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Cocoa"
        "-framework Foundation"
    )
endif()
