# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# WebAssembly (Emscripten) configuration, for running Blender in a web browser.
# Features are OFF unless they have been ported, see `Web_Build_Plan.md`.
#
# Example usage:
#   source web/setup_emsdk.sh && web/build_deps.sh
#   emcmake cmake -C build_files/cmake/config/blender_web.cmake \
#     -DCMAKE_PREFIX_PATH=$HOME/blender-web-deps/install -S . -B ../build_web -G Ninja
#
# Or use `web/build_blender.sh` which does the above.

# Start from the minimal feature set.
include("${CMAKE_CURRENT_LIST_DIR}/blender_lite.cmake")

set(WITH_HEADLESS              ON  CACHE BOOL "" FORCE)
set(WITH_LIBS_PRECOMPILED      OFF CACHE BOOL "" FORCE)
set(WITH_INSTALL_PORTABLE      ON  CACHE BOOL "" FORCE)
set(WITH_STATIC_LIBS           ON  CACHE BOOL "" FORCE)
set(WITH_STRICT_BUILD_OPTIONS  ON  CACHE BOOL "" FORCE)

# No native GPU APIs. A WebGL2/WebGPU backend is a later phase.
set(WITH_OPENGL_BACKEND        OFF CACHE BOOL "" FORCE)
set(WITH_VULKAN_BACKEND        OFF CACHE BOOL "" FORCE)

# x86/ARM specific.
set(WITH_COMPILER_SIMD         OFF CACHE BOOL "" FORCE)
set(WITH_CPU_CHECK             OFF CACHE BOOL "" FORCE)

# Not available or not useful in a browser.
set(WITH_GHOST_X11             OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_WAYLAND         OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_SDL             OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_XDND            OFF CACHE BOOL "" FORCE)
set(WITH_X11_XINPUT            OFF CACHE BOOL "" FORCE)
set(WITH_X11_XFIXES            OFF CACHE BOOL "" FORCE)
set(WITH_INPUT_NDOF            OFF CACHE BOOL "" FORCE)
set(WITH_GTESTS                OFF CACHE BOOL "" FORCE)
set(WITH_GPU_DRAW_TESTS        OFF CACHE BOOL "" FORCE)
set(WITH_RENDERDOC             OFF CACHE BOOL "" FORCE)
set(WITH_COMPILER_ASAN         OFF CACHE BOOL "" FORCE)
set(WITH_LINKER_MOLD           OFF CACHE BOOL "" FORCE)
set(WITH_LINKER_LLD            OFF CACHE BOOL "" FORCE)
set(WITH_BLENDER_THUMBNAILER   OFF CACHE BOOL "" FORCE)
set(WITH_INTERNATIONAL         OFF CACHE BOOL "" FORCE)
set(WITH_BUILDINFO             OFF CACHE BOOL "" FORCE)
set(WITH_GPU_SHADER_CPP_COMPILATION OFF CACHE BOOL "" FORCE)

# Optional libraries built by `web/build_deps.sh`.
set(WITH_TBB                   ON  CACHE BOOL "" FORCE)

# Bring-up stages, enabled as they are ported.
set(WITH_PYTHON                OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES                OFF CACHE BOOL "" FORCE)
