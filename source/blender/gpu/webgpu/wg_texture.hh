/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_private.hh"

#include "wg_commands.hh"

namespace blender::gpu {

class WGTexture : public Texture {
  /** The WebGPU texture, of the source texture for views. */
  WGObjectID id_ = WG_NULL_ID;
  /** Format of the WebGPU texture, which can differ from #format_ (e.g. RGB is RGBA). */
  TextureFormat device_format_ = TextureFormat::Invalid;
  WGPUTextureFormat wg_format_ = WGPUTextureFormat_Undefined;
  /** Not supported by WebGPU (e.g. buffer textures). */
  bool is_unsupported_ = false;

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
    return mip_min_;
  }
  int view_layer_start() const
  {
    return view_layer_start_;
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
