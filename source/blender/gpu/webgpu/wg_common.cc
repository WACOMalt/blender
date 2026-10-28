/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_hash_mm2a.hh"

#include "wg_common.hh"
#include "wg_backend.hh"

namespace blender::gpu {

WGRecorder &wg_recorder()
{
  static WGRecorder recorder;
  return recorder;
}

WGObjectID wg_object_id_alloc()
{
  return WGBackend::get().device.object_id_alloc();
}

void wg_object_destroy(const WGObjectID id)
{
  if (id == WG_NULL_ID) {
    return;
  }
  WGRecord record;
  record.stream.push(WGCommandType::Destroy, WGCmdDestroy{id});
}

TextureFormat wg_device_format(const TextureFormat format)
{
  using TF = TextureFormat;
  switch (format) {
    /* WebGPU has no 3 component formats. */
    case TF::SNORM_8_8_8:
      return TF::SNORM_8_8_8_8;
    case TF::UNORM_8_8_8:
      return TF::UNORM_8_8_8_8;
    case TF::SINT_8_8_8:
      return TF::SINT_8_8_8_8;
    case TF::UINT_8_8_8:
      return TF::UINT_8_8_8_8;
    case TF::SINT_16_16_16:
      return TF::SINT_16_16_16_16;
    case TF::UINT_16_16_16:
      return TF::UINT_16_16_16_16;
    case TF::SINT_32_32_32:
      return TF::SINT_32_32_32_32;
    case TF::UINT_32_32_32:
      return TF::UINT_32_32_32_32;
    case TF::SFLOAT_16_16_16:
      return TF::SFLOAT_16_16_16_16;
    case TF::SFLOAT_32_32_32:
      return TF::SFLOAT_32_32_32_32;
    case TF::SRGBA_8_8_8:
      return TF::SRGBA_8_8_8_8;
    /* 16 bit normalized formats are optional in WebGPU, use half floats. */
    case TF::SNORM_16:
    case TF::UNORM_16:
      return TF::SFLOAT_16;
    case TF::SNORM_16_16:
    case TF::UNORM_16_16:
      return TF::SFLOAT_16_16;
    case TF::SNORM_16_16_16:
    case TF::SNORM_16_16_16_16:
    case TF::UNORM_16_16_16:
    case TF::UNORM_16_16_16_16:
      return TF::SFLOAT_16_16_16_16;
    default:
      return format;
  }
}

WGPUTextureFormat wg_texture_format(const TextureFormat device_format)
{
  using TF = TextureFormat;
  const WGDeviceInfo &info = WGBackend::get().device.info();
  switch (device_format) {
    case TF::SNORM_8:
      return WGPUTextureFormat_R8Snorm;
    case TF::SNORM_8_8:
      return WGPUTextureFormat_RG8Snorm;
    case TF::SNORM_8_8_8_8:
      return WGPUTextureFormat_RGBA8Snorm;
    case TF::UNORM_8:
      return WGPUTextureFormat_R8Unorm;
    case TF::UNORM_8_8:
      return WGPUTextureFormat_RG8Unorm;
    case TF::UNORM_8_8_8_8:
      return WGPUTextureFormat_RGBA8Unorm;
    case TF::SINT_8:
      return WGPUTextureFormat_R8Sint;
    case TF::SINT_8_8:
      return WGPUTextureFormat_RG8Sint;
    case TF::SINT_8_8_8_8:
      return WGPUTextureFormat_RGBA8Sint;
    case TF::SINT_16:
      return WGPUTextureFormat_R16Sint;
    case TF::SINT_16_16:
      return WGPUTextureFormat_RG16Sint;
    case TF::SINT_16_16_16_16:
      return WGPUTextureFormat_RGBA16Sint;
    case TF::SINT_32:
      return WGPUTextureFormat_R32Sint;
    case TF::SINT_32_32:
      return WGPUTextureFormat_RG32Sint;
    case TF::SINT_32_32_32_32:
      return WGPUTextureFormat_RGBA32Sint;
    case TF::UINT_8:
      return WGPUTextureFormat_R8Uint;
    case TF::UINT_8_8:
      return WGPUTextureFormat_RG8Uint;
    case TF::UINT_8_8_8_8:
      return WGPUTextureFormat_RGBA8Uint;
    case TF::UINT_16:
      return WGPUTextureFormat_R16Uint;
    case TF::UINT_16_16:
      return WGPUTextureFormat_RG16Uint;
    case TF::UINT_16_16_16_16:
      return WGPUTextureFormat_RGBA16Uint;
    case TF::UINT_32:
      return WGPUTextureFormat_R32Uint;
    case TF::UINT_32_32:
      return WGPUTextureFormat_RG32Uint;
    case TF::UINT_32_32_32_32:
      return WGPUTextureFormat_RGBA32Uint;
    case TF::SFLOAT_16:
      return WGPUTextureFormat_R16Float;
    case TF::SFLOAT_16_16:
      return WGPUTextureFormat_RG16Float;
    case TF::SFLOAT_16_16_16_16:
      return WGPUTextureFormat_RGBA16Float;
    case TF::SFLOAT_32:
      return WGPUTextureFormat_R32Float;
    case TF::SFLOAT_32_32:
      return WGPUTextureFormat_RG32Float;
    case TF::SFLOAT_32_32_32_32:
      return WGPUTextureFormat_RGBA32Float;
    case TF::UNORM_10_10_10_2:
      return WGPUTextureFormat_RGB10A2Unorm;
    case TF::UINT_10_10_10_2:
      return WGPUTextureFormat_RGB10A2Uint;
    case TF::UFLOAT_11_11_10:
      return WGPUTextureFormat_RG11B10Ufloat;
    case TF::UFLOAT_9_9_9_EXP_5:
      return WGPUTextureFormat_RGB9E5Ufloat;
    case TF::UNORM_16_DEPTH:
      return WGPUTextureFormat_Depth16Unorm;
    case TF::SFLOAT_32_DEPTH:
      return WGPUTextureFormat_Depth32Float;
    case TF::SFLOAT_32_DEPTH_UINT_8:
      return info.depth32float_stencil8 ? WGPUTextureFormat_Depth32FloatStencil8 :
                                         WGPUTextureFormat_Depth24PlusStencil8;
    case TF::SRGBA_8_8_8_8:
      return WGPUTextureFormat_RGBA8UnormSrgb;
    case TF::SNORM_DXT1:
      return WGPUTextureFormat_BC1RGBAUnorm;
    case TF::SNORM_DXT3:
      return WGPUTextureFormat_BC2RGBAUnorm;
    case TF::SNORM_DXT5:
      return WGPUTextureFormat_BC3RGBAUnorm;
    case TF::SRGB_DXT1:
      return WGPUTextureFormat_BC1RGBAUnormSrgb;
    case TF::SRGB_DXT3:
      return WGPUTextureFormat_BC2RGBAUnormSrgb;
    case TF::SRGB_DXT5:
      return WGPUTextureFormat_BC3RGBAUnormSrgb;
    default:
      return WGPUTextureFormat_Undefined;
  }
}

bool wg_texture_format_is_renderable(const TextureFormat device_format)
{
  using TF = TextureFormat;
  switch (device_format) {
    case TF::SNORM_8:
    case TF::SNORM_8_8:
    case TF::SNORM_8_8_8_8:
    case TF::UFLOAT_9_9_9_EXP_5:
    case TF::SNORM_DXT1:
    case TF::SNORM_DXT3:
    case TF::SNORM_DXT5:
    case TF::SRGB_DXT1:
    case TF::SRGB_DXT3:
    case TF::SRGB_DXT5:
      return false;
    case TF::UFLOAT_11_11_10:
      return WGBackend::get().device.info().rg11b10ufloat_renderable;
    default:
      return wg_texture_format(device_format) != WGPUTextureFormat_Undefined;
  }
}

bool wg_texture_format_is_storage(const TextureFormat device_format)
{
  using TF = TextureFormat;
  /* Storage texture formats of core WebGPU. */
  switch (device_format) {
    case TF::UNORM_8_8_8_8:
    case TF::SNORM_8_8_8_8:
    case TF::UINT_8_8_8_8:
    case TF::SINT_8_8_8_8:
    case TF::UINT_16_16_16_16:
    case TF::SINT_16_16_16_16:
    case TF::SFLOAT_16_16_16_16:
    case TF::SFLOAT_32:
    case TF::UINT_32:
    case TF::SINT_32:
    case TF::SFLOAT_32_32:
    case TF::UINT_32_32:
    case TF::SINT_32_32:
    case TF::SFLOAT_32_32_32_32:
    case TF::UINT_32_32_32_32:
    case TF::SINT_32_32_32_32:
      return true;
    default:
      return false;
  }
}

WGPUTextureSampleType wg_texture_sample_type(const TextureFormat device_format)
{
  using TF = TextureFormat;
  switch (device_format) {
    case TF::UNORM_16_DEPTH:
    case TF::SFLOAT_32_DEPTH:
    case TF::SFLOAT_32_DEPTH_UINT_8:
      return WGPUTextureSampleType_Depth;
    case TF::SINT_8:
    case TF::SINT_8_8:
    case TF::SINT_8_8_8_8:
    case TF::SINT_16:
    case TF::SINT_16_16:
    case TF::SINT_16_16_16_16:
    case TF::SINT_32:
    case TF::SINT_32_32:
    case TF::SINT_32_32_32_32:
      return WGPUTextureSampleType_Sint;
    case TF::UINT_8:
    case TF::UINT_8_8:
    case TF::UINT_8_8_8_8:
    case TF::UINT_16:
    case TF::UINT_16_16:
    case TF::UINT_16_16_16_16:
    case TF::UINT_32:
    case TF::UINT_32_32:
    case TF::UINT_32_32_32_32:
    case TF::UINT_10_10_10_2:
      return WGPUTextureSampleType_Uint;
    case TF::SFLOAT_32:
    case TF::SFLOAT_32_32:
    case TF::SFLOAT_32_32_32_32:
      /* 32 bit floats can only be filtered with an optional feature. */
      return WGBackend::get().device.info().float32_filterable ?
                 WGPUTextureSampleType_Float :
                 WGPUTextureSampleType_UnfilterableFloat;
    default:
      return WGPUTextureSampleType_Float;
  }
}

bool wg_texture_format_is_integer(const WGPUTextureFormat format)
{
  switch (format) {
    case WGPUTextureFormat_R8Uint:
    case WGPUTextureFormat_R8Sint:
    case WGPUTextureFormat_R16Uint:
    case WGPUTextureFormat_R16Sint:
    case WGPUTextureFormat_RG8Uint:
    case WGPUTextureFormat_RG8Sint:
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RG16Uint:
    case WGPUTextureFormat_RG16Sint:
    case WGPUTextureFormat_RGBA8Uint:
    case WGPUTextureFormat_RGBA8Sint:
    case WGPUTextureFormat_RGB10A2Uint:
    case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RGBA16Uint:
    case WGPUTextureFormat_RGBA16Sint:
    case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGBA32Sint:
      return true;
    default:
      return false;
  }
}

bool wg_texture_format_has_stencil(const WGPUTextureFormat format)
{
  return ELEM(format,
              WGPUTextureFormat_Depth24PlusStencil8,
              WGPUTextureFormat_Depth32FloatStencil8,
              WGPUTextureFormat_Stencil8);
}

uint64_t wg_hash_bytes(const void *data, const size_t size)
{
  const uchar *bytes = static_cast<const uchar *>(data);
  return (uint64_t(BLI_hash_mm2(bytes, size, 0)) << 32) | BLI_hash_mm2(bytes, size, 0x9747b28c);
}

WGPUVertexFormat wg_vertex_format(const gpu::VertAttrType type)
{
  using VT = gpu::VertAttrType;
  switch (type) {
    case VT::SNORM_8_8_8_8:
      return WGPUVertexFormat_Snorm8x4;
    case VT::SNORM_16_16:
      return WGPUVertexFormat_Snorm16x2;
    case VT::SNORM_16_16_16_16:
      return WGPUVertexFormat_Snorm16x4;
    case VT::UNORM_8_8_8_8:
      return WGPUVertexFormat_Unorm8x4;
    case VT::UNORM_16_16:
      return WGPUVertexFormat_Unorm16x2;
    case VT::UNORM_16_16_16_16:
      return WGPUVertexFormat_Unorm16x4;
    case VT::SINT_8_8_8_8:
      return WGPUVertexFormat_Sint8x4;
    case VT::SINT_16_16:
      return WGPUVertexFormat_Sint16x2;
    case VT::SINT_16_16_16_16:
      return WGPUVertexFormat_Sint16x4;
    case VT::SINT_32:
      return WGPUVertexFormat_Sint32;
    case VT::SINT_32_32:
      return WGPUVertexFormat_Sint32x2;
    case VT::SINT_32_32_32:
      return WGPUVertexFormat_Sint32x3;
    case VT::SINT_32_32_32_32:
      return WGPUVertexFormat_Sint32x4;
    case VT::UINT_8_8_8_8:
      return WGPUVertexFormat_Uint8x4;
    case VT::UINT_16_16:
      return WGPUVertexFormat_Uint16x2;
    case VT::UINT_16_16_16_16:
      return WGPUVertexFormat_Uint16x4;
    case VT::UINT_32:
      return WGPUVertexFormat_Uint32;
    case VT::UINT_32_32:
      return WGPUVertexFormat_Uint32x2;
    case VT::UINT_32_32_32:
      return WGPUVertexFormat_Uint32x3;
    case VT::UINT_32_32_32_32:
      return WGPUVertexFormat_Uint32x4;
    case VT::SFLOAT_16_16:
      return WGPUVertexFormat_Float16x2;
    case VT::SFLOAT_16_16_16_16:
      return WGPUVertexFormat_Float16x4;
    case VT::SFLOAT_32:
      return WGPUVertexFormat_Float32;
    case VT::SFLOAT_32_32:
      return WGPUVertexFormat_Float32x2;
    case VT::SFLOAT_32_32_32:
      return WGPUVertexFormat_Float32x3;
    case VT::SFLOAT_32_32_32_32:
      return WGPUVertexFormat_Float32x4;
    case VT::UNORM_10_10_10_2:
      return WGPUVertexFormat_Unorm10_10_10_2;
    default:
      /* TODO: SNORM_10_10_10_2 (used for normals) has no WebGPU equivalent. */
      return WGPUVertexFormat_Force32;
  }
}

}  // namespace blender::gpu
