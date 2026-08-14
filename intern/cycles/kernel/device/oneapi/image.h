/* SPDX-FileCopyrightText: 2021-2022 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0 */

CCL_NAMESPACE_BEGIN

/* For oneAPI implementation we do manual lookup and interpolation. */
/* TODO: share implementation with ../cpu/image.h. */

template<typename T> ccl_device_forceinline T tex_fetch(const KernelImageInfo &info, const int index)
{
  return reinterpret_cast<ccl_global T *>(info.data)[index];
}

ccl_device_inline int svm_image_texture_wrap_periodic(int x, int width)
{
  x %= width;
  if (x < 0) {
    x += width;
  }
  return x;
}

ccl_device_inline int svm_image_texture_wrap_clamp(const int x, const int width)
{
  return clamp(x, 0, width - 1);
}

ccl_device_inline int svm_image_texture_wrap_mirror(const int x, const int width)
{
  const int m = abs(x + (x < 0)) % (2 * width);
  if (m >= width) {
    return 2 * width - m - 1;
  }
  return m;
}

ccl_device_inline float4 svm_image_texture_read(const KernelImageInfo &info,
                                                const int x,
                                                int y,
                                                const int z)
{
  const int data_offset = x + info.width * y + info.width * info.height * z;
  const int texture_type = info.data_type;

  /* Float4 */
  if (texture_type == IMAGE_DATA_TYPE_FLOAT4) {
    return tex_fetch<float4>(info, data_offset);
  }
  /* Byte4 */
  if (texture_type == IMAGE_DATA_TYPE_BYTE4) {
    uchar4 r = tex_fetch<uchar4>(info, data_offset);
    float f = 1.0f / 255.0f;
    return make_float4(r.x * f, r.y * f, r.z * f, r.w * f);
  }
  /* Ushort4 */
  if (texture_type == IMAGE_DATA_TYPE_USHORT4) {
    ushort4 r = tex_fetch<ushort4>(info, data_offset);
    float f = 1.0f / 65535.f;
    return make_float4(r.x * f, r.y * f, r.z * f, r.w * f);
  }
  /* Float */
  if (texture_type == IMAGE_DATA_TYPE_FLOAT) {
    float f = tex_fetch<float>(info, data_offset);
    return make_float4(f, f, f, 1.0f);
  }
  /* UShort */
  if (texture_type == IMAGE_DATA_TYPE_USHORT) {
    ushort r = tex_fetch<ushort>(info, data_offset);
    float f = r * (1.0f / 65535.0f);
    return make_float4(f, f, f, 1.0f);
  }
  if (texture_type == IMAGE_DATA_TYPE_HALF) {
    float f = tex_fetch<half>(info, data_offset);
    return make_float4(f, f, f, 1.0f);
  }
  if (texture_type == IMAGE_DATA_TYPE_HALF4) {
    half4 r = tex_fetch<half4>(info, data_offset);
    return make_float4(r.x, r.y, r.z, r.w);
  }
  /* Byte */
  uchar r = tex_fetch<uchar>(info, data_offset);
  float f = r * (1.0f / 255.0f);
  return make_float4(f, f, f, 1.0f);
}

ccl_device_inline float4 svm_image_texture_read_2d(const int id, int x, int y)
{
  const KernelImageInfo &info = kernel_data_fetch(image_info, id);

  /* Wrap */
  if (info.extension == EXTENSION_REPEAT) {
    x = svm_image_texture_wrap_periodic(x, info.width);
    y = svm_image_texture_wrap_periodic(y, info.height);
  }
  else if (info.extension == EXTENSION_EXTEND) {
    x = svm_image_texture_wrap_clamp(x, info.width);
    y = svm_image_texture_wrap_clamp(y, info.height);
  }
  else if (info.extension == EXTENSION_MIRROR) {
    x = svm_image_texture_wrap_mirror(x, info.width);
    y = svm_image_texture_wrap_mirror(y, info.height);
  }
  else {
    if (x < 0 || x >= info.width || y < 0 || y >= info.height) {
      return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
  }

  return svm_image_texture_read(info, x, y, 0);
}


static float svm_image_texture_frac(const float x, int *ix)
{
  int i = float_to_int(x) - ((x < 0.0f) ? 1 : 0);
  *ix = i;
  return x - (float)i;
}

#define SET_CUBIC_SPLINE_WEIGHTS(u, t) \
  { \
    u[0] = (((-1.0f / 6.0f) * t + 0.5f) * t - 0.5f) * t + (1.0f / 6.0f); \
    u[1] = ((0.5f * t - 1.0f) * t) * t + (2.0f / 3.0f); \
    u[2] = ((-0.5f * t + 0.5f) * t + 0.5f) * t + (1.0f / 6.0f); \
    u[3] = (1.0f / 6.0f) * t * t * t; \
  } \
  (void)0

ccl_device float4 kernel_image_interp(KernelGlobals kg, const int id, float x, float y)
{
  const KernelImageInfo &info = kernel_data_fetch(image_info, id);

  if (info.interpolation == INTERPOLATION_CLOSEST) {
    /* Closest interpolation. */
    int ix, iy;
    svm_image_texture_frac(x * info.width, &ix);
    svm_image_texture_frac(y * info.height, &iy);

    return svm_image_texture_read_2d(id, ix, iy);
  }
  if (info.interpolation == INTERPOLATION_LINEAR) {
    /* Bilinear interpolation. */
    int ix, iy;
    float tx = svm_image_texture_frac(x * info.width - 0.5f, &ix);
    float ty = svm_image_texture_frac(y * info.height - 0.5f, &iy);

    float4 r;
    r = (1.0f - ty) * (1.0f - tx) * svm_image_texture_read_2d(id, ix, iy);
    r += (1.0f - ty) * tx * svm_image_texture_read_2d(id, ix + 1, iy);
    r += ty * (1.0f - tx) * svm_image_texture_read_2d(id, ix, iy + 1);
    r += ty * tx * svm_image_texture_read_2d(id, ix + 1, iy + 1);
    return r;
  }
  /* Bicubic interpolation. */
  int ix, iy;
  float tx = svm_image_texture_frac(x * info.width - 0.5f, &ix);
  float ty = svm_image_texture_frac(y * info.height - 0.5f, &iy);

  float u[4], v[4];
  SET_CUBIC_SPLINE_WEIGHTS(u, tx);
  SET_CUBIC_SPLINE_WEIGHTS(v, ty);

  float4 r = make_float4(0.0f, 0.0f, 0.0f, 0.0f);

  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      float weight = u[x] * v[y];
      r += weight * svm_image_texture_read_2d(id, ix + x - 1, iy + y - 1);
    }
  }
  return r;
}



#undef SET_CUBIC_SPLINE_WEIGHTS

CCL_NAMESPACE_END
