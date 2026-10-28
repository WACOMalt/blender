/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>

#include "BLI_math_base.hh"
#include "BLI_vector.hh"

#include "vk_data_conversion.hh"

#include "wg_backend.hh"
#include "wg_buffer.hh"
#include "wg_common.hh"
#include "wg_misc.hh"
#include "wg_texture.hh"

namespace blender::gpu {

static bool wg_format_is_compressed(const TextureFormat format)
{
  return ELEM(format,
              TextureFormat::SNORM_DXT1,
              TextureFormat::SNORM_DXT3,
              TextureFormat::SNORM_DXT5,
              TextureFormat::SRGB_DXT1,
              TextureFormat::SRGB_DXT3,
              TextureFormat::SRGB_DXT5);
}

static size_t wg_compressed_block_size(const TextureFormat format)
{
  return ELEM(format, TextureFormat::SNORM_DXT1, TextureFormat::SRGB_DXT1) ? 8 : 16;
}

WGTexture::~WGTexture()
{
  if (!is_texture_view()) {
    wg_object_destroy(id_);
  }
}

int WGTexture::wg_layers() const
{
  switch (type_) {
    case GPU_TEXTURE_1D_ARRAY:
      return max_ii(h_, 1);
    case GPU_TEXTURE_2D_ARRAY:
    case GPU_TEXTURE_CUBE:
    case GPU_TEXTURE_CUBE_ARRAY:
    case GPU_TEXTURE_3D:
      return max_ii(d_, 1);
    default:
      return 1;
  }
}

bool WGTexture::has_stencil() const
{
  return device_format_ == TextureFormat::SFLOAT_32_DEPTH_UINT_8;
}

void WGTexture::wg_mip_extent(const int mip, uint32_t r_extent[3]) const
{
  r_extent[0] = max_ii(1, w_ >> mip);
  switch (type_) {
    case GPU_TEXTURE_1D:
    case GPU_TEXTURE_1D_ARRAY:
      r_extent[1] = 1;
      break;
    default:
      r_extent[1] = max_ii(1, h_ >> mip);
      break;
  }
  r_extent[2] = (type_ == GPU_TEXTURE_3D) ? max_ii(1, d_ >> mip) : wg_layers();
}

bool WGTexture::init_internal()
{
  device_format_ = wg_device_format(format_);
  wg_format_ = wg_texture_format(device_format_);
  if (wg_format_ == WGPUTextureFormat_Undefined) {
    is_unsupported_ = true;
    return true;
  }

  WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                           WGPUTextureUsage_CopyDst;
  /* 1D textures can't be render targets in WebGPU. */
  if (wg_texture_format_is_renderable(device_format_) && type_ != GPU_TEXTURE_1D) {
    usage |= WGPUTextureUsage_RenderAttachment;
  }
  if ((gpu_image_usage_flags_ & GPU_TEXTURE_USAGE_SHADER_WRITE) &&
      wg_texture_format_is_storage(device_format_))
  {
    usage |= WGPUTextureUsage_StorageBinding;
  }

  WGCmdCreateTexture cmd = {};
  cmd.texture = id_ = wg_object_id_alloc();
  cmd.dimension = (type_ == GPU_TEXTURE_3D) ? WGPUTextureDimension_3D :
                  (type_ == GPU_TEXTURE_1D) ? WGPUTextureDimension_1D :
                                              WGPUTextureDimension_2D;
  cmd.format = wg_format_;
  cmd.usage = usage;
  uint32_t extent[3];
  wg_mip_extent(0, extent);
  cmd.width = extent[0];
  cmd.height = extent[1];
  cmd.depth_or_layers = extent[2];
  /* WebGPU 1D textures can't have mip-maps. */
  cmd.mip_count = (type_ == GPU_TEXTURE_1D) ? 1 : max_ii(mipmaps_, 1);
  cmd.sample_count = 1;
  /* Allow writing sRGB textures without conversion, and the opposite for views. */
  if (wg_format_ == WGPUTextureFormat_RGBA8UnormSrgb) {
    cmd.view_format = WGPUTextureFormat_RGBA8Unorm;
  }
  else if (wg_format_ == WGPUTextureFormat_RGBA8Unorm &&
           (gpu_image_usage_flags_ & GPU_TEXTURE_USAGE_FORMAT_VIEW))
  {
    cmd.view_format = WGPUTextureFormat_RGBA8UnormSrgb;
  }
  mipmaps_ = cmd.mip_count;

  WGRecord record;
  record.stream.push(WGCommandType::CreateTexture, cmd);
  return true;
}

bool WGTexture::init_internal(VertBuf *vbo)
{
  source_vbo_ = static_cast<WGVertBuf *>(vbo);
  device_format_ = wg_device_format(format_);
  wg_format_ = wg_texture_format(device_format_);
  is_unsupported_ = wg_format_ == WGPUTextureFormat_Undefined;
  mipmaps_ = 1;
  return true;
}

void WGTexture::buffer_texture_update()
{
  if (source_vbo_ == nullptr || is_unsupported_) {
    return;
  }
  const WGBufferBinding source = source_vbo_->texture_binding_ensure();
  const uint32_t texels = source_vbo_->vertex_len;
  if (texels == 0 || source.buffer == WG_NULL_ID) {
    return;
  }
  const uint32_t width = std::min(texels, WG_BUFFER_TEXTURE_WIDTH);
  const uint32_t height = (texels + WG_BUFFER_TEXTURE_WIDTH - 1) / WG_BUFFER_TEXTURE_WIDTH;
  const uint32_t texel_size = uint32_t(to_bytesize(device_format_));

  WGRecord record;
  if (id_ == WG_NULL_ID || width != buffer_width_ || height != buffer_height_) {
    wg_object_destroy(id_);
    WGCmdCreateTexture cmd = {};
    cmd.texture = id_ = wg_object_id_alloc();
    cmd.dimension = WGPUTextureDimension_2D;
    cmd.format = wg_format_;
    cmd.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    cmd.width = width;
    cmd.height = height;
    cmd.depth_or_layers = 1;
    cmd.mip_count = 1;
    cmd.sample_count = 1;
    record.stream.push(WGCommandType::CreateTexture, cmd);
    buffer_width_ = width;
    buffer_height_ = height;
  }
  /* Full rows, then the remaining texels. */
  const uint32_t full_rows = texels / WG_BUFFER_TEXTURE_WIDTH;
  const uint32_t remainder = texels % WG_BUFFER_TEXTURE_WIDTH;
  if (full_rows > 0) {
    WGCmdCopyBufferToTexture cmd = {};
    cmd.buffer = source.buffer;
    cmd.buffer_offset = source.offset;
    cmd.bytes_per_row = (full_rows > 1) ? WG_BUFFER_TEXTURE_WIDTH * texel_size : 0;
    cmd.texture = id_;
    cmd.extent[0] = WG_BUFFER_TEXTURE_WIDTH;
    cmd.extent[1] = full_rows;
    cmd.extent[2] = 1;
    record.stream.push(WGCommandType::CopyBufferToTexture, cmd);
  }
  if (remainder > 0) {
    WGCmdCopyBufferToTexture cmd = {};
    cmd.buffer = source.buffer;
    cmd.buffer_offset = source.offset + uint64_t(full_rows) * WG_BUFFER_TEXTURE_WIDTH *
                                            texel_size;
    cmd.texture = id_;
    cmd.offset[1] = full_rows;
    cmd.extent[0] = remainder;
    cmd.extent[1] = 1;
    cmd.extent[2] = 1;
    record.stream.push(WGCommandType::CopyBufferToTexture, cmd);
  }
}

bool WGTexture::init_internal(gpu::Texture *src, bool /*use_stencil*/)
{
  WGTexture *source = static_cast<WGTexture *>(src);
  id_ = source->id_;
  is_unsupported_ = source->is_unsupported_;
  device_format_ = wg_device_format(format_);
  wg_format_ = wg_texture_format(device_format_);
  /* Views are relative to the source texture, which may itself be a view. */
  /* Mip levels of views are relative to the view, like for other textures. */
  view_mip_start_ = source->view_mip_start_ + mip_min_;
  mip_max_ -= mip_min_;
  mip_min_ = 0;
  view_layer_start_ += source->view_layer_start_;
  return true;
}

void WGTexture::update_sub(int mip,
                           int offset[3],
                           int extent[3],
                           eGPUDataFormat format,
                           const void *data,
                           uint unpack_row_length)
{
  if (!is_usable() || data == nullptr) {
    return;
  }
  /* Rows (and 1D array layers) are copied in WebGPU's layout. */
  uint32_t dst_offset[3] = {uint32_t(offset[0]), uint32_t(max_ii(offset[1], 0)), 0};
  uint32_t copy_extent[3] = {uint32_t(extent[0]), uint32_t(max_ii(extent[1], 1)), 1};
  switch (type_) {
    case GPU_TEXTURE_1D:
      dst_offset[1] = 0;
      copy_extent[1] = 1;
      break;
    case GPU_TEXTURE_1D_ARRAY:
      /* Rows are layers. */
      dst_offset[2] = dst_offset[1];
      dst_offset[1] = 0;
      copy_extent[2] = copy_extent[1];
      copy_extent[1] = 1;
      break;
    default:
      dst_offset[2] = uint32_t(max_ii(offset[2], 0));
      copy_extent[2] = uint32_t(max_ii(extent[2], 1));
      break;
  }

  const bool compressed = wg_format_is_compressed(device_format_);
  const uint32_t rows_per_slice = compressed ? divide_ceil_u(copy_extent[1], 4) : copy_extent[1];
  const size_t row_size = compressed ? divide_ceil_u(copy_extent[0], 4) *
                                           wg_compressed_block_size(device_format_) :
                                       size_t(copy_extent[0]) * to_bytesize(device_format_);
  const size_t padded_row_size = WGCommandStream::align_up(row_size, 256);
  const size_t rows = size_t(rows_per_slice) * copy_extent[2];

  const size_t host_texel_size = compressed ? 0 : to_bytesize(format_, format);
  const size_t host_row_stride = compressed ?
                                     row_size :
                                     size_t(unpack_row_length ? unpack_row_length :
                                                                copy_extent[0]) *
                                         host_texel_size;
  const bool convert = !compressed && needs_conversion(format, format_, device_format_);

  WGRecord record;
  const uint64_t staging_offset = record.stream.staging_alloc(padded_row_size * rows);
  uint8_t *dst = record.stream.staging.data() + staging_offset;
  const uint8_t *src = static_cast<const uint8_t *>(data);
  for (size_t row = 0; row < rows; row++) {
    const uint8_t *src_row = src + row * host_row_stride;
    uint8_t *dst_row = dst + row * padded_row_size;
    if (convert) {
      convert_host_to_device(dst_row, src_row, copy_extent[0], format, format_, device_format_);
    }
    else {
      memcpy(dst_row, src_row, row_size);
    }
  }

  WGCmdCopyStagingToTexture cmd = {};
  cmd.texture = id_;
  cmd.staging_offset = staging_offset;
  cmd.bytes_per_row = uint32_t(padded_row_size);
  cmd.rows_per_image = rows_per_slice;
  cmd.mip = uint32_t(view_mip_start_ + mip);
  for (int i = 0; i < 3; i++) {
    cmd.offset[i] = dst_offset[i];
    cmd.extent[i] = copy_extent[i];
  }
  if (type_ != GPU_TEXTURE_3D) {
    cmd.offset[2] += view_layer_start_;
  }
  cmd.aspect = WGPUTextureAspect_All;
  record.stream.push(WGCommandType::CopyStagingToTexture, cmd);
}

void WGTexture::update_sub(int offset[3],
                           int extent[3],
                           eGPUDataFormat format,
                           GPUPixelBuffer *pixbuf)
{
  WGPixelBuffer *pixel_buffer = reinterpret_cast<WGPixelBuffer *>(pixbuf);
  update_sub(0, offset, extent, format, pixel_buffer->data(), 0);
}

void WGTexture::read(int mip, eGPUDataFormat format, void *r_data)
{
  if (!is_usable()) {
    return;
  }
  uint32_t extent[3];
  wg_mip_extent(mip, extent);
  if (type_ != GPU_TEXTURE_3D) {
    extent[2] = layer_count();
  }
  const uint32_t offset[3] = {0, 0, uint32_t((type_ == GPU_TEXTURE_3D) ? 0 : view_layer_start_)};
  read_region(mip, offset, extent, format, r_data);
}

void WGTexture::read_region(const int mip,
                            const uint32_t offset[3],
                            const uint32_t extent[3],
                            const eGPUDataFormat format,
                            void *r_data)
{
  if (!is_usable()) {
    return;
  }
  const size_t texel_size = to_bytesize(device_format_);
  const size_t pixels = size_t(extent[0]) * extent[1] * extent[2];
  Vector<uint8_t> device_data(pixels * texel_size);

  WGCmdReadTexture cmd = {};
  cmd.texture = id_;
  cmd.mip = uint32_t(view_mip_start_ + mip);
  for (int i = 0; i < 3; i++) {
    cmd.offset[i] = offset[i];
    cmd.extent[i] = extent[i];
  }
  cmd.texel_size = uint32_t(texel_size);
  cmd.aspect = (format_flag_ & GPU_FORMAT_DEPTH) ? WGPUTextureAspect_DepthOnly :
                                                   WGPUTextureAspect_All;
  cmd.destination = device_data.data();

  WGCommandStream stream;
  {
    WGRecord record;
    record.stream.push(WGCommandType::ReadTexture, cmd);
    wg_recorder().take(stream);
  }
  WGBackend::get().device.submit_and_wait(stream);

  convert_device_to_host(r_data, device_data.data(), pixels, format, format_, device_format_);
}

void WGTexture::copy_to(Texture *dst, IndexRange mip_levels)
{
  WGTexture *dst_texture = static_cast<WGTexture *>(dst);
  if (!is_usable() || !dst_texture->is_usable()) {
    return;
  }
  WGRecord record;
  for (const int mip : mip_levels) {
    WGCmdCopyTextureToTexture cmd = {};
    cmd.src = id_;
    cmd.dst = dst_texture->id_;
    cmd.src_mip = uint32_t(view_mip_start_ + mip);
    cmd.dst_mip = uint32_t(dst_texture->view_mip_start_ + mip);
    cmd.src_offset[2] = (type_ == GPU_TEXTURE_3D) ? 0 : view_layer_start_;
    cmd.dst_offset[2] = (type_ == GPU_TEXTURE_3D) ? 0 : dst_texture->view_layer_start_;
    wg_mip_extent(mip, cmd.extent);
    if (type_ != GPU_TEXTURE_3D) {
      cmd.extent[2] = layer_count();
    }
    record.stream.push(WGCommandType::CopyTextureToTexture, cmd);
  }
}

void WGTexture::clear(double4 data)
{
  if (!is_usable()) {
    return;
  }
  /* Render-able textures are cleared by render passes, depth can't be uploaded. */
  if (wg_texture_format_is_renderable(device_format_) &&
      !ELEM(type_, GPU_TEXTURE_1D, GPU_TEXTURE_3D, GPU_TEXTURE_BUFFER))
  {
    const bool is_depth = (format_flag_ & GPU_FORMAT_DEPTH) != 0;
    const int layers = (type_ & (GPU_TEXTURE_ARRAY | GPU_TEXTURE_CUBE)) ? layer_count() : 1;
    WGRecord record;
    for (int mip = 0; mip < max_ii(mipmaps_, 1); mip++) {
      for (int layer = 0; layer < layers; layer++) {
        WGCmdBeginRenderPass cmd = {};
        cmd.pass_id = wg_recorder().next_pass_id++;
        WGAttachment &attachment = is_depth ? cmd.depth : cmd.color[0];
        attachment.texture = id_;
        attachment.view_format = wg_format_;
        attachment.mip = uint32_t(view_mip_start_ + mip);
        attachment.layer = uint32_t(view_layer_start_ + layer);
        attachment.load_op = WGPULoadOp_Clear;
        attachment.store_op = WGPUStoreOp_Store;
        if (is_depth) {
          cmd.clear_depth = float(data[0]);
          cmd.has_stencil = has_stencil();
          cmd.stencil_load_op = WGPULoadOp_Clear;
          cmd.stencil_store_op = WGPUStoreOp_Store;
          cmd.clear_stencil = (data[1] == double(0xFFFFFFFFu)) ? 0u : uint32_t(data[1]);
        }
        else {
          cmd.color_len = 1;
          for (int i = 0; i < 4; i++) {
            attachment.clear_color[i] = data[i];
          }
        }
        record.stream.push(WGCommandType::BeginRenderPass, cmd);
        record.stream.push(WGCommandType::EndRenderPass, WGCmdEndRenderPass{cmd.pass_id});
      }
    }
    return;
  }
  /* Upload the clear value to every texel of every mip level, which works for all formats.
   * TODO: use render passes for render-able formats. */
  const bool is_integer = format_flag_ & GPU_FORMAT_INTEGER;
  const eGPUDataFormat data_format = is_integer ? ((format_flag_ & GPU_FORMAT_SIGNED) ?
                                                       GPU_DATA_INT :
                                                       GPU_DATA_UINT) :
                                                  GPU_DATA_FLOAT;
  const int components = to_component_len(format_);
  for (int mip = 0; mip < mipmaps_; mip++) {
    int3 size = mip_size_get(mip);
    size = int3(max_ii(size.x, 1), max_ii(size.y, 1), max_ii(size.z, 1));
    const size_t pixels = size_t(size.x) * size.y * size.z;
    Vector<uint32_t> texels(pixels * components);
    for (size_t i = 0; i < pixels; i++) {
      for (int c = 0; c < components; c++) {
        if (is_integer) {
          texels[i * components + c] = uint32_t(int32_t(data[c]));
        }
        else {
          const float value = float(data[c]);
          memcpy(&texels[i * components + c], &value, sizeof(float));
        }
      }
    }
    int offset[3] = {0, 0, 0};
    int extent[3] = {size.x, size.y, size.z};
    update_sub(mip, offset, extent, data_format, texels.data(), 0);
  }
}

void WGTexture::generate_mipmap()
{
  /* TODO: WebGPU doesn't generate mip-maps, needs a render pass per level. */
}

void WGTexture::swizzle_set(const char /*swizzle_mask*/[4])
{
  /* TODO: WebGPU texture views can't swizzle, it has to be done in shaders. */
}

}  // namespace blender::gpu
