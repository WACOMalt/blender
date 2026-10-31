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
 * - Writable storage isn't allowed in vertex shaders: writable buffers and images get a second,
 *   read-only binding used by the vertex stage (#WGResourceBinding::vertex_alias).
 * - Emulated images (#WGImageMode): WebGPU has no image atomics, can only read and write 32 bit
 *   single channel storage textures and has few storage formats. Images that are only read are
 *   bound as textures. The others are emulated with a storage buffer holding the texels in the
 *   bit layout of the texture format (#WGTexelLayout) and the size of the image in the push
 *   constants; the image functions become generated helpers. The device copies the texture to
 *   the buffer before the first use of the buffer, and the buffer back to the texture before the
 *   texture is used again (#WGTexture::texture_use, #WGCmdSyncBufferImage).
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "BLI_map.hh"
#include "BLI_set.hh"
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
  /** Hash of the #WGPointVariant, zero for other draws. */
  uint64_t point_variant_hash = 0;

  bool operator==(const WGRenderPipelineKey &other) const;
  uint64_t hash() const;
};

struct WGVertexLayout {
  Vector<WGVertexBufferLayout, 4> buffers;
  Vector<WGVertexAttribute, 16> attributes;

  uint64_t hash() const;
};

/**
 * WebGPU points are always one pixel. Shaders that set a point size have a variant to draw points
 * as quads (two triangles per point). The variant reads the vertex attributes from storage
 * buffers, so it depends on the vertex layout.
 *
 * Bindings of the variant (in group 0), besides the bindings of the shader:
 * - Vertex buffers at `WG_POINT_BINDING + i`.
 * - The index buffer at `WG_POINT_BINDING + WG_MAX_VERTEX_BUFFERS`.
 * - #WGPointDrawData at `WG_POINT_BINDING + WG_MAX_VERTEX_BUFFERS + 1`.
 */
struct WGPointVariant {
  uint64_t hash = 0;
  WGObjectID vertex_module = WG_NULL_ID;
  WGObjectID bind_group_layout = WG_NULL_ID;
  WGObjectID pipeline_layout = WG_NULL_ID;
};

constexpr uint32_t WG_POINT_BINDING = 900;
constexpr uint32_t WG_POINT_INDEX_BINDING = WG_POINT_BINDING + WG_MAX_VERTEX_BUFFERS;
constexpr uint32_t WG_POINT_DRAW_DATA_BINDING = WG_POINT_INDEX_BINDING + 1;

/** Uniform data of a point variant draw, std140 layout. */
struct WGPointDrawData {
  /** First index (or vertex for non indexed draws). */
  int32_t first;
  int32_t base_vertex;
  int32_t _pad[2];
  /** One over the viewport size. */
  float viewport_inv_size[2];
  float _pad2[2];
  /** Offsets (in bytes) of the vertex buffers from the start of their binding. */
  uint32_t vertex_buffer_offsets[WG_MAX_VERTEX_BUFFERS];
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
  /** Bindings (of group 0) used by each stage (#WSC_Stage), for the stage visibility. */
  Set<uint32_t> used_bindings_[3];
  /** The used bindings of the stage are unknown, all are assumed used. */
  bool all_bindings_used_[3] = {false, false, false};
  /**
   * Vertex stage aliases the vertex shader doesn't use are left out of the layout: binding the
   * same buffer as read-only and read-write in one bind group is a usage conflict in WebGPU.
   * Aliases used while the fragment stage writes the resource are bound to a dummy buffer.
   */
  Set<uint32_t> disabled_bindings_;
  Set<uint32_t> dummy_bindings_;

  std::mutex pipelines_mutex_;
  Map<WGRenderPipelineKey, WGObjectID> render_pipelines_;
  Map<uint64_t, WGObjectID> compute_pipelines_;

  /** Push constants, std140 layout. */
  Vector<uint8_t> push_constants_data_;

 public:
  /** See #disabled_bindings_, used when gathering the resources of a draw. */
  bool binding_is_disabled(const uint32_t binding) const
  {
    return disabled_bindings_.contains(binding);
  }
  bool binding_is_dummy(const uint32_t binding) const
  {
    return dummy_bindings_.contains(binding);
  }

 private:

  /** Entries of the bind group layouts. */
  Vector<WGBindingLayout> bind_group_entries_;

  /** Vertex inputs, needed by the point variants. */
  struct VertexInput {
    uint32_t location;
    shader::Type type;
    std::string name;
  };
  Vector<VertexInput> vertex_inputs_;
  /** GLSL of the vertex shader for point variants, empty when the shader has no point size. */
  std::string point_vertex_glsl_;
  WGObjectID point_fragment_module_ = WG_NULL_ID;
  /** Location of the point coordinates passed to the fragment shader of point variants. */
  uint32_t point_coord_location_ = 0;
  /** Point variants by vertex layout, null when the layout isn't supported. */
  Map<uint64_t, std::unique_ptr<WGPointVariant>> point_variants_;

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
  /** Write raw data in the push constants (the sizes of emulated images). */
  void push_constants_set_raw(const uint32_t offset, const void *data, const size_t size)
  {
    if (offset + size <= size_t(push_constants_data_.size())) {
      memcpy(push_constants_data_.data() + offset, data, size);
    }
  }

  /**
   * Get the render pipeline for the given state, recording its creation when needed.
   * The recorder must not be locked.
   */
  WGObjectID render_pipeline_get(const WGRenderPipelineKey &key,
                                 const WGVertexLayout &vertex_layout,
                                 const shader::SpecializationConstants *constants,
                                 const WGPointVariant *point_variant = nullptr);

  /** True when points must be drawn with a point variant to have the size set by the shader. */
  bool has_point_size() const
  {
    return !point_vertex_glsl_.empty();
  }
  /**
   * Variant drawing points as quads for the vertex layout and index format
   * (#WGPUIndexFormat_Undefined for non indexed draws). Null when not supported.
   */
  const WGPointVariant *point_variant_get(const WGVertexLayout &vertex_layout,
                                          WGPUIndexFormat index_format);
  WGObjectID compute_pipeline_get(const shader::SpecializationConstants *constants);

 private:
  void build_module(const shader::ShaderCreateInfo &info,
                    MutableSpan<StringRefNull> sources,
                    int stage,
                    WGObjectID &r_module);
  Vector<uint8_t> pipeline_constants(const shader::SpecializationConstants *constants) const;
  WGObjectID module_from_glsl(const std::string &glsl, int stage);
  std::unique_ptr<WGPointVariant> point_variant_create(const WGVertexLayout &vertex_layout,
                                                       WGPUIndexFormat index_format);
};

}  // namespace blender::gpu
