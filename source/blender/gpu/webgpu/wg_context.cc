/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GHOST_IWindow.hh"

#include "BLI_math_base.hh"

#include "GPU_vertex_format.hh"

#include "wg_backend.hh"
#include "wg_buffer.hh"
#include "wg_common.hh"
#include "wg_context.hh"
#include "wg_framebuffer.hh"
#include "wg_immediate.hh"
#include "wg_state.hh"
#include "wg_texture.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Context
 * \{ */

WGContext::WGContext(GHOST_IWindow *ghost_window)
{
  ghost_window_ = ghost_window;
  state_manager = new WGStateManager();
  imm = new WGImmediate();

  back_left = new WGFrameBuffer("back_left");
  front_left = back_left;
  active_fb = back_left;
}

WGContext::~WGContext()
{
  if (Context::get() == this) {
    flush();
  }
  delete imm;
  imm = nullptr;
  delete back_left;
  back_left = nullptr;
  front_left = nullptr;
  if (window_texture_) {
    GPU_texture_free(window_texture_);
    window_texture_ = nullptr;
  }
  delete state_manager;
  state_manager = nullptr;
}

WGStateManager &WGContext::state_manager_get() const
{
  return *static_cast<WGStateManager *>(state_manager);
}

WGFrameBuffer *WGContext::active_framebuffer_get() const
{
  return static_cast<WGFrameBuffer *>(active_fb);
}

void WGContext::window_size_sync()
{
  if (ghost_window_ == nullptr) {
    return;
  }
  GHOST_Rect bounds;
  ghost_window_->getClientBounds(bounds);
  const int width = max_ii(bounds.getWidth(), 1);
  const int height = max_ii(bounds.getHeight(), 1);
  if (window_texture_ && GPU_texture_width(window_texture_) == width &&
      GPU_texture_height(window_texture_) == height)
  {
    return;
  }
  WGFrameBuffer *framebuffer = static_cast<WGFrameBuffer *>(back_left);
  framebuffer->render_pass_end();
  if (window_texture_) {
    GPU_texture_free(window_texture_);
  }
  window_texture_ = static_cast<WGTexture *>(
      GPU_texture_create_2d("window",
                            width,
                            height,
                            1,
                            TextureFormat::UNORM_8_8_8_8,
                            GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ,
                            nullptr));
  framebuffer->attachment_set(GPU_FB_COLOR_ATTACHMENT0, GPU_ATTACHMENT_TEXTURE(window_texture_));
  framebuffer->size_set(width, height);
  framebuffer->viewport_reset();
  framebuffer->scissor_reset();
}

void WGContext::activate()
{
  thread_ = pthread_self();
  is_active_ = true;
  window_size_sync();
  immActivate();
}

void WGContext::deactivate()
{
  immDeactivate();
  is_active_ = false;
}

void WGContext::begin_frame()
{
  window_size_sync();
}

void WGContext::end_frame()
{
  if (ghost_window_ && window_texture_) {
    static_cast<WGFrameBuffer *>(back_left)->render_pass_end();
    WGRecord record;
    record.stream.push(WGCommandType::Present, WGCmdPresent{window_texture_->id_get()});
  }
  flush();
}

void WGContext::flush()
{
  WGCommandStream stream;
  {
    WGRecord record;
    if (record.stream.is_empty()) {
      return;
    }
    wg_recorder().take(stream);
  }
  WGBackend::get().device.submit(stream);
}

void WGContext::finish()
{
  WGCommandStream stream;
  {
    WGRecord record;
    wg_recorder().take(stream);
  }
  WGBackend::get().device.submit_and_wait(stream);
}

void WGContext::memory_statistics_get(int *r_total_mem, int *r_free_mem)
{
  *r_total_mem = 0;
  *r_free_mem = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Input
 * \{ */

void wg_vertex_layout_add(const GPUVertFormat &format,
                          const WGObjectID buffer,
                          const uint32_t vertex_len,
                          const bool per_instance,
                          const WGShaderInterface &interface,
                          uint16_t &r_occupied,
                          WGDrawCall &r_call)
{
  if (format.attr_len == 0) {
    return;
  }
  const WGPUVertexStepMode step_mode = per_instance ? WGPUVertexStepMode_Instance :
                                                      WGPUVertexStepMode_Vertex;
  /* Index of the buffer of interleaved formats, -1 until used. */
  int interleaved_buffer = -1;
  uint64_t deinterleaved_offset = 0;
  for (uint32_t attr_index = 0; attr_index < format.attr_len; attr_index++) {
    const GPUVertAttr &attr = format.attrs[attr_index];
    if (format.deinterleaved && attr_index > 0) {
      deinterleaved_offset += format.attrs[attr_index - 1].type.size() * vertex_len;
    }
    const WGPUVertexFormat wg_format = wg_vertex_format(attr.type.format);
    if (wg_format == WGPUVertexFormat_Force32) {
      continue;
    }
    for (uint32_t name_index = 0; name_index < attr.name_len; name_index++) {
      const char *name = GPU_vertformat_attr_name_get(&format, &attr, name_index);
      const ShaderInput *input = interface.attr_get(name);
      if (input == nullptr || input->location == -1) {
        continue;
      }
      const uint16_t mask = uint16_t(1u << input->location);
      if (r_occupied & mask) {
        continue;
      }
      r_occupied |= mask;

      int buffer_index;
      if (format.deinterleaved) {
        buffer_index = -1;
      }
      else {
        buffer_index = interleaved_buffer;
      }
      if (buffer_index == -1) {
        buffer_index = r_call.vertex_layout.buffers.size();
        WGVertexBufferLayout layout = {};
        layout.stride = format.deinterleaved ? uint32_t(attr.type.size()) : format.stride;
        layout.step_mode = step_mode;
        layout.attributes_start = 0;
        layout.attributes_len = 0;
        r_call.vertex_layout.buffers.append(layout);
        r_call.vertex_buffers.append(
            {buffer, format.deinterleaved ? deinterleaved_offset : uint64_t(0)});
        if (!format.deinterleaved) {
          interleaved_buffer = buffer_index;
        }
      }
      WGVertexAttribute attribute = {};
      attribute.format = wg_format;
      attribute.offset = format.deinterleaved ? 0 : attr.offset;
      attribute.location = uint32_t(input->location);
      r_call.vertex_layout.attributes.append(attribute);
      WGVertexBufferLayout &layout = r_call.vertex_layout.buffers[buffer_index];
      layout.attributes_len++;
    }
  }
}

void wg_vertex_layout_finish(const WGShaderInterface &interface,
                             const uint16_t occupied,
                             WGDrawCall &r_call)
{
  /* Attributes are added buffer after buffer. */
  uint32_t attribute_index = 0;
  for (WGVertexBufferLayout &layout : r_call.vertex_layout.buffers) {
    layout.attributes_start = attribute_index;
    attribute_index += layout.attributes_len;
  }

  const uint16_t missing = interface.enabled_attr_mask_ & ~occupied;
  if (missing == 0) {
    return;
  }
  WGVertexBufferLayout layout = {};
  layout.stride = 0;
  layout.step_mode = WGPUVertexStepMode_Vertex;
  layout.attributes_start = r_call.vertex_layout.attributes.size();
  for (int location = 0; location < 16; location++) {
    if (!(missing & (1u << location))) {
      continue;
    }
    WGVertexAttribute attribute = {};
    attribute.offset = 0;
    attribute.location = uint32_t(location);
    switch (interface.attribute_type_get(location)) {
      case shader::Type::int_t:
      case shader::Type::int2_t:
      case shader::Type::int3_t:
      case shader::Type::int4_t:
        attribute.format = WGPUVertexFormat_Sint32x4;
        break;
      case shader::Type::uint_t:
      case shader::Type::uint2_t:
      case shader::Type::uint3_t:
      case shader::Type::uint4_t:
        attribute.format = WGPUVertexFormat_Uint32x4;
        break;
      default:
        /* The missing 4th component is 1, which is useful for positions. */
        attribute.format = WGPUVertexFormat_Float32x3;
        break;
    }
    r_call.vertex_layout.attributes.append(attribute);
    layout.attributes_len++;
  }
  r_call.vertex_layout.buffers.append(layout);
  r_call.vertex_buffers.append({WGBackend::get().dummy_buffer_get(), 0});
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resources
 * \{ */

static bool wg_topology(const GPUPrimType prim_type, WGPUPrimitiveTopology &r_topology)
{
  switch (prim_type) {
    case GPU_PRIM_POINTS:
      r_topology = WGPUPrimitiveTopology_PointList;
      return true;
    case GPU_PRIM_LINES:
      r_topology = WGPUPrimitiveTopology_LineList;
      return true;
    case GPU_PRIM_LINE_STRIP:
      r_topology = WGPUPrimitiveTopology_LineStrip;
      return true;
    case GPU_PRIM_TRIS:
      r_topology = WGPUPrimitiveTopology_TriangleList;
      return true;
    case GPU_PRIM_TRI_STRIP:
      r_topology = WGPUPrimitiveTopology_TriangleStrip;
      return true;
    default:
      /* Fans, loops and adjacency primitives are not supported by WebGPU. */
      return false;
  }
}

/** Fill the texture view of `r_resource`, using a dummy texture when `texture` can't be used. */
static void wg_texture_binding(WGTexture *texture,
                               const WGBindingLayout &layout,
                               const bool is_storage,
                               WGBindingResource &r_resource)
{
  const WGPUTextureViewDimension dimension = layout.view_dimension;
  bool usable = texture && texture->is_usable();
  if (usable) {
    const GPUTextureType type = texture->type_get();
    const bool is_3d = type == GPU_TEXTURE_3D;
    const bool is_1d = type == GPU_TEXTURE_1D;
    usable = (is_3d == (dimension == WGPUTextureViewDimension_3D)) &&
             (is_1d == (dimension == WGPUTextureViewDimension_1D));
    if (ELEM(dimension, WGPUTextureViewDimension_Cube, WGPUTextureViewDimension_CubeArray)) {
      usable &= texture->wg_layers() >= 6;
    }
    if (is_storage) {
      usable &= texture->wg_format_get() == layout.storage_format;
    }
  }
  r_resource.view_dimension = dimension;
  if (!usable) {
    const WGBackend::DummyTexture dummy = WGBackend::get().dummy_texture_get(
        dimension,
        layout.sample_type,
        is_storage ? layout.storage_format : WGPUTextureFormat_Undefined);
    r_resource.object = dummy.texture;
    r_resource.view_format = dummy.format;
    r_resource.aspect = WGPUTextureAspect_All;
    r_resource.base_mip = 0;
    r_resource.mip_count = 1;
    r_resource.base_layer = 0;
    r_resource.layer_count = ELEM(dimension,
                                  WGPUTextureViewDimension_Cube,
                                  WGPUTextureViewDimension_CubeArray) ?
                                 6 :
                                 1;
    return;
  }

  r_resource.object = texture->id_get();
  r_resource.view_format = texture->wg_format_get();
  r_resource.aspect = (texture->format_flag_get() & GPU_FORMAT_DEPTH) ?
                          WGPUTextureAspect_DepthOnly :
                          WGPUTextureAspect_All;
  const IndexRange mips = texture->sampling_mips();
  r_resource.base_mip = uint32_t(mips.start());
  r_resource.mip_count = is_storage ? 1 : uint32_t(mips.size());
  const int base_layer = (texture->type_get() == GPU_TEXTURE_3D) ? 0 :
                                                                   texture->view_layer_start();
  r_resource.base_layer = uint32_t(base_layer);
  switch (dimension) {
    case WGPUTextureViewDimension_2DArray:
      r_resource.layer_count = uint32_t(max_ii(texture->layer_count(), 1));
      break;
    case WGPUTextureViewDimension_Cube:
      r_resource.layer_count = 6;
      break;
    case WGPUTextureViewDimension_CubeArray:
      r_resource.layer_count = uint32_t((texture->wg_layers() - base_layer) / 6 * 6);
      break;
    default:
      r_resource.layer_count = 1;
      break;
  }
}

static void wg_buffer_binding(const WGBufferBinding &buffer, WGBindingResource &r_resource)
{
  if (buffer.buffer == WG_NULL_ID) {
    r_resource.object = WGBackend::get().dummy_buffer_get();
    r_resource.offset = 0;
    r_resource.size = WG_DUMMY_BUFFER_SIZE;
    return;
  }
  r_resource.object = buffer.buffer;
  r_resource.offset = buffer.offset;
  r_resource.size = WGCommandStream::align_up(buffer.size, 4);
}

/**
 * Resources of the bound shader. Uniform buffers and push constants are stored in the stream
 * data, they are resolved by #wg_resources_resolve_stream_data with the recorder locked.
 */
struct WGPendingResources {
  Vector<WGBindingResource, 16> resources;
  /** Resources with the data of a uniform buffer. */
  Vector<std::pair<int, WGUniformBuf *>, 8> uniform_buffers;
  /** Resource of the push constants, -1 when there are none. */
  int push_constants = -1;
};

static void wg_resources_get(WGContext &context, WGShader &shader, WGPendingResources &r_pending)
{
  WGStateManager &state_manager = context.state_manager_get();
  WGBackend &backend = WGBackend::get();

  for (const WGResourceBinding &resource : shader.interface_get().resources()) {
    if (shader.binding_is_disabled(resource.binding)) {
      continue;
    }
    WGBindingResource binding = {};
    binding.group = 0;
    binding.binding = resource.binding;
    binding.kind = resource.layout.kind;
    if (shader.binding_is_dummy(resource.binding)) {
      binding.object = backend.dummy_buffer_get();
      binding.offset = 0;
      binding.size = WG_DUMMY_BUFFER_SIZE;
      r_pending.resources.append(binding);
      continue;
    }
    switch (resource.type) {
      case WGResourceBinding::Type::Sampler:
      case WGResourceBinding::Type::SubpassInput: {
        WGTexture *texture = nullptr;
        GPUSamplerState sampler_state = GPUSamplerState::default_sampler();
        if (resource.type == WGResourceBinding::Type::Sampler) {
          if (resource.slot >= 0 && resource.slot < WG_MAX_TEXTURE_UNITS) {
            const WGStateManager::TextureBinding &texture_binding =
                state_manager.textures[resource.slot];
            texture = texture_binding.texture;
            sampler_state = texture_binding.sampler;
          }
        }
        else if (WGFrameBuffer *framebuffer = context.active_framebuffer_get()) {
          texture = framebuffer->color_texture(resource.slot);
        }
        if (texture && sampler_state.type == GPU_SAMPLER_STATE_TYPE_INTERNAL) {
          sampler_state = texture->sampler_state;
        }
        if (texture && texture->type_get() == GPU_TEXTURE_BUFFER) {
          texture->buffer_texture_update();
        }
        wg_texture_binding(texture, resource.layout, false, binding);
        if (texture && texture->is_usable()) {
          texture->texture_use(IndexRange(binding.base_mip, binding.mip_count), false);
        }
        r_pending.resources.append(binding);

        WGBindingResource sampler = {};
        sampler.group = WG_SAMPLER_GROUP;
        sampler.binding = resource.binding;
        sampler.kind = resource.sampler_kind;
        sampler.object = backend.sampler_get(sampler_state, resource.sampler_kind);
        r_pending.resources.append(sampler);
        break;
      }
      case WGResourceBinding::Type::Image: {
        WGTexture *texture = (resource.slot >= 0 && resource.slot < WG_MAX_IMAGE_UNITS) ?
                                 state_manager.images[resource.slot] :
                                 nullptr;
        const bool as_texture = resource.image_as_texture;
        wg_texture_binding(texture, resource.layout, !as_texture, binding);
        if (texture && texture->is_usable()) {
          const bool writes = !as_texture && resource.layout.storage_access !=
                                                 WGPUStorageTextureAccess_ReadOnly;
          texture->texture_use(IndexRange(binding.base_mip, binding.mip_count), writes);
        }
        r_pending.resources.append(binding);
        if (as_texture) {
          WGBindingResource sampler = {};
          sampler.group = WG_SAMPLER_GROUP;
          sampler.binding = resource.binding;
          sampler.kind = resource.sampler_kind;
          sampler.object = backend.sampler_get(GPUSamplerState::default_sampler(),
                                               resource.sampler_kind);
          r_pending.resources.append(sampler);
        }
        break;
      }
      case WGResourceBinding::Type::BufferImage: {
        /* The buffer of an emulated image, see `wg_shader.hh`. */
        WGTexture *texture = nullptr;
        if (resource.sampler_slot) {
          if (resource.slot >= 0 && resource.slot < WG_MAX_TEXTURE_UNITS) {
            texture = state_manager.textures[resource.slot].texture;
          }
        }
        else if (resource.slot >= 0 && resource.slot < WG_MAX_IMAGE_UNITS) {
          texture = state_manager.images[resource.slot];
        }
        uint32_t meta[4] = {0, 0, 0, 0};
        const bool usable = texture && texture->is_usable() &&
                            texture->device_format_get() == resource.image_format &&
                            texture->type_get() != GPU_TEXTURE_BUFFER;
        if (usable) {
          const int mip = texture->sampling_mips().start();
          const bool writable = resource.layout.kind == WGBindingLayout::Kind::StorageBuffer;
          texture->buffer_image_use(mip, writable);
          binding.object = texture->id_get();
          binding.base_mip = uint32_t(mip);
          binding.buffer_image = true;
          binding.offset = 0;
          binding.size = 0;
          const int3 size = texture->mip_size_get(mip - texture->view_mip_start());
          meta[0] = uint32_t(max_ii(size.x, 1));
          meta[1] = uint32_t(max_ii(size.y, 1));
          meta[2] = uint32_t(max_ii(size.z, 1));
          WGTexelLayout layout;
          if (wg_texel_layout_get(texture->device_format_get(), layout)) {
            meta[3] = uint32_t(layout.words);
          }
        }
        else {
          binding.object = backend.dummy_buffer_get();
          binding.offset = 0;
          binding.size = WG_DUMMY_BUFFER_SIZE;
        }
        shader.push_constants_set_raw(resource.meta_offset, meta, sizeof(meta));
        r_pending.resources.append(binding);
        break;
      }
      case WGResourceBinding::Type::UniformBuffer:
      case WGResourceBinding::Type::StorageBuffer: {
        const bool is_uniform = resource.type == WGResourceBinding::Type::UniformBuffer;
        WGStateManager::UniformBinding buffer = {};
        if (resource.slot >= 0 && resource.slot < WG_MAX_BUFFER_SLOTS) {
          buffer = is_uniform ? state_manager.uniform_buffers[resource.slot] :
                                state_manager.storage_buffers[resource.slot];
        }
        if (buffer.uniform_buffer) {
          r_pending.uniform_buffers.append({int(r_pending.resources.size()),
                                            buffer.uniform_buffer});
        }
        else {
          wg_buffer_binding(buffer.buffer, binding);
        }
        r_pending.resources.append(binding);
        break;
      }
      case WGResourceBinding::Type::PushConstants:
        r_pending.push_constants = int(r_pending.resources.size());
        r_pending.resources.append(binding);
        break;
    }
  }
}

/** Store the uniform data in the stream, the recorder must be locked. */
static void wg_resources_resolve_stream_data(WGShader &shader,
                                             WGCommandStream &stream,
                                             const uint64_t generation,
                                             WGPendingResources &pending)
{
  for (const auto &[index, uniform_buffer] : pending.uniform_buffers) {
    wg_buffer_binding(uniform_buffer->binding_ensure(stream, generation),
                      pending.resources[index]);
  }
  if (pending.push_constants != -1) {
    const Span<uint8_t> data = shader.push_constants_data();
    WGBindingResource &resource = pending.resources[pending.push_constants];
    resource.object = WG_STREAM_DATA_ID;
    resource.offset = stream.staging_push(data.data(), data.size());
    resource.size = data.size();
  }
}

/**
 * Bind the vertex and index buffers as storage buffers of a point variant, and turn the draw
 * into a non indexed draw of two triangles per point. The recorder must be locked.
 */
static void wg_point_variant_resources(const WGDrawCall &call,
                                       WGCommandStream &stream,
                                       const uint64_t stream_vertex_offset,
                                       WGCmdDraw &cmd,
                                       WGPendingResources &pending)
{
  const uint64_t alignment = WGBackend::get().device.info().limits.minStorageBufferOffsetAlignment;

  WGPointDrawData data = {};
  data.first = int32_t(call.first);
  data.base_vertex = call.base_vertex;
  data.viewport_inv_size[0] = 1.0f / max_ff(cmd.viewport[2], 1.0f);
  data.viewport_inv_size[1] = 1.0f / max_ff(cmd.viewport[3], 1.0f);

  auto add_buffer = [&](const uint32_t binding, const WGObjectID buffer, const uint64_t offset) {
    WGBindingResource resource = {};
    resource.group = 0;
    resource.binding = binding;
    resource.kind = WGBindingLayout::Kind::ReadOnlyStorageBuffer;
    resource.object = buffer;
    resource.offset = offset;
    resource.size = 0;
    pending.resources.append(resource);
  };
  for (const int i : call.vertex_buffers.index_range()) {
    uint64_t offset = call.vertex_buffers[i].offset;
    if (call.vertex_buffers[i].buffer == WG_STREAM_DATA_ID) {
      offset += stream_vertex_offset;
    }
    const uint64_t aligned_offset = offset - offset % alignment;
    data.vertex_buffer_offsets[i] = uint32_t(offset - aligned_offset);
    add_buffer(WG_POINT_BINDING + i, call.vertex_buffers[i].buffer, aligned_offset);
  }
  if (call.index_buffer != WG_NULL_ID) {
    add_buffer(WG_POINT_INDEX_BINDING, call.index_buffer, 0);
  }

  WGBindingResource resource = {};
  resource.group = 0;
  resource.binding = WG_POINT_DRAW_DATA_BINDING;
  resource.kind = WGBindingLayout::Kind::UniformBuffer;
  resource.object = WG_STREAM_DATA_ID;
  resource.offset = stream.staging_push(&data, sizeof(data));
  resource.size = sizeof(data);
  pending.resources.append(resource);

  cmd.vertex_buffers_len = 0;
  cmd.index_buffer = WG_NULL_ID;
  cmd.first = 0;
  cmd.count = call.count * 6;
  cmd.base_vertex = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw and Dispatch
 * \{ */

void WGContext::draw(const WGDrawCall &call)
{
  WGShader *wg_shader = static_cast<WGShader *>(shader);
  if (wg_shader == nullptr || !wg_shader->is_valid() || wg_shader->is_compute()) {
    return;
  }
  if (call.indirect_buffer == WG_NULL_ID && (call.count == 0 || call.instance_count == 0)) {
    return;
  }
  WGPUPrimitiveTopology topology;
  if (!wg_topology(call.prim_type, topology)) {
    return;
  }
  WGFrameBuffer *framebuffer = active_framebuffer_get();
  if (framebuffer == nullptr) {
    return;
  }
  const uint32_t pass_id = framebuffer->render_pass_ensure();
  if (pass_id == 0) {
    return;
  }

  /* Pipeline state, zero initialized as keys are compared as bytes. */
  WGRenderPipelineKey key;
  memset(&key, 0, sizeof(key));
  const GPUState &state = state_manager->state;
  GPUState pipeline_state = {};
  pipeline_state.write_mask = state.write_mask;
  pipeline_state.blend = state.blend;
  pipeline_state.culling_test = state.culling_test;
  pipeline_state.depth_test = state.depth_test;
  pipeline_state.stencil_test = state.stencil_test;
  pipeline_state.stencil_op = state.stencil_op;
  pipeline_state.invert_facing = state.invert_facing;
  key.state = pipeline_state.data;
  key.topology = topology;
  key.strip_index_format = (call.index_buffer != WG_NULL_ID &&
                            ELEM(topology,
                                 WGPUPrimitiveTopology_LineStrip,
                                 WGPUPrimitiveTopology_TriangleStrip)) ?
                               call.index_format :
                               WGPUIndexFormat_Undefined;
  framebuffer->pipeline_formats_get(key);
  if (key.has_stencil && state.stencil_test != GPU_STENCIL_NONE) {
    key.stencil_write_mask = state_manager->mutable_state.stencil_write_mask;
    key.stencil_compare_mask = state_manager->mutable_state.stencil_compare_mask;
  }
  key.sample_count = 1;
  key.vertex_layout_hash = call.vertex_layout.hash();
  key.constants_hash = wg_constants_hash(constants_ ? constants_ : wg_shader->constants.get());

  /* WebGPU points are one pixel, draw them as quads when the shader sets their size.
   * TODO: Indirect draws, the vertex count would need to be multiplied on the GPU. */
  const WGPointVariant *point_variant = nullptr;
  if (call.prim_type == GPU_PRIM_POINTS && call.indirect_buffer == WG_NULL_ID &&
      wg_shader->has_point_size())
  {
    point_variant = wg_shader->point_variant_get(
        call.vertex_layout,
        call.index_buffer != WG_NULL_ID ? call.index_format : WGPUIndexFormat_Undefined);
  }
  if (point_variant) {
    key.topology = WGPUPrimitiveTopology_TriangleList;
    key.strip_index_format = WGPUIndexFormat_Undefined;
    key.point_variant_hash = point_variant->hash;
  }
  const WGObjectID pipeline = wg_shader->render_pipeline_get(
      key, call.vertex_layout, constants_, point_variant);

  WGCmdDraw cmd = {};
  cmd.pass_id = pass_id;
  cmd.pipeline = pipeline;
  cmd.pipeline_layout = point_variant ? point_variant->pipeline_layout :
                                        wg_shader->pipeline_layout_get();
  cmd.vertex_buffers_len = uint32_t(min_ii(call.vertex_buffers.size(), WG_MAX_VERTEX_BUFFERS));
  cmd.index_buffer = call.index_buffer;
  cmd.index_format = call.index_format;
  cmd.indirect_buffer = call.indirect_buffer;
  cmd.indirect_offset = call.indirect_offset;
  cmd.first = call.first;
  cmd.count = call.count;
  cmd.first_instance = call.first_instance;
  cmd.instance_count = call.instance_count;
  cmd.base_vertex = call.base_vertex;
  framebuffer->viewport_get_wg(cmd.viewport);
  cmd.depth_range[0] = 0.0f;
  cmd.depth_range[1] = 1.0f;
  framebuffer->scissor_get_wg(cmd.scissor);
  cmd.stencil_reference = state_manager->mutable_state.stencil_reference;

  WGPendingResources pending;
  wg_resources_get(*this, *wg_shader, pending);

  WGRecord record;
  wg_resources_resolve_stream_data(*wg_shader, record.stream, wg_recorder().generation, pending);
  uint64_t stream_vertex_offset = 0;
  if (!call.stream_vertex_data.is_empty()) {
    stream_vertex_offset = record.stream.staging_push(call.stream_vertex_data.data(),
                                                      call.stream_vertex_data.size());
  }
  for (uint32_t i = 0; i < cmd.vertex_buffers_len; i++) {
    cmd.vertex_buffers[i] = call.vertex_buffers[i];
    if (cmd.vertex_buffers[i].buffer == WG_STREAM_DATA_ID) {
      cmd.vertex_buffers[i].offset += stream_vertex_offset;
    }
  }
  if (point_variant) {
    wg_point_variant_resources(call, record.stream, stream_vertex_offset, cmd, pending);
  }
  cmd.resources_len = uint32_t(pending.resources.size());
  record.stream.push(WGCommandType::Draw,
                     cmd,
                     pending.resources.data(),
                     pending.resources.size() * sizeof(WGBindingResource));
}

void WGContext::dispatch(const uint32_t groups[3], WGStorageBuf *indirect_buffer)
{
  WGShader *wg_shader = static_cast<WGShader *>(shader);
  if (wg_shader == nullptr || !wg_shader->is_valid() || !wg_shader->is_compute()) {
    return;
  }
  WGCmdDispatch cmd = {};
  cmd.pipeline = wg_shader->compute_pipeline_get(constants_);
  cmd.pipeline_layout = wg_shader->pipeline_layout_get();
  for (int i = 0; i < 3; i++) {
    cmd.groups[i] = groups[i];
  }
  if (indirect_buffer) {
    cmd.indirect_buffer = indirect_buffer->binding_ensure().buffer;
  }

  WGPendingResources pending;
  wg_resources_get(*this, *wg_shader, pending);

  WGRecord record;
  wg_resources_resolve_stream_data(*wg_shader, record.stream, wg_recorder().generation, pending);
  cmd.resources_len = uint32_t(pending.resources.size());
  record.stream.push(WGCommandType::Dispatch,
                     cmd,
                     pending.resources.data(),
                     pending.resources.size() * sizeof(WGBindingResource));
}

/** \} */

}  // namespace blender::gpu
