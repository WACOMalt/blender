/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_base.hh"

#include "gpu_context_private.hh"

#include "wg_backend.hh"
#include "wg_buffer.hh"
#include "wg_common.hh"
#include "wg_state.hh"

namespace blender::gpu {

static WGStateManager &wg_state_manager()
{
  return *static_cast<WGStateManager *>(Context::get()->state_manager);
}

/* -------------------------------------------------------------------- */
/** \name WGBuffer
 * \{ */

WGBuffer::~WGBuffer()
{
  free();
}

void WGBuffer::ensure(const size_t size)
{
  if (id_ != WG_NULL_ID && size_ >= size) {
    return;
  }
  free();
  size_ = WGCommandStream::align_up(max_ii(int(size), 4), 4);
  id_ = wg_object_id_alloc();
  WGRecord record;
  record.stream.push(WGCommandType::CreateBuffer, WGCmdCreateBuffer{id_, size_});
}

void WGBuffer::free()
{
  wg_object_destroy(id_);
  id_ = WG_NULL_ID;
  size_ = 0;
}

void WGBuffer::update(const void *data, const size_t size, const size_t offset)
{
  if (size == 0) {
    return;
  }
  ensure(offset + size);
  /* Copies need 4 byte aligned offsets and sizes. */
  const size_t aligned_offset = offset & ~size_t(3);
  const size_t head = offset - aligned_offset;
  const size_t aligned_size = WGCommandStream::align_up(head + size, 4);
  WGRecord record;
  const uint64_t staging_offset = record.stream.staging_alloc(aligned_size);
  uint8_t *staging = record.stream.staging.data() + staging_offset;
  memset(staging, 0, aligned_size);
  memcpy(staging + head, data, size);
  WGCmdCopyStagingToBuffer cmd = {};
  cmd.buffer = id_;
  cmd.staging_offset = staging_offset;
  cmd.buffer_offset = aligned_offset;
  cmd.size = std::min<uint64_t>(aligned_size, size_ - aligned_offset);
  record.stream.push(WGCommandType::CopyStagingToBuffer, cmd);
}

void WGBuffer::clear_to_zero()
{
  if (id_ == WG_NULL_ID) {
    return;
  }
  WGRecord record;
  record.stream.push(WGCommandType::ClearBuffer, WGCmdClearBuffer{id_, 0, size_});
}

void WGBuffer::read(void *r_data, const size_t size) const
{
  if (id_ == WG_NULL_ID) {
    memset(r_data, 0, size);
    return;
  }
  WGCommandStream stream;
  {
    WGRecord record;
    WGCmdReadBuffer cmd = {};
    cmd.buffer = id_;
    cmd.offset = 0;
    cmd.size = std::min(size, size_);
    cmd.destination = r_data;
    record.stream.push(WGCommandType::ReadBuffer, cmd);
    wg_recorder().take(stream);
  }
  WGBackend::get().device.submit_and_wait(stream);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name WGVertBuf
 * \{ */

void WGVertBuf::acquire_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  MEM_SAFE_DELETE(data_);
  data_ = MEM_new_array_uninitialized<uchar>(this->size_alloc_get(), __func__);
}

void WGVertBuf::resize_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  data_ = static_cast<uchar *>(
      MEM_realloc_uninitialized(data_, sizeof(uchar) * this->size_alloc_get()));
}

void WGVertBuf::release_data()
{
  buffer_.free();
  MEM_SAFE_DELETE(data_);
}

void WGVertBuf::upload_data()
{
  binding_ensure();
}

WGBufferBinding WGVertBuf::binding_ensure()
{
  if (flag & GPU_VERTBUF_DATA_DIRTY) {
    const size_t size = this->size_used_get();
    buffer_.ensure(size);
    if (usage_ != GPU_USAGE_DEVICE_ONLY && data_) {
      buffer_.update(data_, size);
    }
    if (usage_ == GPU_USAGE_STATIC) {
      MEM_SAFE_DELETE(data_);
    }
    flag &= ~GPU_VERTBUF_DATA_DIRTY;
    flag |= GPU_VERTBUF_DATA_UPLOADED;
  }
  else if (!buffer_.is_allocated()) {
    /* Device only buffers, written by shaders. */
    buffer_.ensure(this->size_alloc_get());
  }
  return {buffer_.id_get(), 0, buffer_.size_get()};
}

void WGVertBuf::bind_as_ssbo(uint binding)
{
  wg_state_manager().storage_buffer_bind(binding_ensure(), int(binding));
}

void WGVertBuf::bind_as_texture(uint /*binding*/)
{
  /* TODO: buffer textures are not supported by WebGPU. */
}

void WGVertBuf::wrap_handle(uint64_t /*handle*/)
{
  BLI_assert_unreachable();
}

void WGVertBuf::update_sub(uint start, uint len, const void *data)
{
  binding_ensure();
  buffer_.update(data, len, start);
}

void WGVertBuf::copy_sub(VertBuf &source_buf,
                         uint source_first_vertex,
                         uint dest_first_vertex,
                         uint vertex_len)
{
  WGVertBuf &source = static_cast<WGVertBuf &>(source_buf);
  const WGBufferBinding src = source.binding_ensure();
  const WGBufferBinding dst = binding_ensure();
  const uint stride = format.stride;
  WGCmdCopyBufferToBuffer cmd = {};
  cmd.src = src.buffer;
  cmd.dst = dst.buffer;
  cmd.src_offset = uint64_t(source_first_vertex) * source.format.stride;
  cmd.dst_offset = uint64_t(dest_first_vertex) * stride;
  cmd.size = WGCommandStream::align_up(uint64_t(vertex_len) * stride, 4);
  WGRecord record;
  record.stream.push(WGCommandType::CopyBufferToBuffer, cmd);
}

void WGVertBuf::read(void *data) const
{
  buffer_.read(data, this->size_used_get());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name WGIndexBuf
 * \{ */

void WGIndexBuf::upload_data()
{
  binding_ensure();
}

WGBufferBinding WGIndexBuf::binding_ensure()
{
  if (is_subrange_) {
    return static_cast<WGIndexBuf *>(src_)->binding_ensure();
  }
  if (!buffer_.is_allocated()) {
    const size_t size = max_ii(int(size_get()), 4);
    buffer_.ensure(size);
    if (data_) {
      buffer_.update(data_, size_get());
      MEM_SAFE_DELETE_VOID(data_);
    }
  }
  return {buffer_.id_get(), 0, buffer_.size_get()};
}

void WGIndexBuf::bind_as_ssbo(uint binding)
{
  wg_state_manager().storage_buffer_bind(binding_ensure(), int(binding));
}

void WGIndexBuf::read(uint32_t *data) const
{
  buffer_.read(data, size_get());
}

void WGIndexBuf::update_sub(uint start, uint len, const void *data)
{
  binding_ensure();
  buffer_.update(data, len, start);
}

void WGIndexBuf::copy_sub(IndexBuf &source_buf,
                          uint source_first_index,
                          uint dest_first_index,
                          uint index_len)
{
  WGIndexBuf &source = static_cast<WGIndexBuf &>(source_buf);
  const size_t index_size = to_bytesize(index_type_);
  WGCmdCopyBufferToBuffer cmd = {};
  cmd.src = source.binding_ensure().buffer;
  cmd.dst = binding_ensure().buffer;
  cmd.src_offset = uint64_t(source_first_index) * index_size;
  cmd.dst_offset = uint64_t(dest_first_index) * index_size;
  cmd.size = WGCommandStream::align_up(uint64_t(index_len) * index_size, 4);
  WGRecord record;
  record.stream.push(WGCommandType::CopyBufferToBuffer, cmd);
}

void WGIndexBuf::strip_restart_indices()
{
  /* Restart indices are only used by strips, where WebGPU supports them. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name WGUniformBuf
 * \{ */

WGUniformBuf::WGUniformBuf(size_t size, const char *name) : UniformBuf(size, name) {}

void WGUniformBuf::update(const void *data)
{
  if (data_ == nullptr) {
    data_ = MEM_new_array_uninitialized<uint8_t>(size_in_bytes_, __func__);
  }
  memcpy(data_, data, size_in_bytes_);
  uploaded_generation_ = 0;
}

void WGUniformBuf::clear_to_zero()
{
  if (data_ == nullptr) {
    data_ = MEM_new_array_uninitialized<uint8_t>(size_in_bytes_, __func__);
  }
  memset(data_, 0, size_in_bytes_);
  uploaded_generation_ = 0;
}

WGBufferBinding WGUniformBuf::binding_ensure(WGCommandStream &stream,
                                             const uint64_t stream_generation)
{
  if (uploaded_generation_ != stream_generation) {
    if (data_ == nullptr) {
      clear_to_zero();
    }
    uploaded_offset_ = stream.staging_push(data_, size_in_bytes_);
    uploaded_generation_ = stream_generation;
  }
  return {WG_STREAM_DATA_ID, uploaded_offset_, WGCommandStream::align_up(size_in_bytes_, 16)};
}

void WGUniformBuf::bind(int slot)
{
  wg_state_manager().uniform_buffer_bind(this, slot);
}

void WGUniformBuf::bind_as_ssbo(int slot)
{
  wg_state_manager().storage_buffer_bind(this, slot);
}

void WGUniformBuf::unbind()
{
  wg_state_manager().uniform_buffer_unbind(this);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name WGStorageBuf
 * \{ */

WGStorageBuf::WGStorageBuf(size_t size, GPUUsageType usage, const char *name)
    : StorageBuf(size, name), usage_(usage)
{
}

WGBufferBinding WGStorageBuf::binding_ensure()
{
  if (!buffer_.is_allocated()) {
    buffer_.ensure(size_in_bytes_);
    buffer_.clear_to_zero();
  }
  return {buffer_.id_get(), 0, usage_size_in_bytes_};
}

void WGStorageBuf::update(const void *data)
{
  binding_ensure();
  buffer_.update(data, usage_size_in_bytes_);
}

void WGStorageBuf::bind(int slot)
{
  wg_state_manager().storage_buffer_bind(binding_ensure(), slot);
}

void WGStorageBuf::unbind()
{
  wg_state_manager().storage_buffer_unbind(buffer_.id_get());
}

void WGStorageBuf::clear(uint32_t clear_value)
{
  binding_ensure();
  if (clear_value == 0) {
    buffer_.clear_to_zero();
    return;
  }
  Vector<uint32_t> data(divide_ceil_ul(size_in_bytes_, 4), clear_value);
  buffer_.update(data.data(), size_in_bytes_);
}

void WGStorageBuf::copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size)
{
  WGVertBuf *source = static_cast<WGVertBuf *>(src);
  WGCmdCopyBufferToBuffer cmd = {};
  cmd.src = source->binding_ensure().buffer;
  cmd.dst = binding_ensure().buffer;
  cmd.src_offset = src_offset;
  cmd.dst_offset = dst_offset;
  cmd.size = WGCommandStream::align_up(copy_size, 4);
  WGRecord record;
  record.stream.push(WGCommandType::CopyBufferToBuffer, cmd);
}

void WGStorageBuf::read(void *data)
{
  binding_ensure();
  buffer_.read(data, usage_size_in_bytes_);
}

/** \} */

}  // namespace blender::gpu
