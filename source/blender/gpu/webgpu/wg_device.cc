/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include "BLI_assert.hh"
#include "BLI_vector.hh"

#include "wg_device.hh"

namespace blender::gpu {

static WGPUStringView wg_string(const char *str)
{
  return WGPUStringView{str, strlen(str)};
}

static std::string wg_string_get(const WGPUStringView view)
{
  return view.data ? std::string(view.data, view.length == WGPU_STRLEN ? strlen(view.data) :
                                                                          view.length) :
                     std::string();
}

/* -------------------------------------------------------------------- */
/** \name Initialization
 * \{ */

static void wg_on_uncaptured_error(WGPUDevice const * /*device*/,
                                   WGPUErrorType type,
                                   WGPUStringView message,
                                   void * /*userdata1*/,
                                   void * /*userdata2*/)
{
  fprintf(stderr, "WebGPU error (%d): %s\n", int(type), wg_string_get(message).c_str());
}

static void wg_on_device_lost(WGPUDevice const * /*device*/,
                              WGPUDeviceLostReason reason,
                              WGPUStringView message,
                              void * /*userdata1*/,
                              void * /*userdata2*/)
{
  if (reason != WGPUDeviceLostReason_Destroyed) {
    fprintf(stderr, "WebGPU device lost (%d): %s\n", int(reason), wg_string_get(message).c_str());
  }
}

static void wg_device_init_main_thread(em_proxying_ctx *ctx, void *arg)
{
  static_cast<WGDevice *>(arg)->init_on_main_thread(ctx);
}

bool WGDevice::init()
{
  if (initialized_) {
    return true;
  }
  /* WebGPU objects can only be used on the browser's main thread, see #WGCommandStream. */
  emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                 emscripten_main_runtime_thread_id(),
                                 wg_device_init_main_thread,
                                 this);
  return initialized_;
}

void WGDevice::init_on_main_thread(void *ctx)
{
  instance_ = wgpuCreateInstance(nullptr);
  if (instance_ == nullptr) {
    fprintf(stderr, "WebGPU is not available in this browser\n");
    emscripten_proxy_finish(static_cast<em_proxying_ctx *>(ctx));
    return;
  }
  WGPURequestAdapterOptions options = {};
  options.powerPreference = WGPUPowerPreference_HighPerformance;
  WGPURequestAdapterCallbackInfo callback = {};
  callback.mode = WGPUCallbackMode_AllowSpontaneous;
  callback.callback = [](WGPURequestAdapterStatus status,
                         WGPUAdapter adapter,
                         WGPUStringView message,
                         void *userdata1,
                         void *userdata2) {
    WGDevice *device = static_cast<WGDevice *>(userdata1);
    if (status != WGPURequestAdapterStatus_Success) {
      fprintf(stderr, "WebGPU adapter not available: %s\n", wg_string_get(message).c_str());
      emscripten_proxy_finish(static_cast<em_proxying_ctx *>(userdata2));
      return;
    }
    device->on_adapter(adapter, userdata2);
  };
  callback.userdata1 = this;
  callback.userdata2 = ctx;
  wgpuInstanceRequestAdapter(instance_, &options, callback);
}

void WGDevice::on_adapter(WGPUAdapter adapter, void *ctx)
{
  adapter_ = adapter;

  WGPUAdapterInfo adapter_info = {};
  wgpuAdapterGetInfo(adapter_, &adapter_info);
  info_.adapter_vendor = wg_string_get(adapter_info.vendor);
  info_.adapter_architecture = wg_string_get(adapter_info.architecture);
  info_.adapter_description = wg_string_get(adapter_info.description);
  wgpuAdapterInfoFreeMembers(adapter_info);

  /* Request the adapter's limits instead of the (lower) defaults. */
  WGPULimits limits = {};
  wgpuAdapterGetLimits(adapter_, &limits);

  Vector<WGPUFeatureName> features;
  auto request_feature = [&](const WGPUFeatureName feature) {
    if (wgpuAdapterHasFeature(adapter_, feature)) {
      features.append(feature);
      return true;
    }
    return false;
  };
  info_.float32_filterable = request_feature(WGPUFeatureName_Float32Filterable);
  info_.depth32float_stencil8 = request_feature(WGPUFeatureName_Depth32FloatStencil8);
  info_.dual_source_blending = request_feature(WGPUFeatureName_DualSourceBlending);
  info_.rg11b10ufloat_renderable = request_feature(WGPUFeatureName_RG11B10UfloatRenderable);
  request_feature(WGPUFeatureName_TextureCompressionBC);
  request_feature(WGPUFeatureName_IndirectFirstInstance);

  WGPUDeviceDescriptor desc = {};
  desc.label = wg_string("Blender");
  desc.requiredLimits = &limits;
  desc.requiredFeatureCount = features.size();
  desc.requiredFeatures = features.data();
  desc.uncapturedErrorCallbackInfo.callback = wg_on_uncaptured_error;
  desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
  desc.deviceLostCallbackInfo.callback = wg_on_device_lost;

  WGPURequestDeviceCallbackInfo callback = {};
  callback.mode = WGPUCallbackMode_AllowSpontaneous;
  callback.callback = [](WGPURequestDeviceStatus status,
                         WGPUDevice device,
                         WGPUStringView message,
                         void *userdata1,
                         void *userdata2) {
    WGDevice *wg_device = static_cast<WGDevice *>(userdata1);
    if (status != WGPURequestDeviceStatus_Success) {
      fprintf(stderr, "WebGPU device not available: %s\n", wg_string_get(message).c_str());
      emscripten_proxy_finish(static_cast<em_proxying_ctx *>(userdata2));
      return;
    }
    wg_device->on_device(device, userdata2);
  };
  callback.userdata1 = this;
  callback.userdata2 = ctx;
  wgpuAdapterRequestDevice(adapter_, &desc, callback);
}

