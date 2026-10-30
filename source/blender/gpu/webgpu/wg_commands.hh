/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Command stream of the WebGPU backend.
 *
 * Blender runs on a pthread, WebGPU must be used on the browser's main thread (its asynchronous
 * results are delivered there). Blender's GPU calls record commands into a #WGCommandStream,
 * which is executed on the main thread by #WGDevice. Objects are referred to by #WGObjectID,
 * allocated when recording, so creation commands don't need a round-trip.
 *
 * Data uploads are stored in the stream's staging data. When executing, it is copied into a
 * buffer created mapped, and uploads become copies in the command encoder, keeping them ordered
 * with the draws. See #WG_STREAM_DATA_ID for data used directly from that buffer.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "BLI_vector.hh"

#include <webgpu/webgpu.h>

namespace blender::gpu {

using WGObjectID = uint32_t;
constexpr WGObjectID WG_NULL_ID = 0;
/** The texture of the canvas for the current frame. */
constexpr WGObjectID WG_SURFACE_TEXTURE_ID = 0xFFFFFFFFu;
/**
 * The buffer with the stream's data (#WGCommandStream::staging). Besides being the source of
 * uploads it can be bound as uniform, vertex, index or storage buffer, for data only used by
 * the stream's draws (push constants, immediate mode vertices...).
 */
constexpr WGObjectID WG_STREAM_DATA_ID = 0xFFFFFFFEu;

enum class WGCommandType : uint32_t {
  CreateBuffer,
  CreateTexture,
  CreateSampler,
  CreateShaderModule,
  CreateBindGroupLayout,
  CreatePipelineLayout,
  CreateRenderPipeline,
  CreateComputePipeline,
  Destroy,

  CopyStagingToBuffer,
  CopyStagingToTexture,
  CopyBufferToTexture,
  CopyBufferToBuffer,
  CopyTextureToTexture,
  ClearBuffer,
  ReadBuffer,
  ReadTexture,

