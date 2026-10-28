/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <emscripten/emscripten.h>

#include "BLI_math_base.hh"
#include "BLI_threads.hh"

#include "CLG_log.h"

#include "GPU_capabilities.hh"
#include "gpu_capabilities_private.hh"
#include "gpu_platform_private.hh"
#include "gpu_texture_pool_private.hh"

#include "web_shader_compiler.h"

#include "wg_backend.hh"
#include "wg_batch.hh"
#include "wg_buffer.hh"
#include "wg_common.hh"
#include "wg_context.hh"
#include "wg_framebuffer.hh"
#include "wg_misc.hh"
#include "wg_shader.hh"
#include "wg_texture.hh"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.webgpu"};

WGBackend::WGBackend()
{
  WSC_init();
  platform_init();
  if (!device.init()) {
    CLOG_ERROR(&LOG, "Failed to initialize the WebGPU device");
  }
  capabilities_init();
}

WGBackend::~WGBackend()
{
  GPG.clear();
  WSC_exit();
}

bool WGBackend::is_supported()
{
  return MAIN_THREAD_EM_ASM_INT({ return (typeof navigator !== 'undefined' && navigator.gpu) ? 1 : 0; }) != 0;
}

void WGBackend::platform_init()
{
  GPG.init(GPU_DEVICE_ANY,
           GPU_OS_UNIX,
           GPU_DRIVER_ANY,
           GPU_SUPPORT_LEVEL_SUPPORTED,
           GPU_BACKEND_WEBGPU,
           "WebGPU",
           "WebGPU",
           "",
           GPU_ARCHITECTURE_IMR);
}

static const char *wg_extension_get(int /*index*/)
{
  return "";
}

void WGBackend::capabilities_init()
{
  const WGPULimits &limits = device.info().limits;

  GCaps = {};
  GCaps.geometry_shader_support = false;
  GCaps.stencil_export_support = false;
  GCaps.vertex_pipeline_stores_and_atomics_support = false;
  GCaps.ray_query_support = false;
  GCaps.multi_viewport_support = false;
  GCaps.srgb_write_view_support = true;
  GCaps.hdr_viewport_support = false;

  GCaps.max_texture_size = int(min_uu(limits.maxTextureDimension2D, INT_MAX));
  GCaps.max_texture_3d_size = int(min_uu(limits.maxTextureDimension3D, INT_MAX));
  GCaps.max_buffer_texture_size = 0;
  GCaps.max_texture_layers = int(min_uu(limits.maxTextureArrayLayers, INT_MAX));
  GCaps.max_textures = int(min_uu(limits.maxSampledTexturesPerShaderStage, INT_MAX));
  GCaps.max_images = int(min_uu(limits.maxStorageTexturesPerShaderStage, INT_MAX));
  GCaps.max_work_group_count[0] = GCaps.max_work_group_count[1] =
      GCaps.max_work_group_count[2] = int(
          min_uu(limits.maxComputeWorkgroupsPerDimension, INT_MAX));
  GCaps.max_work_group_size[0] = int(limits.maxComputeWorkgroupSizeX);
  GCaps.max_work_group_size[1] = int(limits.maxComputeWorkgroupSizeY);
  GCaps.max_work_group_size[2] = int(limits.maxComputeWorkgroupSizeZ);
  GCaps.max_uniforms_vert = GCaps.max_uniforms_frag = int(
      min_uu(limits.maxUniformBuffersPerShaderStage, INT_MAX));
  GCaps.max_batch_indices = INT_MAX;
  GCaps.max_batch_vertices = INT_MAX;
  GCaps.max_vertex_attribs = int(min_uu(limits.maxVertexAttributes, 16));
  GCaps.max_varying_floats = int(min_uu(limits.maxInterStageShaderVariables * 4, INT_MAX));
  GCaps.max_shader_storage_buffer_bindings = GCaps.max_compute_shader_storage_blocks = int(
      min_uu(limits.maxStorageBuffersPerShaderStage, INT_MAX));
  GCaps.max_uniform_buffer_size = size_t(limits.maxUniformBufferBindingSize);
  GCaps.max_storage_buffer_size = size_t(limits.maxStorageBufferBindingSize);
  GCaps.storage_buffer_alignment = limits.minStorageBufferOffsetAlignment;

  GCaps.max_parallel_compilations = BLI_system_thread_count();
  GCaps.mem_stats_support = false;
  GCaps.extensions_len = 0;
  GCaps.extension_get = wg_extension_get;

  /* Normals in 10 bit formats can't be vertex attributes. */
  GCaps.use_hq_normals_workaround = true;
  GCaps.texture_pool_workaround = true;
  /* Vertex strides need to be a multiple of 4. */
  GCaps.minimum_per_vertex_stride = 4;
}

