/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Compile Blender's Vulkan style GLSL to WGSL for the WebGPU backend:
 * GLSL → SPIR-V with glslang, SPIR-V → WGSL with Tint.
 *
 * A C API so Blender doesn't depend on glslang's or Tint's headers.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum WSC_Stage {
  WSC_STAGE_VERTEX = 0,
  WSC_STAGE_FRAGMENT = 1,
  WSC_STAGE_COMPUTE = 2,
} WSC_Stage;

/**
 * WebGPU has no combined texture/sampler bindings: a GLSL `sampler2D` at (group, binding)
 * becomes a texture at the same binding point and a sampler at the binding point given here.
 */
typedef struct WSC_SamplerMapping {
  uint32_t group;
  uint32_t binding;
  uint32_t sampler_group;
  uint32_t sampler_binding;
} WSC_SamplerMapping;

typedef struct WSC_Result {
  /** WGSL source, or null on failure. */
  char *wgsl;
  /** Errors and warnings, or null. */
  char *log;
} WSC_Result;

/** Must be called once before compiling. */
void WSC_init(void);
void WSC_exit(void);

/**
 * Compile GLSL (version 450, Vulkan semantics) to WGSL.
 * Free the result with #WSC_result_free.
 */
WSC_Result WSC_glsl_to_wgsl(const char *glsl,
                            WSC_Stage stage,
                            const WSC_SamplerMapping *sampler_mappings,
                            int sampler_mappings_num);

void WSC_result_free(WSC_Result *result);

#ifdef __cplusplus
}
#endif