void WGDevice::on_device(WGPUDevice device, void *ctx)
{
  device_ = device;
  queue_ = wgpuDeviceGetQueue(device_);
  wgpuDeviceGetLimits(device_, &info_.limits);

  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
  canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
  canvas.selector = wg_string("#canvas");
  WGPUSurfaceDescriptor surface_desc = {};
  surface_desc.nextInChain = &canvas.chain;
  surface_ = wgpuInstanceCreateSurface(instance_, &surface_desc);
  if (surface_) {
    WGPUSurfaceCapabilities capabilities = {};
    wgpuSurfaceGetCapabilities(surface_, adapter_, &capabilities);
    if (capabilities.formatCount > 0) {
      surface_format_ = capabilities.formats[0];
    }
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
  }

  initialized_ = true;
  emscripten_proxy_finish(static_cast<em_proxying_ctx *>(ctx));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Submission
 * \{ */

struct WGSubmission {
  WGDevice *device;
  WGCommandStream stream;
};

static void wg_execute_async(void *arg)
{
  WGSubmission *submission = static_cast<WGSubmission *>(arg);
  submission->device->execute(submission->stream, nullptr);
  delete submission;
}

static void wg_execute_sync(em_proxying_ctx *ctx, void *arg)
{
  WGSubmission *submission = static_cast<WGSubmission *>(arg);
  submission->device->execute(submission->stream, ctx);
}

void WGDevice::submit(WGCommandStream &stream)
{
  if (stream.is_empty()) {
    return;
  }
  /* Don't let Blender get too far ahead of the browser. */
  constexpr int max_streams_in_flight = 3;
  int in_flight = streams_in_flight_.load();
  while (in_flight >= max_streams_in_flight) {
    streams_in_flight_.wait(in_flight);
    in_flight = streams_in_flight_.load();
  }
  streams_in_flight_++;

  WGSubmission *submission = new WGSubmission{this, {}};
  std::swap(submission->stream, stream);
  emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                         emscripten_main_runtime_thread_id(),
                         wg_execute_async,
                         submission);
}

void WGDevice::submit_and_wait(WGCommandStream &stream)
{
  streams_in_flight_++;
  WGSubmission submission{this, {}};
  std::swap(submission.stream, stream);
  /* The queue is ordered: previously submitted streams are executed before this one. */
  emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                 emscripten_main_runtime_thread_id(),
                                 wg_execute_sync,
                                 &submission);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Execution (Main Thread)
 * \{ */

struct WGPendingRead {
  WGPUBuffer buffer;
  uint64_t size;
  void *destination;
  /* Textures: rows are padded in `buffer`. */
  uint32_t rows = 0;
  uint32_t row_size = 0;
  uint32_t padded_row_size = 0;
};

struct WGDevice::ExecuteState {
  WGPUCommandEncoder encoder = nullptr;
  WGPURenderPassEncoder render_pass = nullptr;
  /** The pass `render_pass` encodes, 0 when none. */
  uint32_t render_pass_id = 0;
  WGPUBuffer staging = nullptr;
  WGPUTexture surface_texture = nullptr;
  Vector<WGPUTextureView> views;
  Vector<WGPUBindGroup> bind_groups;
  Vector<WGPUBuffer> temporary_buffers;
  Vector<WGPendingRead> reads;
};

WGPUTexture WGDevice::texture_get(const WGObjectID texture, ExecuteState &state)
{
  if (texture == WG_SURFACE_TEXTURE_ID) {
    if (state.surface_texture == nullptr && surface_) {
      int width = 0, height = 0;
      emscripten_get_canvas_element_size("#canvas", &width, &height);
      if (uint32_t(width) != surface_width_ || uint32_t(height) != surface_height_) {
        surface_width_ = width;
        surface_height_ = height;
        WGPUSurfaceConfiguration config = {};
        config.device = device_;
        config.format = surface_format_;
        config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc |
                       WGPUTextureUsage_CopyDst;
        config.width = width;
        config.height = height;
        config.alphaMode = WGPUCompositeAlphaMode_Opaque;
        config.presentMode = WGPUPresentMode_Fifo;
        wgpuSurfaceConfigure(surface_, &config);
      }
      WGPUSurfaceTexture surface_texture = {};
      wgpuSurfaceGetCurrentTexture(surface_, &surface_texture);
      state.surface_texture = surface_texture.texture;
    }
    return state.surface_texture;
  }
  const TextureEntry *entry = textures_.lookup_ptr(texture);
  return entry ? entry->texture : nullptr;
}

WGPUBuffer WGDevice::buffer_get(const WGObjectID buffer, const ExecuteState &state)
{
  if (buffer == WG_STREAM_DATA_ID) {
    return state.staging;
  }
  return buffers_.lookup_default(buffer, nullptr);
}

WGPUTextureView WGDevice::texture_view_get(const WGObjectID texture,
                                           WGPUTextureViewDimension dimension,
                                           WGPUTextureFormat format,
                                           WGPUTextureAspect aspect,
                                           uint32_t base_mip,
                                           uint32_t mip_count,
                                           uint32_t base_layer,
                                           uint32_t layer_count,
                                           ExecuteState &state)
{
  WGPUTexture wgpu_texture = texture_get(texture, state);
  if (wgpu_texture == nullptr) {
    return nullptr;
  }
  WGPUTextureViewDescriptor desc = {};
  desc.format = format;
  desc.dimension = dimension;
  desc.baseMipLevel = base_mip;
  desc.mipLevelCount = mip_count ? mip_count : WGPU_MIP_LEVEL_COUNT_UNDEFINED;
  desc.baseArrayLayer = base_layer;
  desc.arrayLayerCount = layer_count ? layer_count : WGPU_ARRAY_LAYER_COUNT_UNDEFINED;
  desc.aspect = aspect;
  WGPUTextureView view = wgpuTextureCreateView(wgpu_texture, &desc);
  state.views.append(view);
  return view;
}

WGPUBindGroup WGDevice::bind_group_create(WGObjectID pipeline_layout,
                                          uint32_t group,
                                          const WGBindingResource *resources,
                                          uint32_t resources_len,
                                          ExecuteState &state)
{
  const PipelineLayoutEntry *layout = pipeline_layouts_.lookup_ptr(pipeline_layout);
  if (layout == nullptr || layout->groups[group] == nullptr) {
    return nullptr;
  }
  Vector<WGPUBindGroupEntry, 16> entries;
  for (const WGBindingResource &resource : Span(resources, resources_len)) {
    if (resource.group != group) {
      continue;
    }
    WGPUBindGroupEntry entry = {};
    entry.binding = resource.binding;
    switch (resource.kind) {
      case WGBindingLayout::Kind::UniformBuffer:
      case WGBindingLayout::Kind::StorageBuffer:
      case WGBindingLayout::Kind::ReadOnlyStorageBuffer:
        entry.buffer = buffer_get(resource.object, state);
        entry.offset = resource.offset;
        entry.size = resource.size ? resource.size : WGPU_WHOLE_SIZE;
        break;
      case WGBindingLayout::Kind::Texture:
      case WGBindingLayout::Kind::StorageTexture:
        entry.textureView = texture_view_get(resource.object,
                                             resource.view_dimension,
                                             resource.view_format,
                                             resource.aspect,
                                             resource.base_mip,
                                             resource.mip_count,
                                             resource.base_layer,
                                             resource.layer_count,
                                             state);
        break;
      case WGBindingLayout::Kind::Sampler:
      case WGBindingLayout::Kind::NonFilteringSampler:
      case WGBindingLayout::Kind::ComparisonSampler:
        entry.sampler = samplers_.lookup_default(resource.object, nullptr);
        break;
    }
    entries.append(entry);
  }
  WGPUBindGroupDescriptor desc = {};
  desc.layout = layout->groups[group];
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device_, &desc);
  state.bind_groups.append(bind_group);
  return bind_group;
}

static void wg_end_render_pass(WGPURenderPassEncoder &render_pass, uint32_t &render_pass_id)
{
  if (render_pass) {
    wgpuRenderPassEncoderEnd(render_pass);
    wgpuRenderPassEncoderRelease(render_pass);
    render_pass = nullptr;
  }
  render_pass_id = 0;
}

static bool wg_render_pass_has_clear(const WGCmdBeginRenderPass &pass)
{
  bool has_clear = pass.depth.load_op == WGPULoadOp_Clear ||
                   pass.stencil_load_op == WGPULoadOp_Clear;
  for (uint32_t i = 0; i < pass.color_len; i++) {
    has_clear |= pass.color[i].load_op == WGPULoadOp_Clear;
  }
  return has_clear;
}

bool WGDevice::render_pass_activate(const uint32_t pass_id, ExecuteState &state)
{
  if (state.render_pass && state.render_pass_id == pass_id) {
    return true;
  }
  WGCmdBeginRenderPass *cmd = render_passes_.lookup_ptr(pass_id);
  if (cmd == nullptr) {
    return false;
  }
  wg_end_render_pass(state.render_pass, state.render_pass_id);

  WGPURenderPassColorAttachment colors[WG_MAX_COLOR_TARGETS] = {};
  for (uint32_t i = 0; i < cmd->color_len; i++) {
    const WGAttachment &attachment = cmd->color[i];
    WGPURenderPassColorAttachment &color = colors[i];
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    if (attachment.texture == WG_NULL_ID) {
      continue;
    }
    color.view = texture_view_get(attachment.texture,
                                  WGPUTextureViewDimension_2D,
                                  attachment.view_format,
                                  WGPUTextureAspect_All,
                                  attachment.mip,
                                  1,
                                  attachment.layer,
                                  1,
                                  state);
    color.loadOp = attachment.load_op;
    color.storeOp = attachment.store_op;
    color.clearValue = {attachment.clear_color[0],
                        attachment.clear_color[1],
                        attachment.clear_color[2],
                        attachment.clear_color[3]};
  }
  WGPURenderPassDepthStencilAttachment depth = {};
  if (cmd->depth.texture != WG_NULL_ID) {
    depth.view = texture_view_get(cmd->depth.texture,
                                  WGPUTextureViewDimension_2D,
                                  cmd->depth.view_format,
                                  WGPUTextureAspect_All,
                                  cmd->depth.mip,
                                  1,
                                  cmd->depth.layer,
                                  1,
                                  state);
    depth.depthLoadOp = cmd->depth.load_op;
    depth.depthStoreOp = cmd->depth.store_op;
    depth.depthClearValue = cmd->clear_depth;
    if (cmd->has_stencil) {
      depth.stencilLoadOp = cmd->stencil_load_op;
      depth.stencilStoreOp = cmd->stencil_store_op;
      depth.stencilClearValue = cmd->clear_stencil;
    }
  }

  WGPURenderPassDescriptor desc = {};
  desc.colorAttachmentCount = cmd->color_len;
  desc.colorAttachments = colors;
  desc.depthStencilAttachment = (cmd->depth.texture != WG_NULL_ID) ? &depth : nullptr;
  state.render_pass = wgpuCommandEncoderBeginRenderPass(state.encoder, &desc);
  state.render_pass_id = pass_id;

  /* Clears only happen once, resuming the pass later loads the attachments. */
  for (uint32_t i = 0; i < cmd->color_len; i++) {
    cmd->color[i].load_op = WGPULoadOp_Load;
  }
  cmd->depth.load_op = WGPULoadOp_Load;
  cmd->stencil_load_op = WGPULoadOp_Load;
  return true;
}

/** End the render pass for commands that can't be inside one (copies, compute). */
static void wg_interrupt_render_pass(WGPURenderPassEncoder &render_pass, uint32_t &render_pass_id)
{
  wg_end_render_pass(render_pass, render_pass_id);
}

void WGDevice::execute_draw(const WGCmdDraw &cmd, const uint8_t *trailing, ExecuteState &state)
{
  if (!render_pass_activate(cmd.pass_id, state)) {
    return;
  }
  WGPURenderPipeline pipeline = render_pipelines_.lookup_default(cmd.pipeline, nullptr);
  if (pipeline == nullptr) {
    return;
  }
  WGPURenderPassEncoder pass = state.render_pass;
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);

  const WGBindingResource *resources = reinterpret_cast<const WGBindingResource *>(trailing);
  for (uint32_t group = 0; group < 2; group++) {
    WGPUBindGroup bind_group = bind_group_create(
        cmd.pipeline_layout, group, resources, cmd.resources_len, state);
    if (bind_group) {
      wgpuRenderPassEncoderSetBindGroup(pass, group, bind_group, 0, nullptr);
    }
  }
  for (uint32_t i = 0; i < cmd.vertex_buffers_len; i++) {
    WGPUBuffer buffer = buffer_get(cmd.vertex_buffers[i].buffer, state);
    if (buffer) {
      wgpuRenderPassEncoderSetVertexBuffer(
          pass, i, buffer, cmd.vertex_buffers[i].offset, WGPU_WHOLE_SIZE);
    }
  }
  wgpuRenderPassEncoderSetViewport(pass,
                                   cmd.viewport[0],
                                   cmd.viewport[1],
                                   cmd.viewport[2],
                                   cmd.viewport[3],
                                   cmd.depth_range[0],
                                   cmd.depth_range[1]);
  wgpuRenderPassEncoderSetScissorRect(
      pass, cmd.scissor[0], cmd.scissor[1], cmd.scissor[2], cmd.scissor[3]);
  wgpuRenderPassEncoderSetStencilReference(pass, cmd.stencil_reference);
  const WGPUColor blend_constant = {
      cmd.blend_constant[0], cmd.blend_constant[1], cmd.blend_constant[2], cmd.blend_constant[3]};
  wgpuRenderPassEncoderSetBlendConstant(pass, &blend_constant);

  WGPUBuffer indirect = (cmd.indirect_buffer != WG_NULL_ID) ?
                            buffer_get(cmd.indirect_buffer, state) :
                            nullptr;
  if (cmd.index_buffer != WG_NULL_ID) {
    WGPUBuffer index_buffer = buffer_get(cmd.index_buffer, state);
    if (index_buffer == nullptr) {
      return;
    }
    wgpuRenderPassEncoderSetIndexBuffer(
        pass, index_buffer, cmd.index_format, cmd.index_offset, WGPU_WHOLE_SIZE);
    if (indirect) {
      wgpuRenderPassEncoderDrawIndexedIndirect(pass, indirect, cmd.indirect_offset);
    }
    else {
      wgpuRenderPassEncoderDrawIndexed(
          pass, cmd.count, cmd.instance_count, cmd.first, cmd.base_vertex, cmd.first_instance);
    }
  }
  else if (indirect) {
    wgpuRenderPassEncoderDrawIndirect(pass, indirect, cmd.indirect_offset);
  }
  else {
    wgpuRenderPassEncoderDraw(pass, cmd.count, cmd.instance_count, cmd.first, cmd.first_instance);
  }
}

