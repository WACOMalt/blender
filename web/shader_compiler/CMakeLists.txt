# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# GLSL → WGSL compiler for Blender's WebGPU backend: glslang and Tint (from Dawn).
# Built by `web/build_deps.sh` (package `shader_compiler`), which merges all the
# static libraries into `libweb_shader_compiler.a`.

cmake_minimum_required(VERSION 3.20)
project(web_shader_compiler C CXX)

set(DAWN_DIR "" CACHE PATH "Dawn source directory, with third_party dependencies fetched")
if(NOT EXISTS "${DAWN_DIR}/src/tint")
  message(FATAL_ERROR "Set DAWN_DIR to the Dawn source directory")
endif()
set(DAWN_THIRD_PARTY "${DAWN_DIR}/third_party")

set(CMAKE_CXX_STANDARD 20)

# SPIR-V and glslang first: Dawn skips its third party directories for Emscripten.
set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${DAWN_THIRD_PARTY}/spirv-headers/src" spirv-headers EXCLUDE_FROM_ALL)

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${DAWN_THIRD_PARTY}/spirv-headers/src")
add_subdirectory("${DAWN_THIRD_PARTY}/spirv-tools/src" spirv-tools EXCLUDE_FROM_ALL)

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DAWN_THIRD_PARTY}/glslang/src" glslang EXCLUDE_FROM_ALL)

# Tint with only the SPIR-V reader and WGSL writer.
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_USE_X11 OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WAYLAND OFF CACHE BOOL "" FORCE)
set(DAWN_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_WRITER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_VALIDATOR OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_MSL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_NULL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_IR_BINARY OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DAWN_DIR}" dawn EXCLUDE_FROM_ALL)

add_library(web_shader_compiler STATIC web_shader_compiler.cc web_shader_compiler.h)
target_include_directories(web_shader_compiler PRIVATE "${DAWN_DIR}" "${DAWN_THIRD_PARTY}/glslang/src")
target_link_libraries(web_shader_compiler PUBLIC
  tint_api
  tint_lang_spirv_reader
  tint_lang_wgsl_writer
  glslang
  glslang-default-resource-limits
  SPIRV
)

add_executable(wsc_test wsc_test.cc)
target_link_libraries(wsc_test PRIVATE web_shader_compiler)

install(TARGETS web_shader_compiler ARCHIVE DESTINATION lib)
install(FILES web_shader_compiler.h DESTINATION include)
