cmake_minimum_required(VERSION 3.20)
project(blender_wasm_smoke C)
add_executable(smoke main.c)
set_target_properties(smoke PROPERTIES SUFFIX ".js")