void WGDevice::execute_dispatch(const WGCmdDispatch &cmd,
                                const uint8_t *trailing,
                                ExecuteState &state)
{
  WGPUComputePipeline pipeline = compute_pipelines_.lookup_default(cmd.pipeline, nullptr);
  if (pipeline == nullptr) {
    return;
  }
  wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(state.encoder, nullptr);
  wgpuComputePassEncoderSetPipeline(pass, pipeline);
  const WGBindingResource *resources = reinterpret_cast<const WGBindingResource *>(trailing);
  for (uint32_t group = 0; group < 2; group++) {
    WGPUBindGroup bind_group = bind_group_create(
        cmd.pipeline_layout, group, resources, cmd.resources_len, state);
    if (bind_group) {
      wgpuComputePassEncoderSetBindGroup(pass, group, bind_group, 0, nullptr);
    }
  }
  WGPUBuffer indirect = (cmd.indirect_buffer != WG_NULL_ID) ?
                            buffer_get(cmd.indirect_buffer, state) :
                            nullptr;
  if (indirect) {
    wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass, indirect, cmd.indirect_offset);
  }
  else {
    wgpuComputePassEncoderDispatchWorkgroups(pass, cmd.groups[0], cmd.groups[1], cmd.groups[2]);
  }
  wgpuComputePassEncoderEnd(pass);
  wgpuComputePassEncoderRelease(pass);
}

/** WebGPU constant entries of #WGPipelineConstant, keys are the override ids. */
struct WGPipelineConstants {
  Vector<std::string, 8> keys;
  Vector<WGPUConstantEntry, 8> entries;

