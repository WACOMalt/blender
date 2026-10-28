/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Shaders are generated as Vulkan GLSL (like the Vulkan back-end) and translated to WGSL.
 * Differences with the Vulkan back-end:
 * - Push constants are stored in a uniform buffer.
 * - Samplers are separate from textures in WGSL, they are in bind group #WG_SAMPLER_GROUP
 *   using the binding of their texture.
 * - There are no geometry shaders, layered rendering or barycentric coordinates.
 * - WebGPU's framebuffer Y axis points down, the vertex shader flips Y so the memory layout of
 *   textures matches the other back-ends. Front faces are clockwise as a result.
 */

#pragma once

#include <mutex>

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "gpu_shader_private.hh"

#include "wg_commands.hh"
#include "wg_shader_interface.hh"

namespace blender::gpu {

/** State of a render pipeline, besides the shader. */
struct WGRenderPipelineKey {
  uint64_t state = 0;
  WGPUPrimitiveTopology topology = WGPUPrimitiveTopology_TriangleList;
  WGPUIndexFormat strip_index_format = WGPUIndexFormat_Undefined;
  uint32_t color_len = 0;
  WGPUTextureFormat color_formats[WG_MAX_COLOR_TARGETS] = {};
  WGPUTextureFormat depth_format = WGPUTextureFormat_Undefined;
  bool has_stencil = false;
  uint8_t stencil_write_mask = 0;
  uint8_t stencil_compare_mask = 0;
  uint32_t sample_count = 1;
  /** Hash of the vertex buffer layouts. */
  uint64_t vertex_layout_hash = 0;
  /** Hash of the specialization constant values. */
  uint64_t constants_hash = 0;

  bool operator==(const WGRenderPipelineKey &other) const;
  uint64_t hash() const;
};

struct WGVertexLayout {
  Vector<WGVertexBufferLayout, 4> buffers;
  Vector<WGVertexAttribute, 16> attributes;

  uint64_t hash() const;
};

/** Hash of specialization constant values, zero when there are none. */
uint64_t wg_constants_hash(const shader::SpecializationConstants *constants);

class WGShader : public Shader {
  WGObjectID vertex_module_ = WG_NULL_ID;
  WGObjectID fragment_module_ = WG_NULL_ID;
  WGObjectID compute_module_ = WG_NULL_ID;
  bool compilation_failed_ = false;
  bool is_compute_ = false;

  WGObjectID bind_group_layouts_[2] = {WG_NULL_ID, WG_NULL_ID};
  WGObjectID pipeline_layout_ = WG_NULL_ID;

  /** Locations of the fragment shader outputs. */
  uint32_t fragment_outputs_mask_ = 0;
  /** Specialization constant ids present in the WGSL shaders. */
  Vector<uint32_t> used_constant_ids_;

  std::mutex pipelines_mutex_;
  Map<WGRenderPipelineKey, WGObjectID> render_pipelines_;
  Map<uint64_t, WGObjectID> compute_pipelines_;

  /** Push constants, std140 layout. */
  Vector<uint8_t> push_constants_data_;

 public:
  WGShader(const char *name);
  ~WGShader() override;

  void init(const shader::ShaderCreateInfo &info, bool is_codegen_only) override;

  const shader::ShaderCreateInfo &patch_create_info(
      const shader::ShaderCreateInfo &original_info) override
  {
    return original_info;
  }

  void vertex_shader_from_glsl(const shader::ShaderCreateInfo &info,
                               MutableSpan<StringRefNull> sources) override;
  void geometry_shader_from_glsl(const shader::ShaderCreateInfo &info,
                                 MutableSpan<StringRefNull> sources) override;
  void fragment_shader_from_glsl(const shader::ShaderCreateInfo &info,
                                 MutableSpan<StringRefNull> sources) override;
  void compute_shader_from_glsl(const shader::ShaderCreateInfo &info,
                                MutableSpan<StringRefNull> sources) override;
  bool finalize(const shader::ShaderCreateInfo *info = nullptr) override;

  void warm_cache(int /*limit*/) override {}

  void bind(const shader::SpecializationConstants *constants_state) override;
  void unbind() override {}

  void uniform_float(int location, int comp_len, int array_size, const float *data) override;
  void uniform_int(int location, int comp_len, int array_size, const int *data) override;

  std::string resources_declare(const shader::ShaderCreateInfo &info) const override;
  std::string vertex_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string fragment_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string geometry_interface_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string geometry_layout_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string compute_layout_declare(const shader::ShaderCreateInfo &info) const override;

  const WGShaderInterface &interface_get() const
  {
    return *static_cast<const WGShaderInterface *>(interface);
  }
  bool is_compute() const
  {
    return is_compute_;
  }
  bool is_valid() const
  {
    return !compilation_failed_ && pipeline_layout_ != WG_NULL_ID;
  }
  WGObjectID pipeline_layout_get() const
  {
    return pipeline_layout_;
  }
  Span<uint8_t> push_constants_data() const
  {
    return push_constants_data_;
  }

  /**
   * Get the render pipeline for the given state, recording its creation when needed.
   * The recorder must not be locked.
   */
  WGObjectID render_pipeline_get(const WGRenderPipelineKey &key,
                                 const WGVertexLayout &vertex_layout,
                                 const shader::SpecializationConstants *constants);
  WGObjectID compute_pipeline_get(const shader::SpecializationConstants *constants);

 private:
  void build_module(MutableSpan<StringRefNull> sources, int stage, WGObjectID &r_module);
  Vector<uint8_t> pipeline_constants(const shader::SpecializationConstants *constants) const;
};

}  // namespace blender::gpu
