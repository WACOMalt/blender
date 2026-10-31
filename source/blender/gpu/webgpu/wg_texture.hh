/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <memory>

#include "BLI_math_base.hh"

#include "gpu_texture_private.hh"

#include "wg_commands.hh"

namespace blender::gpu {

constexpr uint32_t WG_BUFFER_TEXTURE_WIDTH = 4096;

class WGVertBuf;

/**
 * State of the emulated images of a texture (see `wg_shader.hh`), shared by the texture and its
 * views. Bits are mip levels of the WebGPU texture.
 */
struct WGTextureShared {
  /** Mip levels whose buffer holds the current content. */
  uint32_t buffer_valid = 0;
  /** Mip levels whose texture is outdated (the buffer was written). */
  uint32_t texture_invalid = 0;
};

class WGTexture : public Texture {
  /** The WebGPU texture, of the source texture for views. */
  WGObjectID id_ = WG_NULL_ID;
  /** Format of the WebGPU texture, which can differ from #format_ (e.g. RGB is RGBA). */
  TextureFormat device_format_ = TextureFormat::Invalid;
  WGPUTextureFormat wg_format_ = WGPUTextureFormat_Undefined;
  /** Not supported by WebGPU (e.g. buffer textures). */
  bool is_unsupported_ = false;
  /** First mip level of the WebGPU texture used by this texture view. */
  int view_mip_start_ = 0;
  /**
   * Buffer textures don't exist in WebGPU, they are 2D textures (#WG_BUFFER_TEXTURE_WIDTH wide)
   * with a copy of the vertex buffer, see `wg_shader.cc` for the shader side.
   */
  WGVertBuf *source_vbo_ = nullptr;
  uint32_t buffer_width_ = 0, buffer_height_ = 0;
  std::shared_ptr<WGTextureShared> shared_;

 public:
  WGTexture(const char *name) : Texture(name) {}
  ~WGTexture() override;

  void generate_mipmap() override;
  void copy_to(Texture *dst, IndexRange mip_levels) override;
  void clear(double4 data) override;
  void swizzle_set(const char swizzle_mask[4]) override;
  void read(int mip, eGPUDataFormat format, void *r_data) override;
  void update_sub(int mip,
                  int offset[3],
                  int extent[3],
                  eGPUDataFormat format,
                  const void *data,
                  uint unpack_row_length) override;
  void update_sub(int offset[3],
                  int extent[3],
                  eGPUDataFormat format,
                  GPUPixelBuffer *pixbuf) override;

  /** Copy the vertex buffer of a buffer texture, before it is used by a draw. */
  void buffer_texture_update();

  /**
   * Before a mip level (of the WebGPU texture) is bound as the buffer of an emulated image:
   * copies the texture to the buffer when the buffer is outdated.
   */
  void buffer_image_use(int mip, bool writable);
  /**
   * Before mip levels (of the WebGPU texture) are used as a texture (sampled, storage image,
   * attachment, copy, upload, read): copies the buffers of emulated images back when they are
   * newer. `writes` invalidates the buffers.
   */
  void texture_use(IndexRange mips, bool writes);
  /** All mip levels of the texture. */
  void texture_use(bool writes)
  {
    texture_use(IndexRange(view_mip_start_, max_ii(mipmaps_, 1)), writes);
  }

  /** Read a region of a mip level, `offset[2]` is the layer (or depth for 3D textures). */
  void read_region(int mip,
                   const uint32_t offset[3],
                   const uint32_t extent[3],
                   eGPUDataFormat format,
                   void *r_data);

  WGObjectID id_get() const
  {
    return id_;
  }
  TextureFormat device_format_get() const
  {
    return device_format_;
  }
  WGPUTextureFormat wg_format_get() const
  {
    return wg_format_;
  }
  /** First mip and layer of the WebGPU texture used by this texture (non zero for views). */
  int view_mip_start() const
  {
    return view_mip_start_;
  }
  int view_layer_start() const
  {
    return view_layer_start_;
  }
  /** Mip levels used when sampling (see #GPU_texture_mip_range_set). */
  IndexRange sampling_mips() const
  {
    const int start = view_mip_start_ + min_ii(mip_min_, mipmaps_ - 1);
    const int end = view_mip_start_ + min_ii(mip_max_, mipmaps_ - 1);
    return IndexRange(start, max_ii(end - start + 1, 1));
  }
  bool is_usable() const
  {
    return !is_unsupported_ && id_ != WG_NULL_ID;
  }
  /** Number of layers (or depth slices for 3D) of the WebGPU texture. */
  int wg_layers() const;
  bool has_stencil() const;

 protected:
  bool init_internal() override;
  bool init_internal(VertBuf *vbo) override;
  bool init_internal(gpu::Texture *src, bool use_stencil) override;

 private:
  /** Size of a mip level as stored in WebGPU (1D arrays and cube maps use layers). */
  void wg_mip_extent(int mip, uint32_t r_extent[3]) const;
};

}  // namespace blender::gpu