  WGPipelineConstants(const WGPipelineConstant *constants, const uint32_t constants_len)
  {
    for (const WGPipelineConstant &constant : Span(constants, constants_len)) {
      keys.append(std::to_string(constant.id));
    }
    for (const int i : IndexRange(constants_len)) {
      WGPUConstantEntry entry = {};
      entry.key = {keys[i].c_str(), keys[i].size()};
      entry.value = constants[i].value;
      entries.append(entry);
    }
  }
};

void WGDevice::execute_create_render_pipeline(const WGCmdCreateRenderPipeline &cmd,
                                              const uint8_t *trailing)
{
  const PipelineLayoutEntry *layout = pipeline_layouts_.lookup_ptr(cmd.layout);
  WGPUShaderModule vertex_module = shader_modules_.lookup_default(cmd.vertex_module, nullptr);
  WGPUShaderModule fragment_module = shader_modules_.lookup_default(cmd.fragment_module, nullptr);
  if (layout == nullptr || vertex_module == nullptr) {
    return;
  }

  const WGVertexBufferLayout *buffer_layouts = reinterpret_cast<const WGVertexBufferLayout *>(
      trailing);
  const WGVertexAttribute *attributes = reinterpret_cast<const WGVertexAttribute *>(
      trailing + sizeof(WGVertexBufferLayout) * cmd.vertex_buffers_len);
  const WGPipelineConstant *constants = reinterpret_cast<const WGPipelineConstant *>(
      trailing + sizeof(WGVertexBufferLayout) * cmd.vertex_buffers_len +
      sizeof(WGVertexAttribute) * cmd.attributes_len);
  WGPipelineConstants pipeline_constants(constants, cmd.constants_len);

  Vector<WGPUVertexAttribute, 16> wgpu_attributes;
  for (const WGVertexAttribute &attribute : Span(attributes, cmd.attributes_len)) {
    WGPUVertexAttribute wgpu_attribute = {};
    wgpu_attribute.format = attribute.format;
    wgpu_attribute.offset = attribute.offset;
    wgpu_attribute.shaderLocation = attribute.location;
    wgpu_attributes.append(wgpu_attribute);
  }
  Vector<WGPUVertexBufferLayout, WG_MAX_VERTEX_BUFFERS> wgpu_buffers;
  for (const WGVertexBufferLayout &buffer : Span(buffer_layouts, cmd.vertex_buffers_len)) {
    WGPUVertexBufferLayout wgpu_buffer = {};
    wgpu_buffer.stepMode = buffer.step_mode;
    wgpu_buffer.arrayStride = buffer.stride;
    wgpu_buffer.attributeCount = buffer.attributes_len;
    wgpu_buffer.attributes = wgpu_attributes.data() + buffer.attributes_start;
    wgpu_buffers.append(wgpu_buffer);
  }

  WGPURenderPipelineDescriptor desc = {};
  desc.layout = layout->layout;
  desc.vertex.module = vertex_module;
  desc.vertex.entryPoint = wg_string("main");
  desc.vertex.constantCount = pipeline_constants.entries.size();
  desc.vertex.constants = pipeline_constants.entries.data();
  desc.vertex.bufferCount = wgpu_buffers.size();
  desc.vertex.buffers = wgpu_buffers.data();
  desc.primitive.topology = cmd.topology;
  desc.primitive.stripIndexFormat = cmd.strip_index_format;
  desc.primitive.frontFace = cmd.front_face;
  desc.primitive.cullMode = cmd.cull_mode;
  desc.multisample.count = cmd.sample_count;
  desc.multisample.mask = 0xFFFFFFFFu;

  WGPUDepthStencilState depth_stencil = {};
  if (cmd.depth_stencil_format != WGPUTextureFormat_Undefined) {
    depth_stencil.format = cmd.depth_stencil_format;
    depth_stencil.depthWriteEnabled = cmd.depth_write ? WGPUOptionalBool_True :
                                                        WGPUOptionalBool_False;
    depth_stencil.depthCompare = cmd.depth_compare;
    depth_stencil.stencilFront = cmd.stencil_front;
    depth_stencil.stencilBack = cmd.stencil_back;
    depth_stencil.stencilReadMask = cmd.stencil_read_mask;
    depth_stencil.stencilWriteMask = cmd.stencil_write_mask;
    depth_stencil.depthBias = cmd.depth_bias;
    depth_stencil.depthBiasSlopeScale = cmd.depth_bias_slope_scale;
    desc.depthStencil = &depth_stencil;
  }

  WGPUColorTargetState targets[WG_MAX_COLOR_TARGETS] = {};
  WGPUBlendState blends[WG_MAX_COLOR_TARGETS] = {};
  WGPUFragmentState fragment = {};
  if (fragment_module) {
    for (uint32_t i = 0; i < cmd.color_targets_len; i++) {
      const WGColorTarget &target = cmd.color_targets[i];
      targets[i].format = target.format;
      targets[i].writeMask = target.write_mask;
      if (target.blend_enabled) {
        blends[i].color = target.blend_color;
        blends[i].alpha = target.blend_alpha;
        targets[i].blend = &blends[i];
      }
    }
    fragment.module = fragment_module;
    fragment.entryPoint = wg_string("main");
    fragment.constantCount = pipeline_constants.entries.size();
    fragment.constants = pipeline_constants.entries.data();
    fragment.targetCount = cmd.color_targets_len;
    fragment.targets = targets;
    desc.fragment = &fragment;
  }
  render_pipelines_.add_overwrite(cmd.pipeline, wgpuDeviceCreateRenderPipeline(device_, &desc));
}

void WGDevice::destroy_object(WGObjectID id)
{
  if (WGPUBuffer buffer = buffers_.pop_default(id, nullptr)) {
    wgpuBufferDestroy(buffer);
    wgpuBufferRelease(buffer);
  }
  else if (std::optional<TextureEntry> texture = textures_.pop_try(id)) {
    wgpuTextureDestroy(texture->texture);
    wgpuTextureRelease(texture->texture);
  }
  else if (WGPUSampler sampler = samplers_.pop_default(id, nullptr)) {
    wgpuSamplerRelease(sampler);
  }
  else if (WGPUShaderModule module = shader_modules_.pop_default(id, nullptr)) {
    wgpuShaderModuleRelease(module);
  }
  else if (WGPUBindGroupLayout layout = bind_group_layouts_.pop_default(id, nullptr)) {
    wgpuBindGroupLayoutRelease(layout);
  }
  else if (std::optional<PipelineLayoutEntry> pipeline_layout = pipeline_layouts_.pop_try(id)) {
    wgpuPipelineLayoutRelease(pipeline_layout->layout);
  }
  else if (WGPURenderPipeline pipeline = render_pipelines_.pop_default(id, nullptr)) {
    wgpuRenderPipelineRelease(pipeline);
  }
  else if (WGPUComputePipeline pipeline = compute_pipelines_.pop_default(id, nullptr)) {
    wgpuComputePipelineRelease(pipeline);
  }
}