void WGBackend::init_resources()
{
  compiler_ = MEM_new<ShaderCompiler>(
      __func__, GPU_max_parallel_compilations(), GPUWorker::ContextType::Main);
}

void WGBackend::delete_resources()
{
  MEM_delete(compiler_);
  compiler_ = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name Shared Resources
 * \{ */

WGBackend::DummyTexture WGBackend::dummy_texture_get(const WGPUTextureViewDimension dimension,
                                                     const WGPUTextureSampleType sample_type,
                                                     const WGPUTextureFormat storage_format)
{
  const uint64_t key = (uint64_t(dimension) << 48) | (uint64_t(sample_type) << 32) |
                       uint64_t(storage_format);
  std::lock_guard lock(resources_mutex_);
  if (const DummyTexture *dummy = dummy_textures_.lookup_ptr(key)) {
    return *dummy;
  }
  DummyTexture dummy;
  WGCmdCreateTexture cmd = {};
  cmd.texture = dummy.texture = wg_object_id_alloc();
  cmd.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  if (storage_format != WGPUTextureFormat_Undefined) {
    dummy.format = storage_format;
    cmd.usage |= WGPUTextureUsage_StorageBinding;
  }
  else {
    switch (sample_type) {
      case WGPUTextureSampleType_Sint:
        dummy.format = WGPUTextureFormat_RGBA32Sint;
        break;
      case WGPUTextureSampleType_Uint:
        dummy.format = WGPUTextureFormat_RGBA32Uint;
        break;
      case WGPUTextureSampleType_Depth:
        dummy.format = WGPUTextureFormat_Depth32Float;
        break;
      case WGPUTextureSampleType_UnfilterableFloat:
        dummy.format = WGPUTextureFormat_RGBA32Float;
        break;
      default:
        dummy.format = WGPUTextureFormat_RGBA8Unorm;
        break;
    }
  }
  cmd.format = dummy.format;
  cmd.width = cmd.height = cmd.depth_or_layers = 1;
  cmd.mip_count = 1;
  cmd.sample_count = 1;
  switch (dimension) {
    case WGPUTextureViewDimension_1D:
      cmd.dimension = WGPUTextureDimension_1D;
      break;
    case WGPUTextureViewDimension_3D:
      cmd.dimension = WGPUTextureDimension_3D;
      break;
    case WGPUTextureViewDimension_Cube:
    case WGPUTextureViewDimension_CubeArray:
      cmd.dimension = WGPUTextureDimension_2D;
      cmd.depth_or_layers = 6;
      break;
    default:
      cmd.dimension = WGPUTextureDimension_2D;
      break;
  }
  {
    WGRecord record;
    record.stream.push(WGCommandType::CreateTexture, cmd);
  }
  dummy_textures_.add(key, dummy);
  return dummy;
}

WGObjectID WGBackend::dummy_buffer_get()
{
  std::lock_guard lock(resources_mutex_);
  if (dummy_buffer_ == WG_NULL_ID) {
    dummy_buffer_ = wg_object_id_alloc();
    WGRecord record;
    record.stream.push(WGCommandType::CreateBuffer,
                       WGCmdCreateBuffer{dummy_buffer_, WG_DUMMY_BUFFER_SIZE});
  }
  return dummy_buffer_;
}

static WGPUAddressMode wg_address_mode(const GPUSamplerExtendMode mode)
{
  switch (mode) {
    case GPU_SAMPLER_EXTEND_MODE_REPEAT:
      return WGPUAddressMode_Repeat;
    case GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT:
      return WGPUAddressMode_MirrorRepeat;
    default:
      /* WebGPU has no border color. */
      return WGPUAddressMode_ClampToEdge;
  }
}

WGObjectID WGBackend::sampler_get(GPUSamplerState state, const WGBindingLayout::Kind kind)
{
  uint64_t key = uint64_t(state.filtering) | (uint64_t(state.extend_x) << 8) |
                 (uint64_t(state.extend_yz) << 12) | (uint64_t(state.custom_type) << 16) |
                 (uint64_t(state.type) << 24) | (uint64_t(kind) << 32);
  std::lock_guard lock(resources_mutex_);
  if (const WGObjectID *sampler = samplers_.lookup_ptr(key)) {
    return *sampler;
  }

  WGCmdCreateSampler cmd = {};
  cmd.sampler = wg_object_id_alloc();
  cmd.address_u = wg_address_mode(state.extend_x);
  cmd.address_v = cmd.address_w = wg_address_mode(state.extend_yz);
  cmd.mag_filter = cmd.min_filter = WGPUFilterMode_Nearest;
  cmd.mipmap_filter = WGPUMipmapFilterMode_Nearest;
  cmd.compare = WGPUCompareFunction_Undefined;
  cmd.max_anisotropy = 1;
  cmd.lod_max = 0.0f;
  if (state.type == GPU_SAMPLER_STATE_TYPE_PARAMETERS) {
    if (state.filtering & GPU_SAMPLER_FILTERING_LINEAR) {
      cmd.mag_filter = cmd.min_filter = WGPUFilterMode_Linear;
    }
    if (state.filtering & GPU_SAMPLER_FILTERING_MIPMAP) {
      cmd.mipmap_filter = WGPUMipmapFilterMode_Linear;
      cmd.lod_max = 32.0f;
    }
    if ((state.filtering & GPU_SAMPLER_FILTERING_MIPMAP) &&
        (state.filtering & GPU_SAMPLER_FILTERING_LINEAR) &&
        (state.filtering & GPU_SAMPLER_FILTERING_ANISOTROPIC_MASK))
    {
      cmd.max_anisotropy = uint16_t(GPU_anisotropic_samples_get(state.filtering));
    }
  }
  else if (state.type == GPU_SAMPLER_STATE_TYPE_CUSTOM) {
    if (state.custom_type == GPU_SAMPLER_CUSTOM_ICON) {
      cmd.mag_filter = cmd.min_filter = WGPUFilterMode_Linear;
      cmd.lod_max = 1.0f;
    }
    else {
      cmd.mag_filter = cmd.min_filter = WGPUFilterMode_Linear;
    }
  }
  switch (kind) {
    case WGBindingLayout::Kind::NonFilteringSampler:
      cmd.mag_filter = cmd.min_filter = WGPUFilterMode_Nearest;
      cmd.mipmap_filter = WGPUMipmapFilterMode_Nearest;
      cmd.max_anisotropy = 1;
      break;
    case WGBindingLayout::Kind::ComparisonSampler:
      cmd.compare = WGPUCompareFunction_LessEqual;
      break;
    default:
      break;
  }
  {
    WGRecord record;
    record.stream.push(WGCommandType::CreateSampler, cmd);
  }
  samplers_.add(key, cmd.sampler);
  return cmd.sampler;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Allocation
 * \{ */

void WGBackend::compute_dispatch(int groups_x_len, int groups_y_len, int groups_z_len)
{
  const uint32_t groups[3] = {uint32_t(groups_x_len), uint32_t(groups_y_len), uint32_t(groups_z_len)};
  WGContext::get()->dispatch(groups, nullptr);
}

void WGBackend::compute_dispatch_indirect(StorageBuf *indirect_buf)
{
  const uint32_t groups[3] = {0, 0, 0};
  WGContext::get()->dispatch(groups, static_cast<WGStorageBuf *>(indirect_buf));
}

Context *WGBackend::context_alloc(GHOST_IWindow *ghost_window, GHOST_IContext * /*ghost_context*/)
{
  return new WGContext(ghost_window);
}

Batch *WGBackend::batch_alloc()
{
  return new WGBatch();
}

Fence *WGBackend::fence_alloc()
{
  return new WGFence();
}

WorkInFlight *WGBackend::work_in_flight_alloc(unsigned int /*max_in_flight*/)
{
  return new WGWorkInFlight();
}

FrameBuffer *WGBackend::framebuffer_alloc(const char *name)
{
  return new WGFrameBuffer(name);
}

IndexBuf *WGBackend::indexbuf_alloc()
{
  return new WGIndexBuf();
}

PixelBuffer *WGBackend::pixelbuf_alloc(size_t size)
{
  return new WGPixelBuffer(size);
}

QueryPool *WGBackend::querypool_alloc()
{
  return new WGQueryPool();
}

Shader *WGBackend::shader_alloc(const char *name)
{
  return new WGShader(name);
}

Texture *WGBackend::texture_alloc(const char *name)
{
  return new WGTexture(name);
}

TexturePool *WGBackend::texturepool_alloc()
{
  return new TexturePoolImpl();
}

UniformBuf *WGBackend::uniformbuf_alloc(size_t size, const char *name)
{
  return new WGUniformBuf(size, name);
}

StorageBuf *WGBackend::storagebuf_alloc(size_t size, GPUUsageType usage, const char *name)
{
  return new WGStorageBuf(size, usage, name);
}

VertBuf *WGBackend::vertbuf_alloc()
{
  return new WGVertBuf();
}

TopLevelAS *WGBackend::tlas_alloc(const char * /*name*/)
{
  /* WebGPU doesn't support ray queries. */
  BLI_assert_unreachable();
  return nullptr;
}

BottomLevelAS *WGBackend::blas_alloc(const char * /*name*/)
{
  BLI_assert_unreachable();
  return nullptr;
}

void WGBackend::render_step(bool /*force_resource_release*/) {}

/** \} */

}  // namespace blender::gpu
