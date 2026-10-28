/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "MEM_guardedalloc.h"

#include "vulkan/vk_memory_layout.hh"

#include "wg_common.hh"
#include "wg_shader_interface.hh"

namespace blender::gpu {

using namespace blender::gpu::shader;
using Resource = ShaderCreateInfo::Resource;

/* -------------------------------------------------------------------- */
/** \name Resource Layouts
 * \{ */

static WGPUTextureViewDimension wg_view_dimension(const ImageType type)
{
  switch (type) {
    case ImageType::Float1D:
    case ImageType::Int1D:
    case ImageType::Uint1D:
      return WGPUTextureViewDimension_1D;
    case ImageType::Float2DArray:
    case ImageType::Int2DArray:
    case ImageType::Uint2DArray:
    case ImageType::Shadow2DArray:
    case ImageType::Depth2DArray:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicUint2DArray:
    /* WGSL has no 1D arrays, they are not supported. */
    case ImageType::Float1DArray:
    case ImageType::Int1DArray:
    case ImageType::Uint1DArray:
      return WGPUTextureViewDimension_2DArray;
    case ImageType::Float3D:
    case ImageType::Int3D:
    case ImageType::Uint3D:
    case ImageType::AtomicInt3D:
    case ImageType::AtomicUint3D:
      return WGPUTextureViewDimension_3D;
    case ImageType::FloatCube:
    case ImageType::IntCube:
    case ImageType::UintCube:
    case ImageType::ShadowCube:
    case ImageType::DepthCube:
      return WGPUTextureViewDimension_Cube;
    case ImageType::FloatCubeArray:
    case ImageType::IntCubeArray:
    case ImageType::UintCubeArray:
    case ImageType::ShadowCubeArray:
    case ImageType::DepthCubeArray:
      return WGPUTextureViewDimension_CubeArray;
    default:
      return WGPUTextureViewDimension_2D;
  }
}

static WGPUTextureSampleType wg_sample_type(const ImageType type)
{
  switch (type) {
    case ImageType::IntBuffer:
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Int3D:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicInt3D:
      return WGPUTextureSampleType_Sint;
    case ImageType::UintBuffer:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Uint3D:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint2DArray:
    case ImageType::AtomicUint3D:
      return WGPUTextureSampleType_Uint;
    case ImageType::Shadow2D:
    case ImageType::Shadow2DArray:
    case ImageType::ShadowCube:
    case ImageType::ShadowCubeArray:
      return WGPUTextureSampleType_Depth;
    case ImageType::Depth2D:
    case ImageType::Depth2DArray:
    case ImageType::DepthCube:
    case ImageType::DepthCubeArray:
      /* Depth textures can't be filtered. */
      return WGPUTextureSampleType_UnfilterableFloat;
    default:
      return WGPUTextureSampleType_Float;
  }
}

static WGBindingLayout::Kind wg_sampler_kind(const WGPUTextureSampleType sample_type)
{
  switch (sample_type) {
    case WGPUTextureSampleType_Float:
      return WGBindingLayout::Kind::Sampler;
    case WGPUTextureSampleType_Depth:
      return WGBindingLayout::Kind::ComparisonSampler;
    default:
      return WGBindingLayout::Kind::NonFilteringSampler;
  }
}

static WGPUStorageTextureAccess wg_storage_access(const Qualifier qualifiers)
{
  const bool read = flag_is_set(qualifiers, Qualifier::read);
  const bool write = flag_is_set(qualifiers, Qualifier::write);
  if (read && write) {
    return WGPUStorageTextureAccess_ReadWrite;
  }
  if (write) {
    return WGPUStorageTextureAccess_WriteOnly;
  }
  return WGPUStorageTextureAccess_ReadOnly;
}

/** \} */

const WGPushConstantsLayout::PushConstant *WGPushConstantsLayout::find(int32_t location) const
{
  for (const PushConstant &push_constant : push_constants) {
    if (push_constant.location == location) {
      return &push_constant;
    }
  }
  return nullptr;
}

static Vector<Resource> all_resources_get(const ShaderCreateInfo &info)
{
  Vector<Resource> all_resources;
  all_resources.extend(info.pass_resources_);
  all_resources.extend(info.batch_resources_);
  all_resources.extend(info.geometry_resources_);
  return all_resources;
}

void WGShaderInterface::init(const ShaderCreateInfo &info)
{
  shader_builtins_ = info.builtins_combined();
  populate_inputs(info);
  populate_builtins();
  populate_resources(info);
  populate_push_constants(info);
}

void WGShaderInterface::populate_inputs(const ShaderCreateInfo &info)
{
  const Vector<Resource> all_resources = all_resources_get(info);

  attr_len_ = info.vertex_inputs_.size();
  uniform_len_ = info.push_constants_.size() + info.subpass_inputs_.size();
  constant_len_ = info.specialization_constants_.size();
  for (const Resource &res : all_resources) {
    switch (res.bind_type) {
      case Resource::BindType::IMAGE:
      case Resource::BindType::SAMPLER:
        uniform_len_++;
        break;
      case Resource::BindType::UNIFORM_BUFFER:
        ubo_len_++;
        break;
      case Resource::BindType::STORAGE_BUFFER:
        ssbo_len_++;
        break;
      case Resource::BindType::ACCELERATION_STRUCTURE:
        tlas_len_++;
        break;
    }
  }

  static const char SUBPASS_NAME[] = "gpu_subpass_img_0";
  const size_t names_size = info.interface_names_size_ +
                            info.subpass_inputs_.size() * sizeof(SUBPASS_NAME);
  const int inputs_len = attr_len_ + ubo_len_ + uniform_len_ + ssbo_len_ + constant_len_ +
                         tlas_len_;
  inputs_ = MEM_new_array_zeroed<ShaderInput>(inputs_len, __func__);
  name_buffer_ = MEM_new_array_uninitialized<char>(names_size, "name_buffer");
  ShaderInput *input = inputs_;
  uint32_t name_buffer_offset = 0;

  /* Attributes. */
  for (const ShaderCreateInfo::VertIn &attr : info.vertex_inputs_) {
    copy_input_name(input, attr.name, name_buffer_, name_buffer_offset);
    input->location = input->binding = attr.index;
    if (input->location != -1) {
      enabled_attr_mask_ |= (1 << input->location);
      attr_types_[input->location] = uint8_t(attr.type);
    }
    input++;
  }
  /* Uniform blocks. */
  for (const Resource &res : all_resources) {
    if (res.bind_type == Resource::BindType::UNIFORM_BUFFER) {
      copy_input_name(input, res.uniformbuf.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot;
      input++;
    }
  }
  /* Textures, images and push constants. */
  for (const ShaderCreateInfo::SubpassIn &subpass_in : info.subpass_inputs_) {
    copy_input_name(input, SUBPASS_NAME, name_buffer_, name_buffer_offset);
    input->location = input->binding = subpass_in.index;
    input++;
  }
  for (const Resource &res : all_resources) {
    if (res.bind_type == Resource::BindType::SAMPLER) {
      copy_input_name(input, res.sampler.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot;
      input++;
    }
    else if (res.bind_type == Resource::BindType::IMAGE) {
      copy_input_name(input, res.image.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot + WG_IMAGE_BINDING_OFFSET;
      input++;
    }
  }
  set_image_formats_from_info(info);
  int32_t push_constant_location = 1024;
  for (const ShaderCreateInfo::PushConst &push_constant : info.push_constants_) {
    copy_input_name(input, push_constant.name, name_buffer_, name_buffer_offset);
    input->location = push_constant_location++;
    input->binding = -1;
    input++;
  }
  /* Storage buffers. */
  for (const Resource &res : all_resources) {
    if (res.bind_type == Resource::BindType::STORAGE_BUFFER) {
      copy_input_name(input, res.storagebuf.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot;
      input++;
    }
  }
  for (const Resource &res : info.geometry_resources_) {
    if (res.bind_type == Resource::BindType::STORAGE_BUFFER) {
      ssbo_attr_mask_ |= (1 << res.slot);
    }
  }
  /* Acceleration structures (not supported, but part of the interface). */
  for (const Resource &res : all_resources) {
    if (res.bind_type == Resource::BindType::ACCELERATION_STRUCTURE) {
      copy_input_name(input, res.acceleration_structure.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot;
      input++;
    }
  }
  /* Specialization constants. */
  int constant_id = 0;
  for (const SpecializationConstant &constant : info.specialization_constants_) {
    copy_input_name(input, constant.name, name_buffer_, name_buffer_offset);
    input->location = constant_id++;
    input++;
  }

  sort_inputs();
}

void WGShaderInterface::populate_builtins()
{
  for (int32_t u_int = 0; u_int < GPU_NUM_UNIFORMS; u_int++) {
    GPUUniformBuiltin u = static_cast<GPUUniformBuiltin>(u_int);
    const ShaderInput *uni = this->uniform_get(builtin_uniform_name(u));
    builtins_[u] = (uni != nullptr) ? uni->location : -1;
  }
  for (int32_t u_int = 0; u_int < GPU_NUM_UNIFORM_BLOCKS; u_int++) {
    GPUUniformBlockBuiltin u = static_cast<GPUUniformBlockBuiltin>(u_int);
    const ShaderInput *block = this->ubo_get(builtin_uniform_block_name(u));
    builtin_blocks_[u] = (block != nullptr) ? block->binding : -1;
  }
}

void WGShaderInterface::populate_resources(const ShaderCreateInfo &info)
{
  const bool is_compute = !info.compute_source_.is_empty() ||
                          !info.compute_source_generated.empty();
  const WGPUShaderStage all_stages = is_compute ? WGPUShaderStage_Compute :
                                                  WGPUShaderStage_Vertex |
                                                      WGPUShaderStage_Fragment;
  /* Writable storage isn't allowed in vertex shaders. */
  const WGPUShaderStage writable_stages = is_compute ? WGPUShaderStage_Compute :
                                                       WGPUShaderStage_Fragment;

  uint32_t binding = 0;
  auto add_texture = [&](WGResourceBinding::Type type, int slot, ImageType image_type) {
    WGResourceBinding resource = {};
    resource.type = type;
    resource.slot = slot;
    resource.binding = binding++;
    resource.layout.binding = resource.binding;
    resource.layout.kind = WGBindingLayout::Kind::Texture;
    resource.layout.visibility = all_stages;
    resource.layout.sample_type = wg_sample_type(image_type);
    resource.layout.view_dimension = wg_view_dimension(image_type);
    resource.sampler_kind = wg_sampler_kind(resource.layout.sample_type);
    resources_.append(resource);
  };

  for (const ShaderCreateInfo::SubpassIn &subpass_in : info.subpass_inputs_) {
    add_texture(WGResourceBinding::Type::SubpassInput, subpass_in.index, subpass_in.img_type);
  }
  for (const Resource &res : all_resources_get(info)) {
    switch (res.bind_type) {
      case Resource::BindType::SAMPLER:
        add_texture(WGResourceBinding::Type::Sampler, res.slot, res.sampler.type);
        break;
      case Resource::BindType::IMAGE: {
        WGResourceBinding resource = {};
        resource.type = WGResourceBinding::Type::Image;
        resource.slot = res.slot;
        resource.binding = binding++;
        resource.layout.binding = resource.binding;
        resource.layout.kind = WGBindingLayout::Kind::StorageTexture;
        resource.layout.storage_access = wg_storage_access(res.image.qualifiers);
        resource.layout.visibility = resource.layout.storage_access ==
                                             WGPUStorageTextureAccess_ReadOnly ?
                                         all_stages :
                                         writable_stages;
        resource.layout.view_dimension = wg_view_dimension(res.image.type);
        resource.layout.storage_format = wg_texture_format(TextureFormat(res.image.format));
        resources_.append(resource);
        break;
      }
      case Resource::BindType::UNIFORM_BUFFER: {
        WGResourceBinding resource = {};
        resource.type = WGResourceBinding::Type::UniformBuffer;
        resource.slot = res.slot;
        resource.binding = binding++;
        resource.layout.binding = resource.binding;
        resource.layout.kind = WGBindingLayout::Kind::UniformBuffer;
        resource.layout.visibility = all_stages;
        resources_.append(resource);
        break;
      }
      case Resource::BindType::STORAGE_BUFFER: {
        const bool is_writable = flag_is_set(res.storagebuf.qualifiers, Qualifier::write);
        WGResourceBinding resource = {};
        resource.type = WGResourceBinding::Type::StorageBuffer;
        resource.slot = res.slot;
        resource.binding = binding++;
        resource.layout.binding = resource.binding;
        resource.layout.kind = is_writable ? WGBindingLayout::Kind::StorageBuffer :
                                             WGBindingLayout::Kind::ReadOnlyStorageBuffer;
        resource.layout.visibility = is_writable ? writable_stages : all_stages;
        resources_.append(resource);
        break;
      }
      case Resource::BindType::ACCELERATION_STRUCTURE:
        /* Not supported, still reserve a binding to keep the declaration valid. */
        binding++;
        break;
    }
  }
  if (!info.push_constants_.is_empty()) {
    WGResourceBinding resource = {};
    resource.type = WGResourceBinding::Type::PushConstants;
    resource.slot = -1;
    resource.binding = binding++;
    resource.layout.binding = resource.binding;
    resource.layout.kind = WGBindingLayout::Kind::UniformBuffer;
    resource.layout.visibility = all_stages;
    resources_.append(resource);
  }
}

void WGShaderInterface::populate_push_constants(const ShaderCreateInfo &info)
{
  uint32_t offset = 0;
  for (const ShaderCreateInfo::PushConst &push_constant : info.push_constants_) {
    const ShaderInput *shader_input = uniform_get(push_constant.name.c_str());
    align<Std140>(push_constant.type, push_constant.array_size, &offset);
    WGPushConstantsLayout::PushConstant layout;
    layout.location = shader_input->location;
    layout.type = push_constant.type;
    layout.array_size = push_constant.array_size;
    layout.offset = offset;
    layout.inner_row_padding = Std140::inner_row_padding(push_constant.type);
    push_constants_layout_.push_constants.append(layout);
    reserve<Std140>(push_constant.type, push_constant.array_size, &offset);
  }
  align_end_of_struct<Std140>(&offset);
  push_constants_layout_.size_in_bytes = offset;
}

static const WGResourceBinding *find_resource(Span<WGResourceBinding> resources,
                                              WGResourceBinding::Type type,
                                              int slot)
{
  for (const WGResourceBinding &resource : resources) {
    if (resource.type == type && resource.slot == slot) {
      return &resource;
    }
  }
  return nullptr;
}

uint32_t WGShaderInterface::binding_get(const Resource &resource) const
{
  WGResourceBinding::Type type = WGResourceBinding::Type::Sampler;
  switch (resource.bind_type) {
    case Resource::BindType::SAMPLER:
      type = WGResourceBinding::Type::Sampler;
      break;
    case Resource::BindType::IMAGE:
      type = WGResourceBinding::Type::Image;
      break;
    case Resource::BindType::UNIFORM_BUFFER:
      type = WGResourceBinding::Type::UniformBuffer;
      break;
    case Resource::BindType::STORAGE_BUFFER:
      type = WGResourceBinding::Type::StorageBuffer;
      break;
    case Resource::BindType::ACCELERATION_STRUCTURE:
      return 0;
  }
  const WGResourceBinding *binding = find_resource(resources_, type, resource.slot);
  BLI_assert(binding);
  return binding ? binding->binding : 0;
}

uint32_t WGShaderInterface::subpass_input_binding_get(int index) const
{
  const WGResourceBinding *binding = find_resource(
      resources_, WGResourceBinding::Type::SubpassInput, index);
  BLI_assert(binding);
  return binding ? binding->binding : 0;
}

uint32_t WGShaderInterface::push_constants_binding_get() const
{
  const WGResourceBinding *binding = find_resource(
      resources_, WGResourceBinding::Type::PushConstants, -1);
  BLI_assert(binding);
  return binding ? binding->binding : 0;
}

}  // namespace blender::gpu