static WGPUBindGroupLayoutEntry wg_bind_group_layout_entry(const WGBindingLayout &binding)
{
  WGPUBindGroupLayoutEntry entry = {};
  entry.binding = binding.binding;
  entry.visibility = binding.visibility;
  switch (binding.kind) {
    case WGBindingLayout::Kind::UniformBuffer:
      entry.buffer.type = WGPUBufferBindingType_Uniform;
      break;
    case WGBindingLayout::Kind::StorageBuffer:
      entry.buffer.type = WGPUBufferBindingType_Storage;
      break;
    case WGBindingLayout::Kind::ReadOnlyStorageBuffer:
      entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
      break;
    case WGBindingLayout::Kind::Texture:
      entry.texture.sampleType = binding.sample_type;
      entry.texture.viewDimension = binding.view_dimension;
      break;
    case WGBindingLayout::Kind::StorageTexture:
      entry.storageTexture.access = binding.storage_access;
      entry.storageTexture.format = binding.storage_format;
      entry.storageTexture.viewDimension = binding.view_dimension;
      break;
    case WGBindingLayout::Kind::Sampler:
      entry.sampler.type = WGPUSamplerBindingType_Filtering;
      break;
    case WGBindingLayout::Kind::NonFilteringSampler:
      entry.sampler.type = WGPUSamplerBindingType_NonFiltering;
      break;
    case WGBindingLayout::Kind::ComparisonSampler:
      entry.sampler.type = WGPUSamplerBindingType_Comparison;
      break;
  }
  return entry;
}

static void wg_on_mapped(WGPUMapAsyncStatus status,
                         WGPUStringView message,
                         void *userdata1,
                         void *userdata2);

struct WGReadbackGroup {
  Vector<WGPendingRead> reads;
  int remaining;
  em_proxying_ctx *ctx;
};

static void wg_on_mapped(WGPUMapAsyncStatus status,
                         WGPUStringView message,
                         void *userdata1,
                         void *userdata2)
{
  WGReadbackGroup *group = static_cast<WGReadbackGroup *>(userdata1);
  WGPendingRead &read = *static_cast<WGPendingRead *>(userdata2);
  if (status == WGPUMapAsyncStatus_Success) {
    const uint8_t *data = static_cast<const uint8_t *>(
        wgpuBufferGetConstMappedRange(read.buffer, 0, read.size));
    if (read.rows == 0) {
      memcpy(read.destination, data, read.size);
    }
    else {
      uint8_t *dst = static_cast<uint8_t *>(read.destination);
      for (uint32_t row = 0; row < read.rows; row++) {
        memcpy(dst + size_t(row) * read.row_size,
               data + size_t(row) * read.padded_row_size,
               read.row_size);
      }
    }
    wgpuBufferUnmap(read.buffer);
  }
  else {
    fprintf(stderr, "WebGPU read-back failed: %s\n", wg_string_get(message).c_str());
  }
  wgpuBufferDestroy(read.buffer);
  wgpuBufferRelease(read.buffer);
  if (--group->remaining == 0) {
    emscripten_proxy_finish(group->ctx);
    delete group;
  }
}

