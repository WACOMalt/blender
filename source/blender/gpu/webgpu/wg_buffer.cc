/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.hh"

#include "GPU_texture.hh"
#include "gpu_context_private.hh"

#include "wg_backend.hh"
#include "wg_buffer.hh"
#include "wg_common.hh"
#include "wg_state.hh"
#include "wg_texture.hh"

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
  WGRecord record;
  ensure(size, record.stream);
}

void WGBuffer::ensure(const size_t size, WGCommandStream &stream)
{
  if (id_ != WG_NULL_ID && size_ >= size) {
    return;
  }
  if (id_ != WG_NULL_ID) {
    stream.push(WGCommandType::Destroy, WGCmdDestroy{id_});
  }
  size_ = WGCommandStream::align_up(max_ii(int(size), 4), 4);
  id_ = wg_object_id_alloc();
  stream.push(WGCommandType::CreateBuffer, WGCmdCreateBuffer{id_, size_});
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
  WGRecord record;
  update(record.stream, data, size, offset);
}

void WGBuffer::update(WGCommandStream &stream,
                      const void *data,
                      const size_t size,
                      const size_t offset)
{
  if (size == 0) {
    return;
  }
  ensure(offset + size, stream);
  /* Copies need 4 byte aligned offsets and sizes. */
  const size_t aligned_offset = offset & ~size_t(3);
  const size_t head = offset - aligned_offset;
  const size_t aligned_size = WGCommandStream::align_up(head + size, 4);
  const uint64_t staging_offset = stream.staging_alloc(aligned_size);
  uint8_t *staging = stream.staging.data() + staging_offset;
  memset(staging, 0, aligned_size);
  memcpy(staging + head, data, size);
  WGCmdCopyStagingToBuffer cmd = {};
  cmd.buffer = id_;
  cmd.staging_offset = staging_offset;
  cmd.buffer_offset = aligned_offset;
  cmd.size = std::min<uint64_t>(aligned_size, size_ - aligned_offset);
  stream.push(WGCommandType::CopyStagingToBuffer, cmd);
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
  converted_buffer_.free();
  has_converted_ = false;
  padded_buffer_.free();
  has_padded_ = false;
  MEM_SAFE_DELETE(data_);
}

void WGVertBuf::upload_data()
{
  binding_ensure();
}

static bool wg_format_needs_conversion(const GPUVertFormat &format)
{
  for (uint32_t i = 0; i < format.attr_len; i++) {
    if (format.attrs[i].type.format == VertAttrType::SNORM_10_10_10_2) {
      return true;
    }
  }
  return false;
}

static int16_t wg_snorm_bits_to_snorm16(const uint32_t bits, const int bit_count)
{
  /* Sign extend. */
  const int32_t value = int32_t(bits << (32 - bit_count)) >> (32 - bit_count);
  const int32_t max_value = (1 << (bit_count - 1)) - 1;
  const float normalized = std::max(float(value) / float(max_value), -1.0f);
  return int16_t(std::round(normalized * 32767.0f));
}

/**
 * Convert the 10 bit normals of `format` to 16 bits per component, returns the converted data.
 */
static Vector<uint8_t> wg_convert_vertex_data(const GPUVertFormat &format,
                                              const uint8_t *data,
                                              const uint32_t vertex_len,
                                              GPUVertFormat &r_format)
{
  r_format = format;
  /* Offsets of interleaved attributes grow by the size of the converted attributes before. */
  uint32_t growth = 0;
  Vector<uint32_t, 16> order(format.attr_len);
  for (uint32_t i = 0; i < format.attr_len; i++) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return format.attrs[a].offset < format.attrs[b].offset;
  });
  for (const uint32_t i : order) {
    r_format.attrs[i].offset = uint8_t(format.attrs[i].offset + growth);
    if (format.attrs[i].type.format == VertAttrType::SNORM_10_10_10_2) {
      r_format.attrs[i].type.format = VertAttrType::SNORM_16_16_16_16;
      growth += 4;
    }
  }
  r_format.stride = format.stride + growth;

  Vector<uint8_t> result(size_t(r_format.stride) * vertex_len);
  size_t src_section = 0, dst_section = 0;
  for (uint32_t i = 0; i < format.attr_len; i++) {
    const GPUVertAttr &src_attr = format.attrs[i];
    const GPUVertAttr &dst_attr = r_format.attrs[i];
    const size_t src_size = src_attr.type.size();
    const size_t dst_size = dst_attr.type.size();
    for (uint32_t v = 0; v < vertex_len; v++) {
      const uint8_t *src = format.deinterleaved ?
                               data + src_section + v * src_size :
                               data + size_t(v) * format.stride + src_attr.offset;
      uint8_t *dst = r_format.deinterleaved ?
                         result.data() + dst_section + v * dst_size :
                         result.data() + size_t(v) * r_format.stride + dst_attr.offset;
      if (src_attr.type.format == VertAttrType::SNORM_10_10_10_2) {
        uint32_t packed;
        memcpy(&packed, src, sizeof(packed));
        const int16_t components[4] = {wg_snorm_bits_to_snorm16(packed & 0x3FF, 10),
                                       wg_snorm_bits_to_snorm16((packed >> 10) & 0x3FF, 10),
                                       wg_snorm_bits_to_snorm16((packed >> 20) & 0x3FF, 10),
                                       wg_snorm_bits_to_snorm16(packed >> 30, 2)};
        memcpy(dst, components, sizeof(components));
      }
      else {
        memcpy(dst, src, src_size);
      }
    }
    src_section += src_size * vertex_len;
    dst_section += dst_size * vertex_len;
  }
  return result;
}

