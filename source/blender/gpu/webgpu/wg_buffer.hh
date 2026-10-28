/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Vertex, index, uniform and storage buffers of the WebGPU backend.
 */

#pragma once

#include "GPU_index_buffer.hh"
#include "GPU_vertex_buffer.hh"
#include "gpu_storage_buffer_private.hh"
#include "gpu_uniform_buffer_private.hh"

#include "wg_commands.hh"

namespace blender::gpu {

class WGTexture;

/** A WebGPU buffer, created with all usages. */
class WGBuffer {
  WGObjectID id_ = WG_NULL_ID;
  size_t size_ = 0;

 public:
  ~WGBuffer();

  bool is_allocated() const
  {
    return id_ != WG_NULL_ID;
  }
  WGObjectID id_get() const
  {
    return id_;
  }
  size_t size_get() const
  {
    return size_;
  }

  /** (Re-)create the buffer when it is smaller than `size`. Contents are lost when re-created. */
  void ensure(size_t size);
  void free();
  void update(const void *data, size_t size, size_t offset = 0);
  void clear_to_zero();
  /** Wait for the GPU and copy the contents to `r_data`. */
  void read(void *r_data, size_t size) const;
};

/** Where a buffer binding comes from, resolved when drawing. */
struct WGBufferBinding {
  WGObjectID buffer = WG_NULL_ID;
  uint64_t offset = 0;
  uint64_t size = 0;
};

class WGVertBuf : public VertBuf {
  WGBuffer buffer_;
  /**
   * Copy of the data with attributes WebGPU can't read as vertex attributes (10 bit normals)
   * converted to a supported type, used as vertex buffer. Storage buffer bindings use the
   * original data.
   */
  WGBuffer converted_buffer_;
  GPUVertFormat converted_format_ = {};
  bool has_converted_ = false;
  /** Data of buffer textures with 3 components padded to 4, WebGPU has no RGB formats. */
  WGBuffer padded_buffer_;
  bool has_padded_ = false;
  /** Texture used when bound as buffer texture. */
  WGTexture *buffer_texture_ = nullptr;

 public:
  ~WGVertBuf() override;

  void bind_as_ssbo(uint binding) override;
  void bind_as_texture(uint binding) override;
  void wrap_handle(uint64_t handle) override;
  void update_sub(uint start, uint len, const void *data) override;
  void copy_sub(VertBuf &source_buf, uint dst_offset, uint src_offset, uint copy_size) override;
  void read(void *data) const override;

  /** Upload the data when needed, returns the binding of the buffer. */
  WGBufferBinding binding_ensure();
  /** Binding of the buffer with the data of buffer textures. */
  WGBufferBinding texture_binding_ensure();
  /** Binding and format of the buffer when used for vertex attributes. */
  WGBufferBinding vertex_binding_ensure(const GPUVertFormat *&r_format);

 protected:
  void acquire_data() override;
  void resize_data() override;
  void release_data() override;
  void upload_data() override;
};

class WGIndexBuf : public IndexBuf {
  WGBuffer buffer_;

 public:
  void upload_data() override;
  void bind_as_ssbo(uint binding) override;
  void read(uint32_t *data) const override;
  void update_sub(uint start, uint len, const void *data) override;
  void copy_sub(IndexBuf &source_buf, uint dst_offset, uint src_offset, uint copy_size) override;
  void strip_restart_indices() override;

  /** Upload the data when needed, returns the binding of the whole buffer (of the source for
   * sub-ranges). */
  WGBufferBinding binding_ensure();
  WGPUIndexFormat wg_index_format() const
  {
    return is_32bit() ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
  }
};

/**
 * Uniform buffers are small and kept in host memory. They are copied into the stream's data
 * (#WG_STREAM_DATA_ID) the first time they are used by a stream, so updates between draws are
 * ordered correctly without interrupting render passes.
 */
class WGUniformBuf : public UniformBuf {
  uint64_t uploaded_generation_ = 0;
  uint64_t uploaded_offset_ = 0;

 public:
  WGUniformBuf(size_t size, const char *name);

  void update(const void *data) override;
  void clear_to_zero() override;
  void bind(int slot) override;
  void bind_as_ssbo(int slot) override;
  void unbind() override;

  /** Replace the data and size of the buffer. */
  void host_data_set(const void *data, size_t size);

  /** Copy the data to the current stream when needed, must be called with the recorder locked. */
  WGBufferBinding binding_ensure(WGCommandStream &stream, uint64_t stream_generation);
};

class WGStorageBuf : public StorageBuf {
  WGBuffer buffer_;
  GPUUsageType usage_;

 public:
  WGStorageBuf(size_t size, GPUUsageType usage, const char *name);

  void update(const void *data) override;
  void bind(int slot) override;
  void unbind() override;
  void clear(uint32_t clear_value) override;
  void copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size) override;
  void read(void *data) override;
  void async_flush_to_host() override {}
  void sync_as_indirect_buffer() override {}

  WGBufferBinding binding_ensure();
};

}  // namespace blender::gpu