void WGDevice::execute(WGCommandStream &stream, void *wait_ctx)
{
  ExecuteState state;
  state.encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);

  if (!stream.staging.is_empty()) {
    WGPUBufferDescriptor desc = {};
    desc.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_Uniform | WGPUBufferUsage_Vertex |
                 WGPUBufferUsage_Index | WGPUBufferUsage_Storage;
    desc.size = WGCommandStream::align_up(stream.staging.size(), 4);
    desc.mappedAtCreation = true;
    state.staging = wgpuDeviceCreateBuffer(device_, &desc);
    void *mapped = wgpuBufferGetMappedRange(state.staging, 0, desc.size);
    memcpy(mapped, stream.staging.data(), stream.staging.size());
    wgpuBufferUnmap(state.staging);
  }

  const uint8_t *ptr = stream.commands.data();
  const uint8_t *end = ptr + stream.commands.size();
  while (ptr < end) {
    WGCommandHeader header;
    memcpy(&header, ptr, sizeof(header));
    const uint8_t *data = ptr + sizeof(header);
    ptr += header.size;

#define CMD(type) (*reinterpret_cast<const type *>(data))
#define TRAILING(type) (data + sizeof(type))

    switch (header.type) {
      case WGCommandType::CreateBuffer: {
        const WGCmdCreateBuffer &cmd = CMD(WGCmdCreateBuffer);
        WGPUBufferDescriptor desc = {};
        desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_Uniform |
                     WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect |
                     WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
        desc.size = WGCommandStream::align_up(std::max<uint64_t>(cmd.size, 4), 4);
        buffers_.add_overwrite(cmd.buffer, wgpuDeviceCreateBuffer(device_, &desc));
        break;
      }
      case WGCommandType::CreateTexture: {
        const WGCmdCreateTexture &cmd = CMD(WGCmdCreateTexture);
        WGPUTextureDescriptor desc = {};
        desc.usage = cmd.usage;
        desc.dimension = cmd.dimension;
        desc.size = {cmd.width, cmd.height, cmd.depth_or_layers};
        desc.format = cmd.format;
        desc.mipLevelCount = cmd.mip_count;
        desc.sampleCount = cmd.sample_count;
        TextureEntry entry;
        entry.texture = wgpuDeviceCreateTexture(device_, &desc);
        entry.format = cmd.format;
        entry.dimension = cmd.dimension;
        entry.mip_count = cmd.mip_count;
        entry.layers = cmd.depth_or_layers;
        entry.width = cmd.width;
        entry.height = cmd.height;
        entry.sample_count = cmd.sample_count;
        textures_.add_overwrite(cmd.texture, entry);
        break;
      }
      case WGCommandType::CreateSampler: {
        const WGCmdCreateSampler &cmd = CMD(WGCmdCreateSampler);
        WGPUSamplerDescriptor desc = {};
        desc.addressModeU = cmd.address_u;
        desc.addressModeV = cmd.address_v;
        desc.addressModeW = cmd.address_w;
        desc.magFilter = cmd.mag_filter;
        desc.minFilter = cmd.min_filter;
        desc.mipmapFilter = cmd.mipmap_filter;
        desc.lodMinClamp = 0.0f;
        desc.lodMaxClamp = cmd.lod_max;
        desc.compare = cmd.compare;
        desc.maxAnisotropy = std::max<uint16_t>(cmd.max_anisotropy, 1);
        samplers_.add_overwrite(cmd.sampler, wgpuDeviceCreateSampler(device_, &desc));
        break;
      }
      case WGCommandType::CreateShaderModule: {
        const WGCmdCreateShaderModule &cmd = CMD(WGCmdCreateShaderModule);
        WGPUShaderSourceWGSL source = {};
        source.chain.sType = WGPUSType_ShaderSourceWGSL;
        source.code = wg_string(reinterpret_cast<const char *>(TRAILING(WGCmdCreateShaderModule)));
        WGPUShaderModuleDescriptor desc = {};
        desc.nextInChain = &source.chain;
        shader_modules_.add_overwrite(cmd.module, wgpuDeviceCreateShaderModule(device_, &desc));
        break;
      }
      case WGCommandType::CreateBindGroupLayout: {
        const WGCmdCreateBindGroupLayout &cmd = CMD(WGCmdCreateBindGroupLayout);
        const WGBindingLayout *bindings = reinterpret_cast<const WGBindingLayout *>(
            TRAILING(WGCmdCreateBindGroupLayout));
        Vector<WGPUBindGroupLayoutEntry, 16> entries;
        for (const WGBindingLayout &binding : Span(bindings, cmd.entries_len)) {
          entries.append(wg_bind_group_layout_entry(binding));
        }
        WGPUBindGroupLayoutDescriptor desc = {};
        desc.entryCount = entries.size();
        desc.entries = entries.data();
        bind_group_layouts_.add_overwrite(cmd.layout,
                                          wgpuDeviceCreateBindGroupLayout(device_, &desc));
        break;
      }
      case WGCommandType::CreatePipelineLayout: {
        const WGCmdCreatePipelineLayout &cmd = CMD(WGCmdCreatePipelineLayout);
        PipelineLayoutEntry entry = {};
        WGPUBindGroupLayout layouts[2];
        for (int i = 0; i < 2; i++) {
          entry.groups[i] = bind_group_layouts_.lookup_default(cmd.bind_group_layouts[i],
                                                               nullptr);
          layouts[i] = entry.groups[i];
        }
        WGPUPipelineLayoutDescriptor desc = {};
        desc.bindGroupLayoutCount = 2;
        desc.bindGroupLayouts = layouts;
        entry.layout = wgpuDeviceCreatePipelineLayout(device_, &desc);
        pipeline_layouts_.add_overwrite(cmd.layout, entry);
        break;
      }
      case WGCommandType::CreateRenderPipeline: {
        execute_create_render_pipeline(CMD(WGCmdCreateRenderPipeline),
                                       TRAILING(WGCmdCreateRenderPipeline));
        break;
      }
      case WGCommandType::CreateComputePipeline: {
        const WGCmdCreateComputePipeline &cmd = CMD(WGCmdCreateComputePipeline);
        const PipelineLayoutEntry *layout = pipeline_layouts_.lookup_ptr(cmd.layout);
        WGPUShaderModule module = shader_modules_.lookup_default(cmd.module, nullptr);
        if (layout && module) {
          WGPUComputePipelineDescriptor desc = {};
          desc.layout = layout->layout;
          desc.compute.module = module;
          desc.compute.entryPoint = wg_string("main");
          WGPipelineConstants pipeline_constants(
              reinterpret_cast<const WGPipelineConstant *>(TRAILING(WGCmdCreateComputePipeline)),
              cmd.constants_len);
          desc.compute.constantCount = pipeline_constants.entries.size();
          desc.compute.constants = pipeline_constants.entries.data();
          compute_pipelines_.add_overwrite(cmd.pipeline,
                                           wgpuDeviceCreateComputePipeline(device_, &desc));
        }
        break;
      }
      case WGCommandType::Destroy: {
        destroy_object(CMD(WGCmdDestroy).object);
        break;
      }
      case WGCommandType::CopyStagingToBuffer: {
        const WGCmdCopyStagingToBuffer &cmd = CMD(WGCmdCopyStagingToBuffer);
        WGPUBuffer buffer = buffers_.lookup_default(cmd.buffer, nullptr);
        if (buffer && state.staging) {
          wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
          wgpuCommandEncoderCopyBufferToBuffer(state.encoder,
                                               state.staging,
                                               cmd.staging_offset,
                                               buffer,
                                               cmd.buffer_offset,
                                               WGCommandStream::align_up(cmd.size, 4));
        }
        break;
      }
      case WGCommandType::CopyStagingToTexture: {
        const WGCmdCopyStagingToTexture &cmd = CMD(WGCmdCopyStagingToTexture);
        WGPUTexture texture = texture_get(cmd.texture, state);
        if (texture && state.staging) {
          wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
          WGPUTexelCopyBufferInfo src = {};
          src.buffer = state.staging;
          src.layout.offset = cmd.staging_offset;
          src.layout.bytesPerRow = cmd.bytes_per_row;
          src.layout.rowsPerImage = cmd.rows_per_image;
          WGPUTexelCopyTextureInfo dst = {};
          dst.texture = texture;
          dst.mipLevel = cmd.mip;
          dst.origin = {cmd.offset[0], cmd.offset[1], cmd.offset[2]};
          dst.aspect = cmd.aspect;
          const WGPUExtent3D extent = {cmd.extent[0], cmd.extent[1], cmd.extent[2]};
          wgpuCommandEncoderCopyBufferToTexture(state.encoder, &src, &dst, &extent);
        }
        break;
      }
      case WGCommandType::CopyBufferToBuffer: {
        const WGCmdCopyBufferToBuffer &cmd = CMD(WGCmdCopyBufferToBuffer);
        WGPUBuffer src = buffers_.lookup_default(cmd.src, nullptr);
        WGPUBuffer dst = buffers_.lookup_default(cmd.dst, nullptr);
        if (src && dst) {
          wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
          wgpuCommandEncoderCopyBufferToBuffer(
              state.encoder, src, cmd.src_offset, dst, cmd.dst_offset, cmd.size);
        }
        break;
      }
      case WGCommandType::CopyTextureToTexture: {
        const WGCmdCopyTextureToTexture &cmd = CMD(WGCmdCopyTextureToTexture);
        WGPUTexture src = texture_get(cmd.src, state);
        WGPUTexture dst = texture_get(cmd.dst, state);
        if (src && dst) {
          wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
          WGPUTexelCopyTextureInfo src_info = {};
          src_info.texture = src;
          src_info.mipLevel = cmd.src_mip;
          src_info.origin = {cmd.src_offset[0], cmd.src_offset[1], cmd.src_offset[2]};
          WGPUTexelCopyTextureInfo dst_info = {};
          dst_info.texture = dst;
          dst_info.mipLevel = cmd.dst_mip;
          dst_info.origin = {cmd.dst_offset[0], cmd.dst_offset[1], cmd.dst_offset[2]};
          const WGPUExtent3D extent = {cmd.extent[0], cmd.extent[1], cmd.extent[2]};
          wgpuCommandEncoderCopyTextureToTexture(state.encoder, &src_info, &dst_info, &extent);
        }
        break;
      }
      case WGCommandType::ClearBuffer: {
        const WGCmdClearBuffer &cmd = CMD(WGCmdClearBuffer);
        WGPUBuffer buffer = buffers_.lookup_default(cmd.buffer, nullptr);
        if (buffer) {
          wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
          wgpuCommandEncoderClearBuffer(state.encoder, buffer, cmd.offset, cmd.size);
        }
        break;
      }
      case WGCommandType::ReadBuffer: {
        const WGCmdReadBuffer &cmd = CMD(WGCmdReadBuffer);
        WGPUBuffer buffer = buffers_.lookup_default(cmd.buffer, nullptr);
        if (buffer == nullptr || wait_ctx == nullptr) {
          break;
        }
        wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
        WGPUBufferDescriptor desc = {};
        desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        desc.size = WGCommandStream::align_up(cmd.size, 4);
        WGPUBuffer read_buffer = wgpuDeviceCreateBuffer(device_, &desc);
        wgpuCommandEncoderCopyBufferToBuffer(
            state.encoder, buffer, cmd.offset, read_buffer, 0, desc.size);
        state.reads.append({read_buffer, cmd.size, cmd.destination});
        break;
      }
      case WGCommandType::ReadTexture: {
        const WGCmdReadTexture &cmd = CMD(WGCmdReadTexture);
        WGPUTexture texture = texture_get(cmd.texture, state);
        if (texture == nullptr || wait_ctx == nullptr) {
          break;
        }
        wg_interrupt_render_pass(state.render_pass, state.render_pass_id);
        const uint32_t row_size = cmd.extent[0] * cmd.texel_size;
        const uint32_t padded_row_size = uint32_t(WGCommandStream::align_up(row_size, 256));
        const uint32_t rows = cmd.extent[1] * cmd.extent[2];
        WGPUBufferDescriptor desc = {};
        desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        desc.size = uint64_t(padded_row_size) * rows;
        WGPUBuffer read_buffer = wgpuDeviceCreateBuffer(device_, &desc);
        WGPUTexelCopyTextureInfo src = {};
        src.texture = texture;
        src.mipLevel = cmd.mip;
        src.origin = {cmd.offset[0], cmd.offset[1], cmd.offset[2]};
        src.aspect = cmd.aspect;
        WGPUTexelCopyBufferInfo dst = {};
        dst.buffer = read_buffer;
        dst.layout.bytesPerRow = padded_row_size;
        dst.layout.rowsPerImage = cmd.extent[1];
        const WGPUExtent3D extent = {cmd.extent[0], cmd.extent[1], cmd.extent[2]};
        wgpuCommandEncoderCopyTextureToBuffer(state.encoder, &src, &dst, &extent);
        state.reads.append(
            {read_buffer, desc.size, cmd.destination, rows, row_size, padded_row_size});
        break;
      }
      case WGCommandType::BeginRenderPass: {
        const WGCmdBeginRenderPass &cmd = CMD(WGCmdBeginRenderPass);
        render_passes_.add_overwrite(cmd.pass_id, cmd);
        /* Clear right away, keeping the order with other commands using the attachments. */
        if (wg_render_pass_has_clear(cmd)) {
          render_pass_activate(cmd.pass_id, state);
        }
        break;
      }
      case WGCommandType::EndRenderPass: {
        const WGCmdEndRenderPass &cmd = CMD(WGCmdEndRenderPass);
        if (state.render_pass_id == cmd.pass_id) {
          wg_end_render_pass(state.render_pass, state.render_pass_id);
        }
        render_passes_.remove(cmd.pass_id);
        break;
      }
      case WGCommandType::Draw: {
        execute_draw(CMD(WGCmdDraw), TRAILING(WGCmdDraw), state);
        break;
      }
      case WGCommandType::Dispatch: {
        execute_dispatch(CMD(WGCmdDispatch), TRAILING(WGCmdDispatch), state);
        break;
      }
      case WGCommandType::ClearAttachments: {
        execute_clear_attachments(CMD(WGCmdClearAttachments), state);
        break;
      }
      case WGCommandType::Present: {
        execute_present(CMD(WGCmdPresent), state);
        break;
      }
    }
#undef CMD
#undef TRAILING
  }

  wg_end_render_pass(state.render_pass, state.render_pass_id);
  WGPUCommandBuffer command_buffer = wgpuCommandEncoderFinish(state.encoder, nullptr);
  wgpuQueueSubmit(queue_, 1, &command_buffer);
  wgpuCommandBufferRelease(command_buffer);
  wgpuCommandEncoderRelease(state.encoder);

  for (WGPUBindGroup bind_group : state.bind_groups) {
    wgpuBindGroupRelease(bind_group);
  }
  for (WGPUTextureView view : state.views) {
    wgpuTextureViewRelease(view);
  }
  if (state.surface_texture) {
    /* Presented by the browser at the end of this task. */
    wgpuTextureRelease(state.surface_texture);
  }
  if (state.staging) {
    wgpuBufferDestroy(state.staging);
    wgpuBufferRelease(state.staging);
  }

  streams_in_flight_--;
  streams_in_flight_.notify_all();

  if (wait_ctx == nullptr) {
    return;
  }
  em_proxying_ctx *ctx = static_cast<em_proxying_ctx *>(wait_ctx);
  if (state.reads.is_empty()) {
    emscripten_proxy_finish(ctx);
    return;
  }
  /* Finish the proxied call when all read-backs are mapped. */
  WGReadbackGroup *group = new WGReadbackGroup{std::move(state.reads), 0, ctx};
  group->remaining = group->reads.size();
  for (WGPendingRead &read : group->reads) {
    WGPUBufferMapCallbackInfo callback = {};
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = wg_on_mapped;
    callback.userdata1 = group;
    callback.userdata2 = &read;
    wgpuBufferMapAsync(read.buffer, WGPUMapMode_Read, 0, read.size, callback);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Draws
 * \{ */

WGPUShaderModule WGDevice::shader_module_create(const std::string &wgsl)
{
  WGPUShaderSourceWGSL source = {};
  source.chain.sType = WGPUSType_ShaderSourceWGSL;
  source.code = {wgsl.c_str(), wgsl.size()};
  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &source.chain;
  return wgpuDeviceCreateShaderModule(device_, &desc);
}

static const char *WG_FULLSCREEN_VERTEX_WGSL =
    "fn wg_fullscreen_position(i : u32) -> vec2<f32> {\n"
    "  return vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;\n"
    "}\n";

static const char *wg_wgsl_component_type(const WGPUTextureFormat format)
{
  switch (format) {
    case WGPUTextureFormat_R8Uint:
    case WGPUTextureFormat_R16Uint:
    case WGPUTextureFormat_RG8Uint:
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_RG16Uint:
    case WGPUTextureFormat_RGBA8Uint:
    case WGPUTextureFormat_RGB10A2Uint:
    case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RGBA16Uint:
    case WGPUTextureFormat_RGBA32Uint:
      return "u32";
    case WGPUTextureFormat_R8Sint:
    case WGPUTextureFormat_R16Sint:
    case WGPUTextureFormat_RG8Sint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RG16Sint:
    case WGPUTextureFormat_RGBA8Sint:
    case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RGBA16Sint:
    case WGPUTextureFormat_RGBA32Sint:
      return "i32";
    default:
      return "f32";
  }
}

void WGDevice::execute_clear_attachments(const WGCmdClearAttachments &cmd, ExecuteState &state)
{
  if (!render_pass_activate(cmd.pass_id, state)) {
    return;
  }
  const WGCmdBeginRenderPass &pass = render_passes_.lookup(cmd.pass_id);

  /* Size and sample count of the attachments. */
  const WGAttachment *first = pass.depth.texture != WG_NULL_ID ? &pass.depth : nullptr;
  for (uint32_t i = 0; i < pass.color_len && first == nullptr; i++) {
    if (pass.color[i].texture != WG_NULL_ID) {
      first = &pass.color[i];
    }
  }
  const TextureEntry *texture = first ? textures_.lookup_ptr(first->texture) : nullptr;
  if (texture == nullptr) {
    return;
  }
  const uint32_t width = std::max(texture->width >> first->mip, 1u);
  const uint32_t height = std::max(texture->height >> first->mip, 1u);

  const bool has_depth = pass.depth.texture != WG_NULL_ID;
  std::string key = std::to_string(cmd.color_mask) + ":" + std::to_string(cmd.clear_depth) +
                    std::to_string(cmd.clear_stencil) + std::to_string(pass.has_stencil) + ":" +
                    std::to_string(texture->sample_count) + ":" +
                    std::to_string(has_depth ? pass.depth.view_format : 0);
  for (uint32_t i = 0; i < pass.color_len; i++) {
    key += ":" + std::to_string(pass.color[i].texture ? pass.color[i].view_format : 0);
  }

  WGPURenderPipeline pipeline = clear_pipelines_.lookup_default(key, nullptr);
  if (pipeline == nullptr) {
    std::string wgsl = WG_FULLSCREEN_VERTEX_WGSL;
    wgsl += "struct Values { colors : array<vec4<u32>, 8>, depth : vec4<f32> };\n";
    wgsl += "@group(0) @binding(0) var<uniform> values : Values;\n";
    wgsl += "@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {\n";
    wgsl += "  return vec4<f32>(wg_fullscreen_position(i), values.depth.x, 1.0);\n";
    wgsl += "}\n";
    std::string outputs;
    std::string body;
    for (uint32_t i = 0; i < pass.color_len; i++) {
      if (pass.color[i].texture == WG_NULL_ID || !(cmd.color_mask & (1u << i))) {
        continue;
      }
      const std::string type = std::string("vec4<") +
                               wg_wgsl_component_type(pass.color[i].view_format) + ">";
      const std::string index = std::to_string(i);
      outputs += "  @location(" + index + ") c" + index + " : " + type + ",\n";
      body += "  o.c" + index + " = bitcast<" + type + ">(values.colors[" + index + "]);\n";
    }
    if (outputs.empty()) {
      wgsl += "@fragment fn fs() {\n  _ = values.colors[0];\n}\n";
    }
    else {
      wgsl += "struct Out {\n" + outputs + "};\n";
      wgsl += "@fragment fn fs() -> Out {\n  var o : Out;\n" + body + "  return o;\n}\n";
    }
    WGPUShaderModule module = shader_module_create(wgsl);

    WGPUColorTargetState targets[WG_MAX_COLOR_TARGETS] = {};
    for (uint32_t i = 0; i < pass.color_len; i++) {
      targets[i].format = pass.color[i].texture ? pass.color[i].view_format :
                                                  WGPUTextureFormat_Undefined;
      targets[i].writeMask = (pass.color[i].texture && (cmd.color_mask & (1u << i))) ?
                                 WGPUColorWriteMask_All :
                                 WGPUColorWriteMask_None;
    }
    WGPUFragmentState fragment = {};
    fragment.module = module;
    fragment.entryPoint = wg_string("fs");
    fragment.targetCount = pass.color_len;
    fragment.targets = targets;

    WGPUDepthStencilState depth_stencil = {};
    WGPURenderPipelineDescriptor desc = {};
    if (has_depth) {
      depth_stencil.format = pass.depth.view_format;
      depth_stencil.depthWriteEnabled = cmd.clear_depth ? WGPUOptionalBool_True :
                                                          WGPUOptionalBool_False;
      depth_stencil.depthCompare = WGPUCompareFunction_Always;
      WGPUStencilFaceState face = {};
      face.compare = WGPUCompareFunction_Always;
      face.failOp = WGPUStencilOperation_Keep;
      face.depthFailOp = WGPUStencilOperation_Keep;
      face.passOp = cmd.clear_stencil ? WGPUStencilOperation_Replace :
                                        WGPUStencilOperation_Keep;
      depth_stencil.stencilFront = face;
      depth_stencil.stencilBack = face;
      depth_stencil.stencilReadMask = 0xFF;
      depth_stencil.stencilWriteMask = (pass.has_stencil && cmd.clear_stencil) ? 0xFF : 0;
      desc.depthStencil = &depth_stencil;
    }
    desc.vertex.module = module;
    desc.vertex.entryPoint = wg_string("vs");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.multisample.count = texture->sample_count;
    desc.multisample.mask = 0xFFFFFFFFu;
    desc.fragment = &fragment;
    pipeline = wgpuDeviceCreateRenderPipeline(device_, &desc);
    wgpuShaderModuleRelease(module);
    clear_pipelines_.add(key, pipeline);
  }

  WGPUBindGroupEntry entry = {};
  entry.binding = 0;
  entry.buffer = state.staging;
  entry.offset = cmd.data_offset;
  entry.size = WG_CLEAR_DATA_SIZE;
  WGPUBindGroupDescriptor bind_group_desc = {};
  WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  bind_group_desc.layout = layout;
  bind_group_desc.entryCount = 1;
  bind_group_desc.entries = &entry;
  WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device_, &bind_group_desc);
  wgpuBindGroupLayoutRelease(layout);
  state.bind_groups.append(bind_group);

  const uint32_t x = std::min(cmd.scissor[0], width);
  const uint32_t y = std::min(cmd.scissor[1], height);
  WGPURenderPassEncoder render_pass = state.render_pass;
  wgpuRenderPassEncoderSetPipeline(render_pass, pipeline);
  wgpuRenderPassEncoderSetBindGroup(render_pass, 0, bind_group, 0, nullptr);
  wgpuRenderPassEncoderSetViewport(render_pass, 0, 0, width, height, 0.0f, 1.0f);
  wgpuRenderPassEncoderSetScissorRect(render_pass,
                                      x,
                                      y,
                                      std::min(cmd.scissor[2], width - x),
                                      std::min(cmd.scissor[3], height - y));
  wgpuRenderPassEncoderSetStencilReference(render_pass, cmd.stencil);
  wgpuRenderPassEncoderDraw(render_pass, 3, 1, 0, 0);
}

void WGDevice::execute_present(const WGCmdPresent &cmd, ExecuteState &state)
{
  const TextureEntry *source = textures_.lookup_ptr(cmd.texture);
  WGPUTexture surface = texture_get(WG_SURFACE_TEXTURE_ID, state);
  if (source == nullptr || surface == nullptr) {
    return;
  }
  wg_end_render_pass(state.render_pass, state.render_pass_id);

  if (present_pipeline_ == nullptr) {
    std::string wgsl = WG_FULLSCREEN_VERTEX_WGSL;
    wgsl += "@group(0) @binding(0) var source : texture_2d<f32>;\n";
    wgsl += "@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {\n";
    wgsl += "  return vec4<f32>(wg_fullscreen_position(i), 0.0, 1.0);\n";
    wgsl += "}\n";
    wgsl += "@fragment fn fs(@builtin(position) p : vec4<f32>) -> @location(0) vec4<f32> {\n";
    wgsl += "  let size = vec2<i32>(textureDimensions(source));\n";
    wgsl += "  let co = vec2<i32>(i32(p.x), size.y - 1 - i32(p.y));\n";
    wgsl += "  let color = textureLoad(source, clamp(co, vec2<i32>(0), size - 1), 0);\n";
    wgsl += "  return vec4<f32>(color.rgb, 1.0);\n";
    wgsl += "}\n";
    WGPUShaderModule module = shader_module_create(wgsl);
    WGPUColorTargetState target = {};
    target.format = surface_format_;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = {};
    fragment.module = module;
    fragment.entryPoint = wg_string("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor desc = {};
    desc.vertex.module = module;
    desc.vertex.entryPoint = wg_string("vs");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;
    desc.fragment = &fragment;
    present_pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &desc);
    wgpuShaderModuleRelease(module);
  }

  WGPUTextureView source_view = texture_view_get(cmd.texture,
                                                 WGPUTextureViewDimension_2D,
                                                 source->format,
                                                 WGPUTextureAspect_All,
                                                 0,
                                                 1,
                                                 0,
                                                 1,
                                                 state);
  WGPUBindGroupEntry entry = {};
  entry.binding = 0;
  entry.textureView = source_view;
  WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(present_pipeline_, 0);
  WGPUBindGroupDescriptor bind_group_desc = {};
  bind_group_desc.layout = layout;
  bind_group_desc.entryCount = 1;
  bind_group_desc.entries = &entry;
  WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device_, &bind_group_desc);
  wgpuBindGroupLayoutRelease(layout);
  state.bind_groups.append(bind_group);

  WGPUTextureView surface_view = wgpuTextureCreateView(surface, nullptr);
  state.views.append(surface_view);
  WGPURenderPassColorAttachment color = {};
  color.view = surface_view;
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  WGPURenderPassDescriptor pass_desc = {};
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &color;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(state.encoder, &pass_desc);
  wgpuRenderPassEncoderSetPipeline(pass, present_pipeline_);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

/** \} */

}  // namespace blender::gpu
