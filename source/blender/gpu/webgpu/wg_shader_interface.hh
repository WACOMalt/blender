/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_shader_create_info.hh"
#include "gpu_shader_interface.hh"

#include "wg_commands.hh"

namespace blender::gpu {

/** Images share the input namespace of textures, offset to separate them (like Vulkan). */
constexpr int WG_IMAGE_BINDING_OFFSET = 512;

/** Bind group of the samplers, the sampler of a texture uses the binding of the texture. */
constexpr uint32_t WG_SAMPLER_GROUP = 1;

/** A resource of the shader, bound in bind group 0 at #binding. */
struct WGResourceBinding {
  enum class Type : uint8_t {
    UniformBuffer,
    StorageBuffer,
    Sampler,
    Image,
    /** Sub-pass inputs, read from the attachment bound as texture. */
    SubpassInput,
    /** The uniform buffer holding the push constants. */
    PushConstants,
    /**
     * Image emulated with a storage buffer (atomic images, read-write images of formats
     * WebGPU can't read and write, formats that aren't storage formats), see `wg_shader.hh`.
     */
    BufferImage,
  };
  Type type;
  /** Slot of the resource in Blender's bind spaces (texture unit, buffer slot...). */
  int slot;
  /** Binding in the WGSL shader. */
  uint32_t binding;
  WGBindingLayout layout;
  /** Kind of the sampler entry (#WG_SAMPLER_GROUP) of textures. */
  WGBindingLayout::Kind sampler_kind;
  /**
   * Writable storage isn't allowed in vertex shaders, writable buffers and images have a second,
   * read-only binding for the vertex stage (bound to the same resource).
   */
  bool vertex_alias;
  /** Images that are only read are bound as textures (#Type::Image). */
  bool image_as_texture;
  /* #Type::BufferImage. */
  /** The image is a sampler of an atomic type, `slot` is a texture unit. */
  bool sampler_slot;
  /** Expected format of the texture. */
  TextureFormat image_format;
  /** Offset of the `uvec4` with the size of the image in the push constants. */
  uint32_t meta_offset;
};

/** How an image resource is bound, see #WGResourceBinding::Type::BufferImage. */
enum class WGImageMode {
  /** A storage texture. */
  Native,
  /** A texture, for images that are only read. */
  Sampled,
  /** A storage buffer. */
  Buffer,
};

/**
 * Push constants are stored in a uniform buffer (std140 layout), WebGPU has no push constants.
 */
struct WGPushConstantsLayout {
  struct PushConstant {
    int32_t location;
    uint32_t offset;
    shader::Type type;
    int array_size;
    uint inner_row_padding;
  };
  Vector<PushConstant> push_constants;
  uint32_t size_in_bytes = 0;

  const PushConstant *find(int32_t location) const;
};

class WGShaderInterface : public ShaderInterface {
  Vector<WGResourceBinding> resources_;
  WGPushConstantsLayout push_constants_layout_;
  shader::BuiltinBits shader_builtins_;
  bool has_buffer_images_ = false;
  /** Slots of images forced to #WGImageMode::Buffer (see #image_mode_get). */
  Vector<int> buffer_mode_slots_;

 public:
  void init(const shader::ShaderCreateInfo &info);

  Span<WGResourceBinding> resources() const
  {
    return resources_;
  }
  /** Binding of a resource in the WGSL shader. */
  /** Mode of an image from its declaration alone. */
  static WGImageMode image_mode(const shader::ShaderCreateInfo::Resource &resource);
  /**
   * Mode of an image in this shader: storage textures past the device limit are emulated with
   * buffers too.
   */
  WGImageMode image_mode_get(const shader::ShaderCreateInfo::Resource &resource) const;
  bool has_buffer_images() const
  {
    return has_buffer_images_;
  }

  uint32_t binding_get(const shader::ShaderCreateInfo::Resource &resource) const;
  /** Binding of the read-only alias for the vertex stage, -1 when the resource has none. */
  int vertex_alias_binding_get(const shader::ShaderCreateInfo::Resource &resource) const;
  uint32_t subpass_input_binding_get(int index) const;
  /** Binding of the push constants buffer, only valid when there are push constants. */
  uint32_t push_constants_binding_get() const;

  const WGPushConstantsLayout &push_constants_layout() const
  {
    return push_constants_layout_;
  }

  shader::Type attribute_type_get(int location) const
  {
    return static_cast<shader::Type>(attr_types_[location]);
  }

 private:
  void populate_inputs(const shader::ShaderCreateInfo &info);
  void populate_builtins();
  void populate_resources(const shader::ShaderCreateInfo &info);
  void populate_push_constants(const shader::ShaderCreateInfo &info);
};

}  // namespace blender::gpu