  BeginRenderPass,
  EndRenderPass,
  Draw,
  Dispatch,
  ClearAttachments,
  Present,
};

/* -------------------------------------------------------------------- */
/** \name Commands
 *
 * Plain data, followed by `trailing_size` bytes of command specific data.
 * \{ */

struct WGCmdCreateBuffer {
  WGObjectID buffer;
  uint64_t size;
};

struct WGCmdCreateTexture {
  WGObjectID texture;
  WGPUTextureDimension dimension;
  WGPUTextureFormat format;
  WGPUTextureUsage usage;
  uint32_t width, height, depth_or_layers;
  uint32_t mip_count;
  uint32_t sample_count;
  /** Other format views can use (sRGB or not), #WGPUTextureFormat_Undefined when none. */
  WGPUTextureFormat view_format;
};

struct WGCmdCreateSampler {
  WGObjectID sampler;
  WGPUAddressMode address_u, address_v, address_w;
  WGPUFilterMode mag_filter, min_filter;
  WGPUMipmapFilterMode mipmap_filter;
  WGPUCompareFunction compare;
  uint16_t max_anisotropy;
  float lod_max;
};

/** Trailing data: WGSL source (null terminated). */
struct WGCmdCreateShaderModule {
  WGObjectID module;
};

/** One entry of a bind group layout. */
struct WGBindingLayout {
  enum class Kind : uint8_t {
    UniformBuffer,
    StorageBuffer,
    ReadOnlyStorageBuffer,
    Texture,
    StorageTexture,
    Sampler,
    NonFilteringSampler,
    ComparisonSampler,
  };
  uint32_t binding;
  Kind kind;
  WGPUShaderStage visibility;
  WGPUTextureSampleType sample_type;
  WGPUTextureViewDimension view_dimension;
  WGPUTextureFormat storage_format;
  WGPUStorageTextureAccess storage_access;
};

/** Trailing data: `entries_len` #WGBindingLayout. */
struct WGCmdCreateBindGroupLayout {
  WGObjectID layout;
  uint32_t entries_len;
};

struct WGCmdCreatePipelineLayout {
  WGObjectID layout;
  WGObjectID bind_group_layouts[2];
};

struct WGVertexAttribute {
  WGPUVertexFormat format;
  uint32_t offset;
  uint32_t location;
};

struct WGVertexBufferLayout {
  uint32_t stride;
  WGPUVertexStepMode step_mode;
  uint32_t attributes_start;
  uint32_t attributes_len;
};

struct WGColorTarget {
  WGPUTextureFormat format;
  WGPUColorWriteMask write_mask;
  bool blend_enabled;
  WGPUBlendComponent blend_color;
  WGPUBlendComponent blend_alpha;
};

/** Value of a WGSL override (specialization constant), keyed by its id. */
struct WGPipelineConstant {
  uint32_t id;
  double value;
};

constexpr int WG_MAX_COLOR_TARGETS = 8;
constexpr int WG_MAX_VERTEX_BUFFERS = 16;

/**
 * Trailing data: `vertex_buffers_len` #WGVertexBufferLayout, then `attributes_len`
 * #WGVertexAttribute, then `constants_len` #WGPipelineConstant.
 */
struct WGCmdCreateRenderPipeline {
  WGObjectID pipeline;
  WGObjectID layout;
  WGObjectID vertex_module;
  WGObjectID fragment_module;
  WGPUPrimitiveTopology topology;
  WGPUIndexFormat strip_index_format;
  WGPUFrontFace front_face;
  WGPUCullMode cull_mode;
  uint32_t color_targets_len;
  WGColorTarget color_targets[WG_MAX_COLOR_TARGETS];
  /** #WGPUTextureFormat_Undefined when there is no depth/stencil attachment. */
  WGPUTextureFormat depth_stencil_format;
  bool depth_write;
  WGPUCompareFunction depth_compare;
  WGPUStencilFaceState stencil_front;
  WGPUStencilFaceState stencil_back;
  uint32_t stencil_read_mask;
  uint32_t stencil_write_mask;
  int32_t depth_bias;
  float depth_bias_slope_scale;
  uint32_t sample_count;
  uint32_t vertex_buffers_len;
  uint32_t attributes_len;
  uint32_t constants_len;
};

/** Trailing data: `constants_len` #WGPipelineConstant. */
struct WGCmdCreateComputePipeline {
  WGObjectID pipeline;
  WGObjectID layout;
  WGObjectID module;
  uint32_t constants_len;
};

struct WGCmdDestroy {
  WGObjectID object;
};

struct WGCmdCopyStagingToBuffer {
  WGObjectID buffer;
  uint64_t staging_offset;
  uint64_t buffer_offset;
  uint64_t size;
};

/** Staging data rows are padded to `bytes_per_row` (a multiple of 256). */
struct WGCmdCopyStagingToTexture {
  WGObjectID texture;
  uint64_t staging_offset;
  uint32_t bytes_per_row;
  uint32_t rows_per_image;
  uint32_t mip;
  uint32_t offset[3];
  uint32_t extent[3];
  WGPUTextureAspect aspect;
};

/** `bytes_per_row` is a multiple of 256, or 0 when copying a single row. */
struct WGCmdCopyBufferToTexture {
  WGObjectID buffer;
  uint64_t buffer_offset;
  uint32_t bytes_per_row;
  WGObjectID texture;
  uint32_t offset[3];
  uint32_t extent[3];
};

struct WGCmdCopyBufferToBuffer {
  WGObjectID src, dst;
  uint64_t src_offset, dst_offset, size;
};

struct WGCmdCopyTextureToTexture {
  WGObjectID src, dst;
  uint32_t src_mip, dst_mip;
  uint32_t src_offset[3], dst_offset[3];
  uint32_t extent[3];
};

struct WGCmdClearBuffer {
  WGObjectID buffer;
  uint64_t offset, size;
};

/** Copy GPU data to `destination` (memory of Blender's thread). */
struct WGCmdReadBuffer {
  WGObjectID buffer;
  uint64_t offset;
  uint64_t size;
  void *destination;
};

struct WGCmdReadTexture {
  WGObjectID texture;
  uint32_t mip;
  uint32_t offset[3];
  uint32_t extent[3];
  /** Bytes of one texel, rows of `destination` are tightly packed. */
  uint32_t texel_size;
  WGPUTextureAspect aspect;
  void *destination;
};

struct WGAttachment {
  /** #WG_NULL_ID when unused. */
  WGObjectID texture;
  WGPUTextureFormat view_format;
  uint32_t mip;
  uint32_t layer;
  WGPULoadOp load_op;
  WGPUStoreOp store_op;
  double clear_color[4];
};

/**
 * Render passes are identified so draws recorded by different contexts can be interleaved:
 * the executor switches between passes when needed, resuming them by loading the attachments.
 */
struct WGCmdBeginRenderPass {
  uint32_t pass_id;
  uint32_t color_len;
  WGAttachment color[WG_MAX_COLOR_TARGETS];
  WGAttachment depth;
  /** Not all depth formats have stencil. */
  bool has_stencil;
  float clear_depth;
  WGPULoadOp stencil_load_op;
  WGPUStoreOp stencil_store_op;
  uint32_t clear_stencil;
};

/** Resource of a bind group entry. */
struct WGBindingResource {
  uint32_t group;
  uint32_t binding;
  WGBindingLayout::Kind kind;
  WGObjectID object;
  /* Buffers. */
  uint64_t offset;
  uint64_t size;
  /* Textures views. */
  WGPUTextureViewDimension view_dimension;
  WGPUTextureFormat view_format;
  WGPUTextureAspect aspect;
  uint32_t base_mip, mip_count;
  uint32_t base_layer, layer_count;
};

struct WGVertexBufferBinding {
  WGObjectID buffer;
  uint64_t offset;
};

struct WGCmdEndRenderPass {
  uint32_t pass_id;
};

/** Trailing data: `resources_len` #WGBindingResource. */
struct WGCmdDraw {
  uint32_t pass_id;
  WGObjectID pipeline;
  WGObjectID pipeline_layout;
  uint32_t vertex_buffers_len;
  WGVertexBufferBinding vertex_buffers[WG_MAX_VERTEX_BUFFERS];
  /** #WG_NULL_ID for non indexed draws. */
  WGObjectID index_buffer;
  WGPUIndexFormat index_format;
  uint64_t index_offset;
  /** Indirect draw from this buffer when not #WG_NULL_ID. */
  WGObjectID indirect_buffer;
  uint64_t indirect_offset;
  uint32_t first, count;
  uint32_t first_instance, instance_count;
  int32_t base_vertex;
  float viewport[4];
  float depth_range[2];
  uint32_t scissor[4];
  uint32_t stencil_reference;
  float blend_constant[4];
  uint32_t resources_len;
};

/** Trailing data: `resources_len` #WGBindingResource. */
struct WGCmdDispatch {
  WGObjectID pipeline;
  WGObjectID pipeline_layout;
  uint32_t groups[3];
  /** Indirect dispatch from this buffer when not #WG_NULL_ID. */
  WGObjectID indirect_buffer;
  uint64_t indirect_offset;
  uint32_t resources_len;
};

/**
 * Clear attachments of a render pass by drawing, for clears limited to the scissor or happening
 * after draws in the pass. The stream data at `data_offset` contains the clear values: 8 color
 * values as 4 32 bit floats, signed or unsigned integers matching the attachment format, then the
 * depth as float (#WG_CLEAR_DATA_SIZE bytes).
 */
struct WGCmdClearAttachments {
  uint32_t pass_id;
  uint32_t color_mask;
  bool clear_depth;
  bool clear_stencil;
  uint32_t stencil;
  uint32_t scissor[4];
  uint64_t data_offset;
};

constexpr uint32_t WG_CLEAR_DATA_SIZE = 8 * 16 + 16;

/** Copy a texture to the canvas, flipping it vertically (see `wg_shader.hh`). */
struct WGCmdPresent {
  WGObjectID texture;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Command Stream
 * \{ */

struct WGCommandHeader {
  WGCommandType type;
  /** Size of the command struct and trailing data, aligned to 8 bytes. */
  uint32_t size;
};

class WGCommandStream {
 public:
  Vector<uint8_t, 0> commands;
  Vector<uint8_t, 0> staging;

  bool is_empty() const
  {
    return commands.is_empty();
  }

  template<typename T>
  void push(const WGCommandType type,
            const T &command,
            const void *trailing = nullptr,
            const size_t trailing_size = 0)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    const size_t size = align_8(sizeof(WGCommandHeader) + sizeof(T) + trailing_size);
    const int64_t start = commands.size();
    commands.resize(start + size);
    uint8_t *data = commands.data() + start;
    const WGCommandHeader header = {type, uint32_t(size)};
    memcpy(data, &header, sizeof(header));
    memcpy(data + sizeof(header), &command, sizeof(T));
    if (trailing_size) {
      memcpy(data + sizeof(header) + sizeof(T), trailing, trailing_size);
    }
  }

  /**
   * Reserve `size` bytes of staging data, returns the offset in the staging data.
   * Offsets are aligned to 256 bytes, as needed by texture copies.
   */
  uint64_t staging_alloc(const size_t size)
  {
    const uint64_t offset = align_up(staging.size(), 256);
    staging.resize(offset + align_up(size, 4));
    return offset;
  }

  uint64_t staging_push(const void *data, const size_t size)
  {
    const uint64_t offset = staging_alloc(size);
    memcpy(staging.data() + offset, data, size);
    return offset;
  }

  void clear()
  {
    commands.clear();
    staging.clear();
  }

  static size_t align_8(const size_t value)
  {
    return (value + 7) & ~size_t(7);
  }
  static uint64_t align_up(const uint64_t value, const uint64_t alignment)
  {
    return (value + alignment - 1) / alignment * alignment;
  }
};

/** \} */

}  // namespace blender::gpu