/** Buffer texture formats without WebGPU equivalent, stored with 4 components. */
static bool wg_format_needs_padding(const GPUVertFormat &format)
{
  return format.attr_len == 1 && ELEM(format.attrs[0].type.format,
                                      VertAttrType::SFLOAT_32_32_32,
                                      VertAttrType::SINT_32_32_32,
                                      VertAttrType::UINT_32_32_32);
}

static Vector<uint32_t> wg_pad_vertex_data(const void *data, const uint32_t vertex_len)
{
  const uint32_t *src = static_cast<const uint32_t *>(data);
  Vector<uint32_t> result(size_t(vertex_len) * 4, 0);
  for (uint32_t v = 0; v < vertex_len; v++) {
    for (int c = 0; c < 3; c++) {
      result[v * 4 + c] = src[v * 3 + c];
    }
  }
  return result;
}

WGVertBuf::~WGVertBuf()
{
  if (buffer_texture_) {
    GPU_texture_free(buffer_texture_);
    buffer_texture_ = nullptr;
  }
}

WGBufferBinding WGVertBuf::texture_binding_ensure()
{
  const WGBufferBinding binding = binding_ensure();
  if (has_padded_) {
    return {padded_buffer_.id_get(), 0, padded_buffer_.size_get()};
  }
  return binding;
}

WGBufferBinding WGVertBuf::vertex_binding_ensure(const GPUVertFormat *&r_format)
{
  const WGBufferBinding binding = binding_ensure();
  if (has_converted_) {
    r_format = &converted_format_;
    return {converted_buffer_.id_get(), 0, converted_buffer_.size_get()};
  }
  r_format = &format;
  return binding;
}

WGBufferBinding WGVertBuf::binding_ensure()
{
  if (flag & GPU_VERTBUF_DATA_DIRTY) {
    const size_t size = this->size_used_get();
    buffer_.ensure(size);
    if (usage_ != GPU_USAGE_DEVICE_ONLY && data_) {
      buffer_.update(data_, size);
      has_converted_ = wg_format_needs_conversion(format);
      if (has_converted_) {
        const Vector<uint8_t> converted = wg_convert_vertex_data(
            format, data_, vertex_len, converted_format_);
        converted_buffer_.ensure(converted.size());
        converted_buffer_.update(converted.data(), converted.size());
      }
      has_padded_ = (extended_usage_ & GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY) &&
                    wg_format_needs_padding(format);
      if (has_padded_) {
        const Vector<uint32_t> padded = wg_pad_vertex_data(data_, vertex_len);
        padded_buffer_.ensure(padded.size() * sizeof(uint32_t));
        padded_buffer_.update(padded.data(), padded.size() * sizeof(uint32_t));
      }
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

void WGVertBuf::bind_as_texture(uint binding)
{
  /* Buffer textures are 2D textures with a copy of the data, see #WGTexture::source_vbo_. */
  if (buffer_texture_ == nullptr) {
    buffer_texture_ = static_cast<WGTexture *>(
        GPU_texture_create_from_vertbuf("buffer_texture", this));
  }
  if (buffer_texture_) {
    wg_state_manager().texture_bind(
        buffer_texture_, GPUSamplerState::default_sampler(), int(binding));
  }
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
  device_dirty_ = true;
  device_is_source_ = false;
}

void WGUniformBuf::clear_to_zero()
{
  if (data_ == nullptr) {
    data_ = MEM_new_array_uninitialized<uint8_t>(size_in_bytes_, __func__);
  }
  memset(data_, 0, size_in_bytes_);
  uploaded_generation_ = 0;
  device_dirty_ = true;
  device_is_source_ = false;
}

void WGUniformBuf::host_data_set(const void *data, const size_t size)
{
  if (size != size_t(size_in_bytes_)) {
    MEM_SAFE_DELETE_VOID(data_);
    size_in_bytes_ = size;
  }
  update(data);
}

WGBufferBinding WGUniformBuf::storage_binding_ensure(WGCommandStream &stream,
                                                     const bool writable)
{
  if (data_ == nullptr) {
    clear_to_zero();
  }
  const size_t binding_size = WGCommandStream::align_up(size_in_bytes_, 16);
  device_buffer_.ensure(binding_size, stream);
  if (device_dirty_) {
    device_buffer_.update(stream, data_, size_in_bytes_);
    device_dirty_ = false;
  }
  if (writable) {
    device_is_source_ = true;
  }
  return {device_buffer_.id_get(), 0, binding_size};
}

WGBufferBinding WGUniformBuf::binding_ensure(WGCommandStream &stream,
                                             const uint64_t stream_generation)
{
  if (device_is_source_) {
    /* Written by shaders, the device buffer has the current content. */
    return {device_buffer_.id_get(), 0, WGCommandStream::align_up(size_in_bytes_, 16)};
  }
  if (uploaded_generation_ != stream_generation) {
    if (data_ == nullptr) {
      clear_to_zero();
    }
    /* Bindings of uniform buffers are rounded up to 16 bytes, keep the padding in the stream. */
    const size_t binding_size = WGCommandStream::align_up(size_in_bytes_, 16);
    uploaded_offset_ = stream.staging_alloc(binding_size);
    uint8_t *dst = stream.staging.data() + uploaded_offset_;
    memcpy(dst, data_, size_in_bytes_);
    memset(dst + size_in_bytes_, 0, binding_size - size_in_bytes_);
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
