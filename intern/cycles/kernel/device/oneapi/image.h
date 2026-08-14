/* SPDX-FileCopyrightText: 2021-2026 Intel Corporation, Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

CCL_NAMESPACE_BEGIN

/* Software 2D image sampling for oneAPI devices without bindless image
 * support (Xe-LP integrated GPUs, e.g. Tiger Lake).
 *
 * The shared GPU image code (kernel/device/gpu/image.h) funnels every texel
 * fetch through ccl_gpu_image_object_read_2D(texobj, x, y) with normalized
 * coordinates, where texobj comes from KernelImageInfo::data and hardware
 * samplers are expected to apply wrap + filtering. Here texobj instead points
 * to a device-side shadow KernelImageInfo (created by OneapiDevice::
 * image_alloc) whose .data member holds the actual texel pointer, so this
 * software implementation has dimensions, wrap mode, filter and data type
 * available and emulates the CUDA sampler conventions. */

typedef uint64_t ccl_gpu_image_object_2D;

template<typename T>
ccl_device_forceinline T xelp_tex_fetch(const ccl_global KernelImageInfo &info, const int index)
{
  return reinterpret_cast<ccl_global T *>(info.data)[index];
}

ccl_device_inline int xelp_wrap_periodic(int x, const int width)
{
  x %= width;
  if (x < 0) {
    x += width;
  }
  return x;
}

ccl_device_inline int xelp_wrap_clamp(const int x, const int width)
{
  return clamp(x, 0, width - 1);
}

ccl_device_inline int xelp_wrap_mirror(const int x, const int width)
{
  const int m = abs(x + (x < 0)) % (2 * width);
  if (m >= width) {
    return 2 * width - m - 1;
  }
  return m;
}

/* Read a single texel with wrap applied. Returns zero outside the image for
 * CLIP extension. */
ccl_device_inline float4 xelp_image_read_texel(const ccl_global KernelImageInfo &info, int x, int y)
{
  const int width = info.width;
  const int height = info.height;

  switch (info.extension) {
    case EXTENSION_REPEAT:
      x = xelp_wrap_periodic(x, width);
      y = xelp_wrap_periodic(y, height);
      break;
    case EXTENSION_EXTEND:
      x = xelp_wrap_clamp(x, width);
      y = xelp_wrap_clamp(y, height);
      break;
    case EXTENSION_MIRROR:
      x = xelp_wrap_mirror(x, width);
      y = xelp_wrap_mirror(y, height);
      break;
    default:
      if (x < 0 || x >= width || y < 0 || y >= height) {
        return zero_float4();
      }
      break;
  }

  const int offset = x + width * y;

  switch (info.data_type) {
    case IMAGE_DATA_TYPE_FLOAT4:
      return xelp_tex_fetch<float4>(info, offset);
    case IMAGE_DATA_TYPE_BYTE4: {
      const uchar4 r = xelp_tex_fetch<uchar4>(info, offset);
      const float f = 1.0f / 255.0f;
      return make_float4(r.x * f, r.y * f, r.z * f, r.w * f);
    }
    case IMAGE_DATA_TYPE_USHORT4: {
      const ushort4 r = xelp_tex_fetch<ushort4>(info, offset);
      const float f = 1.0f / 65535.0f;
      return make_float4(r.x * f, r.y * f, r.z * f, r.w * f);
    }
    case IMAGE_DATA_TYPE_HALF4: {
      const half4 r = xelp_tex_fetch<half4>(info, offset);
      return make_float4(r.x, r.y, r.z, r.w);
    }
    case IMAGE_DATA_TYPE_FLOAT: {
      const float f = xelp_tex_fetch<float>(info, offset);
      return make_float4(f, f, f, 1.0f);
    }
    case IMAGE_DATA_TYPE_USHORT: {
      const ushort r = xelp_tex_fetch<ushort>(info, offset);
      const float f = r * (1.0f / 65535.0f);
      return make_float4(f, f, f, 1.0f);
    }
    case IMAGE_DATA_TYPE_HALF: {
      const float f = xelp_tex_fetch<half>(info, offset);
      return make_float4(f, f, f, 1.0f);
    }
    default: {
      const uchar r = xelp_tex_fetch<uchar>(info, offset);
      const float f = r * (1.0f / 255.0f);
      return make_float4(f, f, f, 1.0f);
    }
  }
}

ccl_device_inline float xelp_image_frac(const float x, ccl_private int *ix)
{
  const int i = float_to_int(x) - ((x < 0.0f) ? 1 : 0);
  *ix = i;
  return x - (float)i;
}

/* Sample with the filter mode the hardware sampler would have been created
 * with: nearest for CLOSEST interpolation, bilinear otherwise (matching the
 * CUDA -0.5 texel center convention the shared bicubic code compensates
 * for). Coordinates are normalized. */
ccl_device_inline float4 xelp_image_sample(const ccl_global KernelImageInfo &info,
                                           const float u,
                                           const float v)
{
  if (info.interpolation == INTERPOLATION_CLOSEST) {
    int ix, iy;
    xelp_image_frac(u * (float)info.width, &ix);
    xelp_image_frac(v * (float)info.height, &iy);
    return xelp_image_read_texel(info, ix, iy);
  }

  int ix, iy;
  const float tx = xelp_image_frac(u * (float)info.width - 0.5f, &ix);
  const float ty = xelp_image_frac(v * (float)info.height - 0.5f, &iy);

  float4 r = (1.0f - ty) * (1.0f - tx) * xelp_image_read_texel(info, ix, iy);
  r += (1.0f - ty) * tx * xelp_image_read_texel(info, ix + 1, iy);
  r += ty * (1.0f - tx) * xelp_image_read_texel(info, ix, iy + 1);
  r += ty * tx * xelp_image_read_texel(info, ix + 1, iy + 1);
  return r;
}

template<typename T>
ccl_device_forceinline T ccl_gpu_image_object_read_2D(const ccl_gpu_image_object_2D texobj,
                                                      const float x,
                                                      const float y)
{
  const ccl_global KernelImageInfo &info = *reinterpret_cast<const ccl_global KernelImageInfo *>(
      texobj);
  const float4 r = xelp_image_sample(info, x, y);
  if constexpr (sizeof(T) == sizeof(float4)) {
    return r;
  }
  else {
    return r.x;
  }
}

CCL_NAMESPACE_END
