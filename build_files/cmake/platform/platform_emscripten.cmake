# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# WebAssembly (Emscripten) settings.
#
# Included after `platform_unix.cmake`, which handles finding libraries
# (Emscripten defines `UNIX`). This file overrides what does not apply to WebAssembly.
# Libraries are expected in `CMAKE_PREFIX_PATH`, built by `web/build_deps.sh`.

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "platform_emscripten.cmake requires the Emscripten toolchain (use emcmake)")
endif()

# Flags that every object file must share, including the dependencies
# (keep in sync with `WEB_FLAGS` in `web/build_deps.sh`):
# - Threads use Web Workers with a shared memory, objects need atomics and bulk memory.
# - C++ exceptions use native WebAssembly exception handling.
# - WebAssembly SIMD (128 bit) is supported by all current browsers.
set(PLATFORM_WEB_FLAGS "-pthread -fwasm-exceptions -msimd128")

# No `-fPIC`: there is no dynamic linking, and position independent code is slower in WebAssembly.
set(PLATFORM_CFLAGS "-funsigned-char -fno-strict-aliasing -ffp-contract=off ${PLATFORM_WEB_FLAGS}")

# Linker flags only valid for ELF linkers.
string(REPLACE " -no-pie" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
unset(PLATFORM_LINKFLAGS_SYMBOL_HIDING)
unset(PLATFORM_SYMBOLS_MAP)

string(APPEND PLATFORM_LINKFLAGS " ${PLATFORM_WEB_FLAGS}")

# WebAssembly 32 bit is limited to 4 GB.
# Blender recurses deeply (e.g. depsgraph, UI layout), the default 64 KB stack is far too small.
set(PLATFORM_WEB_MEMORY_FLAGS
  "-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB"
)

# Programs which run on the build machine to generate code or data
# (e.g. `makesdna`, `makesrna`, `datatoc`). They are compiled to WebAssembly too and
# executed with Node.js. Running them as WebAssembly means `makesdna` computes the
# struct layout of the actual target, without needing a separate native build.
# - NODERAWFS gives direct access to the host file-system.
# - EXECUTABLE adds a `#!` line so they can be run like native programs.
set(PLATFORM_WEB_HOST_TOOLS
  datatoc
  makesdna
  makesrna
  msgfmt
  shader_tool
  zstd_compress
)

function(blender_web_setup_host_tools)
  if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(FATAL_ERROR "Node.js not found, it is needed to run build tools (set NODE_JS)")
  endif()
  # The emulator may be a list (a program and its arguments), only the program is used.
  list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 _node)
  foreach(_tool ${PLATFORM_WEB_HOST_TOOLS})
    if(TARGET ${_tool})
      set_property(
        TARGET ${_tool} APPEND_STRING PROPERTY
        LINK_FLAGS " ${PLATFORM_WEB_MEMORY_FLAGS} -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sEXECUTABLE=${_node}"
      )
    endif()
  endforeach()
endfunction()

# Flags for the `blender` executable itself, see `source/creator/CMakeLists.txt`.
function(blender_web_setup_blender_target target)
  set_property(
    TARGET ${target} APPEND_STRING PROPERTY
    LINK_FLAGS " ${PLATFORM_WEB_MEMORY_FLAGS} -sEXIT_RUNTIME=1 -sFORCE_FILESYSTEM=1"
  )
endfunction()
