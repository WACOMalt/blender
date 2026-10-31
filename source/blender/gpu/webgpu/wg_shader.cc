/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <sstream>

#include "BLI_math_base.hh"
#include "BLI_set.hh"
#include "BLI_string.hh"

#include "BKE_global.hh"

#include "CLG_log.h"

#include "GPU_capabilities.hh"
#include "gpu_shader_dependency_private.hh"

#include "web_shader_compiler.h"

#include "wg_backend.hh"
#include "wg_common.hh"
#include "wg_context.hh"
#include "wg_shader.hh"

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.webgpu"};

using namespace blender::gpu::shader;

/* -------------------------------------------------------------------- */
/** \name Pipeline Keys
 * \{ */

bool WGRenderPipelineKey::operator==(const WGRenderPipelineKey &other) const
{
  return memcmp(this, &other, sizeof(WGRenderPipelineKey)) == 0;
}

uint64_t WGRenderPipelineKey::hash() const
{
  return wg_hash_bytes(this, sizeof(WGRenderPipelineKey));
}

uint64_t WGVertexLayout::hash() const
{
  const uint64_t hash = wg_hash_bytes(buffers.data(),
                                      buffers.size() * sizeof(WGVertexBufferLayout));
  return hash * 33 ^
         wg_hash_bytes(attributes.data(), attributes.size() * sizeof(WGVertexAttribute));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GLSL Generation
 *
 * Adapted from the Vulkan back-end.
 * \{ */

static const char *to_string(const Interpolation &interp)
{
  switch (interp) {
    case Interpolation::SMOOTH:
      return "smooth";
    case Interpolation::FLAT:
      return "flat";
    case Interpolation::NO_PERSPECTIVE:
      return "noperspective";
    default:
      return "unknown";
  }
}

static const char *to_string(const Type &type)
{
  switch (type) {
    case Type::float_t:
      return "float";
    case Type::float2_t:
      return "vec2";
    case Type::float3_t:
      return "vec3";
    case Type::float4_t:
      return "vec4";
    case Type::float3x3_t:
      return "mat3";
    case Type::float4x4_t:
      return "mat4";
    case Type::uint_t:
      return "uint";
    case Type::uint2_t:
      return "uvec2";
    case Type::uint3_t:
      return "uvec3";
    case Type::uint4_t:
      return "uvec4";
    case Type::int_t:
      return "int";
    case Type::int2_t:
      return "ivec2";
    case Type::int3_t:
      return "ivec3";
    case Type::int4_t:
      return "ivec4";
    case Type::bool_t:
      return "bool";
    default:
      return "unknown";
  }
}

static const char *to_string(const TextureFormat &type)
{
  switch (type) {
    case TextureFormat::UINT_8_8_8_8:
      return "rgba8ui";
    case TextureFormat::SINT_8_8_8_8:
      return "rgba8i";
    case TextureFormat::UNORM_8_8_8_8:
      return "rgba8";
    case TextureFormat::UINT_32_32_32_32:
      return "rgba32ui";
    case TextureFormat::SINT_32_32_32_32:
      return "rgba32i";
    case TextureFormat::SFLOAT_32_32_32_32:
      return "rgba32f";
    case TextureFormat::UINT_16_16_16_16:
      return "rgba16ui";
    case TextureFormat::SINT_16_16_16_16:
      return "rgba16i";
    case TextureFormat::SFLOAT_16_16_16_16:
      return "rgba16f";
    case TextureFormat::UNORM_16_16_16_16:
      return "rgba16";
    case TextureFormat::UINT_8_8:
      return "rg8ui";
    case TextureFormat::SINT_8_8:
      return "rg8i";
    case TextureFormat::UNORM_8_8:
      return "rg8";
    case TextureFormat::UINT_32_32:
      return "rg32ui";
    case TextureFormat::SINT_32_32:
      return "rg32i";
    case TextureFormat::SFLOAT_32_32:
      return "rg32f";
    case TextureFormat::UINT_16_16:
      return "rg16ui";
    case TextureFormat::SINT_16_16:
      return "rg16i";
    case TextureFormat::SFLOAT_16_16:
      return "rg16f";
    case TextureFormat::UNORM_16_16:
      return "rg16";
    case TextureFormat::UINT_8:
      return "r8ui";
    case TextureFormat::SINT_8:
      return "r8i";
    case TextureFormat::UNORM_8:
      return "r8";
    case TextureFormat::UINT_32:
      return "r32ui";
    case TextureFormat::SINT_32:
      return "r32i";
    case TextureFormat::SFLOAT_32:
      return "r32f";
    case TextureFormat::UINT_16:
      return "r16ui";
    case TextureFormat::SINT_16:
      return "r16i";
    case TextureFormat::SFLOAT_16:
      return "r16f";
    case TextureFormat::UNORM_16:
      return "r16";
    case TextureFormat::UFLOAT_11_11_10:
      return "r11f_g11f_b10f";
    case TextureFormat::UNORM_10_10_10_2:
      return "rgb10_a2";
    default:
      return "unknown";
  }
}

static const char *to_string(const DepthWrite &value)
{
  switch (value) {
    case DepthWrite::ANY:
      return "depth_any";
    case DepthWrite::GREATER:
      return "depth_greater";
    case DepthWrite::LESS:
      return "depth_less";
    default:
      return "depth_unchanged";
  }
}

static void print_image_type(std::ostream &os,
                             const ImageType &type,
                             const ShaderCreateInfo::Resource::BindType bind_type)
{
  switch (type) {
    case ImageType::IntBuffer:
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Int3D:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicInt3D:
      os << "i";
      break;
    case ImageType::UintBuffer:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Uint3D:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint2DArray:
    case ImageType::AtomicUint3D:
      os << "u";
      break;
    default:
      break;
  }

  if (bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
    os << "image";
  }
  else {
    os << "sampler";
  }

  switch (type) {
    case ImageType::FloatBuffer:
    case ImageType::IntBuffer:
    case ImageType::UintBuffer:
      os << "Buffer";
      break;
    case ImageType::Float1D:
    case ImageType::Float1DArray:
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
      os << "1D";
      break;
    case ImageType::Float2D:
    case ImageType::Float2DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Shadow2D:
    case ImageType::Shadow2DArray:
    case ImageType::Depth2D:
    case ImageType::Depth2DArray:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint2DArray:
      os << "2D";
      break;
    case ImageType::Float3D:
    case ImageType::Int3D:
    case ImageType::AtomicInt3D:
    case ImageType::Uint3D:
    case ImageType::AtomicUint3D:
      os << "3D";
      break;
    case ImageType::FloatCube:
    case ImageType::FloatCubeArray:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
    case ImageType::ShadowCube:
    case ImageType::ShadowCubeArray:
    case ImageType::DepthCube:
    case ImageType::DepthCubeArray:
      os << "Cube";
      break;
    default:
      break;
  }

  switch (type) {
    case ImageType::Float1DArray:
    case ImageType::Float2DArray:
    case ImageType::FloatCubeArray:
    case ImageType::Int1DArray:
    case ImageType::Int2DArray:
    case ImageType::IntCubeArray:
    case ImageType::Uint1DArray:
    case ImageType::Uint2DArray:
    case ImageType::UintCubeArray:
    case ImageType::Shadow2DArray:
    case ImageType::ShadowCubeArray:
    case ImageType::Depth2DArray:
    case ImageType::DepthCubeArray:
    case ImageType::AtomicUint2DArray:
      os << "Array";
      break;
    default:
      break;
  }

  switch (type) {
    case ImageType::Shadow2D:
    case ImageType::Shadow2DArray:
    case ImageType::ShadowCube:
    case ImageType::ShadowCubeArray:
      os << "Shadow";
      break;
    default:
      break;
  }
  os << " ";
}

static std::ostream &print_qualifier(std::ostream &os,
                                     const Qualifier &qualifiers,
                                     const bool is_buffer)
{
  /* No `restrict`, it is only an optimization hint and Tint doesn't support it.
   * WGSL has no write only storage buffers, they are read-write. */
  if (!flag_is_set(qualifiers, Qualifier::read) && !is_buffer) {
    os << "writeonly ";
  }
  if (!flag_is_set(qualifiers, Qualifier::write)) {
    os << "readonly ";
  }
  return os;
}

/** Name of a resource, used in the macros of the vertex stage aliases. */
static std::string resource_name(const ShaderCreateInfo::Resource &res)
{
  switch (res.bind_type) {
    case ShaderCreateInfo::Resource::BindType::IMAGE:
      return res.image.name;
    case ShaderCreateInfo::Resource::BindType::SAMPLER:
      return res.sampler.name;
    case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
      return res.storagebuf.name.str_no_array();
    default:
      return "";
  }
}

static bool wg_is_identifier_char(char c);

/* -------------------------------------------------------------------- */
/** \name Emulated Images
 *
 * See #WGImageMode. Images bound as textures get `NAME_wg_imageLoad()` helpers that fetch the
 * texture. Images emulated with a buffer are declared as a storage buffer of 32 bit words holding
 * the bits of the texture format (#WGTexelLayout), with helpers for the image functions and the
 * size of the image in the push constants (`NAME_wg_meta`). The device copies the texture to
 * the buffer and back when the texture is used (#WGCmdSyncBufferImage).
 * \{ */

/** Component type of the image (float, int, uint) as the GLSL vector prefix. */
static const char *wg_image_vector_prefix(const ImageType type)
{
  std::stringstream ss;
  print_image_type(ss, type, ShaderCreateInfo::Resource::BindType::IMAGE);
  const std::string str = ss.str();
  return str[0] == 'i' ? "i" : str[0] == 'u' ? "u" : "";
}

/** Dimension of the image coordinates (1, 2 or 3). */
static int wg_image_coord_size(const ImageType type)
{
  switch (type) {
    case ImageType::Float1D:
    case ImageType::Int1D:
    case ImageType::Uint1D:
    case ImageType::FloatBuffer:
    case ImageType::IntBuffer:
    case ImageType::UintBuffer:
      return 1;
    case ImageType::Float2D:
    case ImageType::Int2D:
    case ImageType::Uint2D:
    case ImageType::Float1DArray:
    case ImageType::Int1DArray:
    case ImageType::Uint1DArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicInt2D:
      return 2;
    default:
      return 3;
  }
}

static std::string wg_ivec(const int size)
{
  return size == 1 ? "int" : size == 2 ? "ivec2" : "ivec3";
}

/**
 * GLSL packing the value `v` (a 4 component vector) into the words `w[0..words-1]`, and
 * unpacking the words into `v`, for the texel layout.
 */
static void wg_texel_pack_glsl(const WGTexelLayout &layout, std::string &r_pack, std::string &r_unpack)
{
  using Kind = WGTexelLayout::Kind;
  const int n = layout.components;
  std::string pack, unpack;
  auto comp = [](const int i) { return std::string(1, "xyzw"[i]); };
  switch (layout.kind) {
    case Kind::Float32:
      for (int i = 0; i < n; i++) {
        pack += "w[" + std::to_string(i) + "] = floatBitsToUint(v." + comp(i) + ");";
        unpack += "v." + comp(i) + " = uintBitsToFloat(w[" + std::to_string(i) + "]);";
      }
      break;
    case Kind::Uint32:
      for (int i = 0; i < n; i++) {
        pack += "w[" + std::to_string(i) + "] = v." + comp(i) + ";";
        unpack += "v." + comp(i) + " = w[" + std::to_string(i) + "];";
      }
      break;
    case Kind::Sint32:
      for (int i = 0; i < n; i++) {
        pack += "w[" + std::to_string(i) + "] = uint(v." + comp(i) + ");";
        unpack += "v." + comp(i) + " = int(w[" + std::to_string(i) + "]);";
      }
      break;
    case Kind::Float16:
    case Kind::Unorm16:
    case Kind::Snorm16: {
      const std::string fn = layout.kind == Kind::Float16 ? "Half2x16" :
                             layout.kind == Kind::Unorm16 ? "Unorm2x16" :
                                                            "Snorm2x16";
      for (int i = 0; i < n; i += 2) {
        const std::string second = (i + 1 < n) ? "v." + comp(i + 1) : "0.0";
        const std::string word = "w[" + std::to_string(i / 2) + "]";
        pack += word + " = pack" + fn + "(vec2(v." + comp(i) + ", " + second + "));";
        unpack += "{ vec2 u = unpack" + fn + "(" + word + "); v." + comp(i) + " = u.x;";
        if (i + 1 < n) {
          unpack += " v." + comp(i + 1) + " = u.y;";
        }
        unpack += " }";
      }
      break;
    }
    case Kind::Uint16:
    case Kind::Sint16:
      for (int i = 0; i < n; i += 2) {
        const std::string word = "w[" + std::to_string(i / 2) + "]";
        const std::string second = (i + 1 < n) ? "(uint(v." + comp(i + 1) + ") << 16u)" : "0u";
        pack += word + " = (uint(v." + comp(i) + ") & 0xFFFFu) | " + second + ";";
        if (layout.kind == Kind::Uint16) {
          unpack += "v." + comp(i) + " = " + word + " & 0xFFFFu;";
          if (i + 1 < n) {
            unpack += "v." + comp(i + 1) + " = " + word + " >> 16u;";
          }
        }
        else {
          unpack += "v." + comp(i) + " = (int(" + word + " << 16u) >> 16);";
          if (i + 1 < n) {
            unpack += "v." + comp(i + 1) + " = (int(" + word + ") >> 16);";
          }
        }
      }
      break;
    case Kind::Unorm8:
    case Kind::Snorm8: {
      const char *fn = layout.kind == Kind::Unorm8 ? "Unorm4x8" : "Snorm4x8";
      std::string vec = "vec4(";
      for (int i = 0; i < 4; i++) {
        vec += (i < n ? "v." + comp(i) : std::string("0.0")) + (i < 3 ? ", " : ")");
      }
      pack += "w[0] = pack" + std::string(fn) + "(" + vec + ");";
      unpack += "{ vec4 u = unpack" + std::string(fn) + "(w[0]);";
      for (int i = 0; i < n; i++) {
        unpack += " v." + comp(i) + " = u." + comp(i) + ";";
      }
      unpack += " }";
      break;
    }
    case Kind::Uint8:
    case Kind::Sint8:
      pack += "w[0] = 0u;";
      for (int i = 0; i < n; i++) {
        const std::string shift = std::to_string(i * 8) + "u";
        pack += "w[0] |= (uint(v." + comp(i) + ") & 0xFFu) << " + shift + ";";
        if (layout.kind == Kind::Uint8) {
          unpack += "v." + comp(i) + " = (w[0] >> " + shift + ") & 0xFFu;";
        }
        else {
          unpack += "v." + comp(i) + " = (int(w[0] << " + std::to_string(24 - i * 8) + "u) >> 24);";
        }
      }
      break;
    case Kind::UFloat11_11_10:
      /* Half floats share the exponent bias, drop the low mantissa bits. */
      pack += "{ uint h0 = packHalf2x16(vec2(max(v.x, 0.0), max(v.y, 0.0)));";
      pack += " uint h2 = packHalf2x16(vec2(max(v.z, 0.0), 0.0));";
      pack += " w[0] = ((h0 >> 4u) & 0x7FFu) | (((h0 >> 20u) & 0x7FFu) << 11u) | (((h2 >> 6u) & 0x3FFu) << 22u); }";
      unpack += "{ vec2 u = unpackHalf2x16(((w[0] & 0x7FFu) << 4u) | (((w[0] >> 11u) & 0x7FFu) << 20u));";
      unpack += " v.x = u.x; v.y = u.y; v.z = unpackHalf2x16((w[0] >> 22u) << 6u).x; }";
      break;
    case Kind::Unorm10_10_10_2:
      pack += "{ uvec4 q = uvec4(round(clamp(v, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0)));";
      pack += " w[0] = q.x | (q.y << 10u) | (q.z << 20u) | (q.w << 30u); }";
      unpack += "v = vec4(w[0] & 1023u, (w[0] >> 10u) & 1023u, (w[0] >> 20u) & 1023u, w[0] >> 30u) / vec4(1023.0, 1023.0, 1023.0, 3.0);";
      break;
    case Kind::Uint10_10_10_2:
      pack += "w[0] = (v.x & 1023u) | ((v.y & 1023u) << 10u) | ((v.z & 1023u) << 20u) | ((v.w & 3u) << 30u);";
      unpack += "v = uvec4(w[0] & 1023u, (w[0] >> 10u) & 1023u, (w[0] >> 20u) & 1023u, w[0] >> 30u);";
      break;
  }
  r_pack = pack;
  r_unpack = unpack;
}

/**
 * Declaration of an image emulated with a buffer and the helpers replacing the image
 * functions (see #wg_rewrite_image_calls). Lines are macro continuations.
 */
static void print_buffer_image(std::ostream &os,
                               const uint32_t binding,
                               const int vertex_alias_binding,
                               const ImageType image_type,
                               const Qualifier qualifiers,
                               const TextureFormat image_format,
                               const std::string &name,
                               const bool is_sampler)
{
  WGTexelLayout layout;
  if (!wg_texel_layout_get(image_format, layout)) {
    BLI_assert_unreachable();
    return;
  }
  const std::string buf = name + "_wg_buf";
  const std::string meta = name + "_wg_meta";
  const std::string prefix = wg_image_vector_prefix(image_type);
  const std::string vec4 = prefix + "vec4";
  const int coord_size = wg_image_coord_size(image_type);
  const std::string coord = wg_ivec(coord_size);
  /* The buffer is `int` for atomics on signed images, `uint` otherwise. */
  const std::string elem = layout.kind == WGTexelLayout::Kind::Sint32 ? "int" : "uint";
  const bool writable = flag_is_set(qualifiers, Qualifier::write);
  const std::string nl = " \\\n";

  if (vertex_alias_binding >= 0) {
    os << "layout(binding = wg_binding_" << name << ", std430) wg_qualifier_" << name;
  }
  else {
    os << "layout(binding = " << binding << ", std430) " << (writable ? "" : "readonly ");
  }
  os << " buffer _" << buf << " { " << elem << " " << buf << "[]; };" << nl;

  /* Index of the first word of a texel. */
  os << "uint " << name << "_wg_index(" << coord << " co) { return (uint(co.x)";
  if (coord_size >= 2) {
    os << " + uint(co.y) * " << meta << ".x";
  }
  if (coord_size >= 3) {
    os << " + uint(co.z) * " << meta << ".x * " << meta << ".y";
  }
  os << ") * " << layout.words << "u; }" << nl;
  /* Size. */
  os << coord << " " << name << "_wg_" << (is_sampler ? "textureSize(int lod)" : "imageSize()")
     << " { return " << coord << "(" << meta << (coord_size == 1 ? ".x" :
                                                 coord_size == 2 ? ".xy" :
                                                                   ".xyz")
     << "); }" << nl;

  std::string pack, unpack;
  wg_texel_pack_glsl(layout, pack, unpack);
  /* Load. */
  os << vec4 << " " << name << "_wg_" << (is_sampler ? "texelFetch" : "imageLoad") << "(" << coord
     << " co" << (is_sampler ? ", int lod" : "") << ") { uint i = " << name
     << "_wg_index(co); uint w[" << layout.words << "];";
  for (int k = 0; k < layout.words; k++) {
    os << " w[" << k << "] = uint(" << buf << "[i + " << k << "u]);";
  }
  os << " " << vec4 << " v = " << vec4 << "(0, 0, 0, 1); " << unpack << " return v; }" << nl;
  /* Store. */
  if (writable && !is_sampler) {
    os << "void " << name << "_wg_imageStore(" << coord << " co, " << vec4 << " v) { uint i = "
       << name << "_wg_index(co); uint w[" << layout.words << "]; " << pack;
    for (int k = 0; k < layout.words; k++) {
      os << " " << buf << "[i + " << k << "u] = " << elem << "(w[" << k << "]);";
    }
    os << " }" << nl;
  }
  /* Atomics, on 32 bit integer images. */
  if (writable && !is_sampler && layout.words == 1 &&
      ELEM(layout.kind, WGTexelLayout::Kind::Uint32, WGTexelLayout::Kind::Sint32))
  {
    for (const char *op : {"Add", "Min", "Max", "And", "Or", "Xor", "Exchange"}) {
      os << elem << " " << name << "_wg_imageAtomic" << op << "(" << coord << " co, " << elem
         << " v) { return atomic" << op << "(" << buf << "[" << name << "_wg_index(co)], v); }"
         << nl;
    }
    os << elem << " " << name << "_wg_imageAtomicCompSwap(" << coord << " co, " << elem
       << " c, " << elem << " v) { return atomicCompSwap(" << buf << "[" << name
       << "_wg_index(co)], c, v); }" << nl;
  }
}

/** Declaration of an image bound as a texture and the helpers fetching it. */
static void print_sampled_image(std::ostream &os,
                                const uint32_t binding,
                                const ImageType image_type,
                                const std::string &name)
{
  const std::string prefix = wg_image_vector_prefix(image_type);
  const int coord_size = wg_image_coord_size(image_type);
  const std::string coord = wg_ivec(coord_size);
  const std::string nl = " \\\n";
  os << "layout(binding = " << binding << ") uniform ";
  print_image_type(os, image_type, ShaderCreateInfo::Resource::BindType::SAMPLER);
  os << name << ";" << nl;
  os << prefix << "vec4 " << name << "_wg_imageLoad(" << coord << " co) { return texelFetch("
     << name << ", co, 0); }" << nl;
  os << coord << " " << name << "_wg_imageSize() { return textureSize(" << name << ", 0); }"
     << nl;
}

/**
 * Replace `function(NAME, args...)` by `NAME_wg_function(args...)` for the given names
 * (any whitespace allowed inside the parentheses).
 */
static void wg_rewrite_image_calls(std::string &glsl,
                                   const StringRef function,
                                   const Set<std::string> &names,
                                   const StringRef helper)
{
  const std::string pattern = function + "(";
  size_t pos = 0;
  while ((pos = glsl.find(pattern, pos)) != std::string::npos) {
    if (pos > 0 && wg_is_identifier_char(glsl[pos - 1])) {
      pos += pattern.size();
      continue;
    }
    size_t arg_start = pos + pattern.size();
    while (arg_start < glsl.size() && isspace(glsl[arg_start])) {
      arg_start++;
    }
    size_t arg_end = arg_start;
    while (arg_end < glsl.size() && wg_is_identifier_char(glsl[arg_end])) {
      arg_end++;
    }
    size_t next = arg_end;
    while (next < glsl.size() && isspace(glsl[next])) {
      next++;
    }
    const std::string name = glsl.substr(arg_start, arg_end - arg_start);
    if (next < glsl.size() && ELEM(glsl[next], ',', ')') && names.contains(name)) {
      /* Skip the comma and the spaces after it. */
      size_t args = next;
      if (glsl[args] == ',') {
        args++;
        while (args < glsl.size() && isspace(glsl[args])) {
          args++;
        }
      }
      const std::string replacement = name + "_wg_" + helper + "(";
      glsl.replace(pos, args - pos, replacement);
      pos += replacement.size();
      continue;
    }
    pos += pattern.size();
  }
}

/** \} */

static void print_resource(std::ostream &os,
                           const uint32_t binding,
                           const int vertex_alias_binding,
                           const ShaderCreateInfo::Resource &res)
{
  /* Resources with a vertex stage alias take their binding and qualifier from the macros
   * defined by #WGShader::resources_declare. */
  const bool use_macros = vertex_alias_binding >= 0;
  const std::string name = resource_name(res);
  if (ELEM(res.bind_type,
           ShaderCreateInfo::Resource::BindType::IMAGE,
           ShaderCreateInfo::Resource::BindType::SAMPLER))
  {
    const WGImageMode mode = WGShaderInterface::image_mode(res);
    if (mode == WGImageMode::Buffer) {
      const bool is_sampler = res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER;
      const ImageType type = is_sampler ? res.sampler.type : res.image.type;
      const bool is_signed = ELEM(
          type, ImageType::AtomicInt2D, ImageType::AtomicInt2DArray, ImageType::AtomicInt3D);
      const bool is_atomic = ELEM(type,
                                  ImageType::AtomicUint2D,
                                  ImageType::AtomicUint2DArray,
                                  ImageType::AtomicUint3D,
                                  ImageType::AtomicInt2D,
                                  ImageType::AtomicInt2DArray,
                                  ImageType::AtomicInt3D);
      const TextureFormat format = is_atomic ?
                                       (is_signed ? TextureFormat::SINT_32 :
                                                    TextureFormat::UINT_32) :
                                       wg_device_format(TextureFormat(res.image.format));
      print_buffer_image(os,
                         binding,
                         vertex_alias_binding,
                         type,
                         is_sampler ? Qualifier::read : res.image.qualifiers,
                         format,
                         is_sampler ? res.sampler.name : res.image.name,
                         is_sampler);
      return;
    }
    if (mode == WGImageMode::Sampled) {
      print_sampled_image(os, binding, res.image.type, res.image.name);
      return;
    }
  }
  if (use_macros) {
    os << "layout(binding = wg_binding_" << name;
  }
  else {
    os << "layout(binding = " << binding;
  }
  if (res.bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
    os << ", " << to_string(res.image.format);
  }
  else if (res.bind_type == ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER) {
    os << ", std140";
  }
  else if (res.bind_type == ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER) {
    os << ", std430";
  }
  os << ") ";

  switch (res.bind_type) {
    case ShaderCreateInfo::Resource::BindType::SAMPLER:
      os << "uniform ";
      print_image_type(os, res.sampler.type, res.bind_type);
      os << res.sampler.name << ";";
      break;
    case ShaderCreateInfo::Resource::BindType::IMAGE:
      os << "uniform ";
      if (use_macros) {
        os << "wg_qualifier_" << name << " ";
      }
      else {
        print_qualifier(os, res.image.qualifiers, false);
      }
      print_image_type(os, res.image.type, res.bind_type);
      os << res.image.name << ";";
      break;
    case ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER:
      os << "uniform _" << res.uniformbuf.name.str_no_array() << " { " << res.uniformbuf.type_name
         << " " << res.uniformbuf.name << "; };";
      break;
    case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
      if (use_macros) {
        os << "wg_qualifier_" << name << " ";
      }
      else {
        print_qualifier(os, res.storagebuf.qualifiers, true);
      }
      os << "buffer _" << res.storagebuf.name.str_no_array() << " { " << res.storagebuf.type_name
         << " " << res.storagebuf.name << "; };";
      break;
    case ShaderCreateInfo::Resource::BindType::ACCELERATION_STRUCTURE:
      /* Not supported. */
      break;
  }
}

static void print_resource(std::ostream &os,
                           const WGShaderInterface &shader_interface,
                           const ShaderCreateInfo::Resource &res,
                           StringRefNull res_frequency,
                           StringRefNull &active_info_name)
{
  if (assign_if_different(active_info_name, res.info_name)) {
    os << "\n#define CREATE_INFO_RES_" << res_frequency << "_" << res.info_name << " \\\n";
  }
  print_resource(
      os, shader_interface.binding_get(res), shader_interface.vertex_alias_binding_get(res), res);
  os << " \\\n";
}

/**
 * Macros giving writable resources a read-only alias binding in the vertex stage (WebGPU
 * doesn't allow writable storage there). Preprocessor directives can't be inside the
 * `CREATE_INFO_RES_` macros, so they are defined before them.
 */
static void print_vertex_alias_macros(std::ostream &os,
                                      const WGShaderInterface &shader_interface,
                                      const ShaderCreateInfo::Resource &res)
{
  const int alias = shader_interface.vertex_alias_binding_get(res);
  if (alias < 0) {
    return;
  }
  std::stringstream qualifier;
  if (res.bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
    if (WGShaderInterface::image_mode(res) == WGImageMode::Buffer) {
      /* The buffer is read by the helpers even for write-only images. */
      qualifier << (flag_is_set(res.image.qualifiers, Qualifier::write) ? "" : "readonly");
    }
    else {
      print_qualifier(qualifier, res.image.qualifiers, false);
    }
  }
  else if (res.bind_type == ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER) {
    print_qualifier(qualifier, res.storagebuf.qualifiers, true);
  }
  const std::string name = resource_name(res);
  os << "#ifdef GPU_VERTEX_SHADER\n";
  os << "#define wg_binding_" << name << " " << alias << "\n";
  os << "#define wg_qualifier_" << name << " readonly\n";
  os << "#else\n";
  os << "#define wg_binding_" << name << " " << shader_interface.binding_get(res) << "\n";
  os << "#define wg_qualifier_" << name << " " << qualifier.str() << "\n";
  os << "#endif\n";
}

inline int get_location_count(const Type &type)
{
  if (type == shader::Type::float4x4_t) {
    return 4;
  }
  if (type == shader::Type::float3x3_t) {
    return 3;
  }
  return 1;
}

static void print_interface_as_attributes(std::ostream &os,
                                          const std::string &prefix,
                                          const StageInterfaceInfo &iface,
                                          int &location)
{
  for (const StageInterfaceInfo::InOut &inout : iface.inouts) {
    os << "layout(location=" << location << ") " << prefix << " " << to_string(inout.interp) << " "
       << to_string(inout.type) << " " << inout.name << ";\n";
    location += get_location_count(inout.type);
  }
}

static void print_interface_as_struct(std::ostream &os,
                                      const std::string &prefix,
                                      const StageInterfaceInfo &iface,
                                      int &location)
{
  std::string struct_name = prefix + iface.name;
  Interpolation qualifier = iface.inouts[0].interp;

  os << "struct " << struct_name << " {\n";
  for (const StageInterfaceInfo::InOut &inout : iface.inouts) {
    os << "  " << to_string(inout.type) << " " << inout.name << ";\n";
  }
  os << "};\n";
  os << "layout(location=" << location << ") " << prefix << " " << to_string(qualifier) << " "
     << struct_name << " " << iface.instance_name << ";\n";

  for (const StageInterfaceInfo::InOut &inout : iface.inouts) {
    location += get_location_count(inout.type);
  }
}

static void print_interface(std::ostream &os,
                            const std::string &prefix,
                            const StageInterfaceInfo &iface,
                            int &location)
{
  if (iface.instance_name.is_empty()) {
    print_interface_as_attributes(os, prefix, iface, location);
  }
  else {
    print_interface_as_struct(os, prefix, iface, location);
  }
}

static std::string main_function_wrapper(std::string &pre_main, std::string &post_main)
{
  std::stringstream ss;
  /* Prototype for the original main. */
  ss << "\n";
  ss << "void main_function_();\n";
  /* Wrapper to the main function in order to inject code processing on globals. */
  ss << "void main() {\n";
  ss << pre_main;
  ss << "  main_function_();\n";
  ss << post_main;
  ss << "}\n";
  /* Rename the original main. */
  ss << "#define main main_function_\n";
  ss << "\n";
  return ss.str();
}

std::string WGShader::resources_declare(const ShaderCreateInfo &info) const
{
  const WGShaderInterface &wg_interface = interface_get();
  std::stringstream ss;

  ss << "\n#line " << __LINE__ << " \"" << __FILE__ << "\"\n";

  /* Specialization Constants, they become WGSL overrides. */
  uint constant_id = 0;
  for (const SpecializationConstant &sc : info.specialization_constants_) {
    ss << "layout (constant_id=" << constant_id++ << ") const ";
    switch (sc.type) {
      case Type::int_t:
        ss << "int " << sc.name << "=" << std::to_string(sc.value.i) << ";\n";
        break;
      case Type::uint_t:
        ss << "uint " << sc.name << "=" << std::to_string(sc.value.u) << "u;\n";
        break;
      case Type::bool_t:
        ss << "bool " << sc.name << "=" << (sc.value.u ? "true" : "false") << ";\n";
        break;
      case Type::float_t:
        /* Use uint representation to allow exact same bit pattern even if NaN. */
        ss << "uint " << sc.name << "_uint=" << std::to_string(sc.value.u) << "u;\n";
        ss << "#define " << sc.name << " uintBitsToFloat(" << sc.name << "_uint)\n";
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  }

  /* Compilation Constants (pass-through). */
  for (const CompilationConstant &sc : info.compilation_constants_) {
    ss << "const ";
    switch (sc.type) {
      case Type::int_t:
        ss << "int " << sc.name << "=" << std::to_string(sc.value.i) << ";\n";
        break;
      case Type::uint_t:
        ss << "uint " << sc.name << "=" << std::to_string(sc.value.u) << "u;\n";
        break;
      case Type::bool_t:
        ss << "bool " << sc.name << "=" << (sc.value.u ? "true" : "false") << ";\n";
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  }

  for (const Span<ShaderCreateInfo::Resource> resources :
       {info.pass_resources_.as_span(),
        info.batch_resources_.as_span(),
        info.geometry_resources_.as_span()})
  {
    for (const ShaderCreateInfo::Resource &res : resources) {
      print_vertex_alias_macros(ss, wg_interface, res);
    }
  }
  {
    StringRefNull active_info = "";
    for (const ShaderCreateInfo::SharedVariable &sv : info.shared_variables_) {
      if (assign_if_different(active_info, sv.info_name)) {
        ss << "\n#define CREATE_INFO_RES_SHARED_VARS_" << sv.info_name << " \\\n";
      }
      ss << "shared " << to_string(sv.type) << " " << sv.name << ";";
      ss << " \\\n";
    }
    ss << "\n";
  }
  {
    StringRefNull active_info = "";
    for (const ShaderCreateInfo::Resource &res : info.pass_resources_) {
      print_resource(ss, wg_interface, res, "PASS", active_info);
    }
    ss << "\n";
  }
  {
    StringRefNull active_info = "";
    for (const ShaderCreateInfo::Resource &res : info.batch_resources_) {
      print_resource(ss, wg_interface, res, "BATCH", active_info);
    }
    ss << "\n";
  }
  {
    StringRefNull active_info = "";
    for (const ShaderCreateInfo::Resource &res : info.geometry_resources_) {
      print_resource(ss, wg_interface, res, "GEOMETRY", active_info);
    }
    ss << "\n";
  }
  /* Push constants, stored in a uniform buffer. */
  if (!info.push_constants_.is_empty() || wg_interface.has_buffer_images()) {
    ss << "layout(binding = " << wg_interface.push_constants_binding_get()
       << ", std140) uniform constants\n";
    ss << "{\n";
    for (const ShaderCreateInfo::PushConst &uniform : info.push_constants_) {
      ss << "  " << to_string(uniform.type) << " " << uniform.name;
      if (uniform.array_size > 0) {
        ss << "[" << uniform.array_size << "]";
      }
      ss << ";\n";
    }
    /* Sizes of the emulated images, in the order of #WGShaderInterface::resources(). */
    for (const WGResourceBinding &resource : wg_interface.resources()) {
      if (resource.type != WGResourceBinding::Type::BufferImage || resource.vertex_alias) {
        continue;
      }
      for (const Span<ShaderCreateInfo::Resource> resources :
           {info.pass_resources_.as_span(),
            info.batch_resources_.as_span(),
            info.geometry_resources_.as_span()})
      {
        for (const ShaderCreateInfo::Resource &res : resources) {
          const bool is_sampler = res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER;
          if (res.slot == resource.slot && is_sampler == resource.sampler_slot &&
              ELEM(res.bind_type,
                   ShaderCreateInfo::Resource::BindType::IMAGE,
                   ShaderCreateInfo::Resource::BindType::SAMPLER) &&
              WGShaderInterface::image_mode(res) == WGImageMode::Buffer)
          {
            ss << "  uvec4 " << resource_name(res) << "_wg_meta;\n";
          }
        }
      }
    }
    ss << "};\n";
  }

  ss << "\n";
  return ss.str();
}

/** Number of locations used by the outputs of the vertex shader. */
static int wg_varying_locations(const ShaderCreateInfo &info)
{
  int location = 0;
  for (const StageInterfaceInfo *iface : info.vertex_out_interfaces_) {
    for (const StageInterfaceInfo::InOut &inout : iface->inouts) {
      location += get_location_count(inout.type);
    }
  }
  if (flag_is_set(info.builtins_combined(), BuiltinBits::CLIP_DISTANCES)) {
    location += 6;
  }
  return location;
}

std::string WGShader::vertex_interface_declare(const ShaderCreateInfo &info) const
{
  std::stringstream ss;
  std::string pre_main;
  std::string post_main;

  /* Inputs, read from storage buffers by the variant that expands points (see
   * #WGShader::point_variant_get). */
  ss << "#ifndef WG_POINT_EXPANSION\n";
  for (const ShaderCreateInfo::VertIn &attr : info.vertex_inputs_) {
    ss << "layout(location = " << attr.index << ") ";
    ss << "in " << to_string(attr.type) << " " << attr.name << ";\n";
  }
  ss << "#else\n";
  for (const ShaderCreateInfo::VertIn &attr : info.vertex_inputs_) {
    ss << to_string(attr.type) << " " << attr.name << ";\n";
  }
  ss << "void wg_point_fetch();\n";
  ss << "void wg_point_expand();\n";
  ss << "layout(location=" << wg_varying_locations(info) << ") out vec2 wg_point_coord;\n";
  ss << "#endif\n";
  /* Interfaces. */
  int location = 0;
  for (const StageInterfaceInfo *iface : info.vertex_out_interfaces_) {
    print_interface(ss, "out", *iface, location);
  }

  /* WGSL clip distances are an optional feature, emulate them. */
  if (flag_is_set(info.builtins_combined(), BuiltinBits::CLIP_DISTANCES)) {
    ss << "layout(location=" << location << ") out float wg_ClipDistance[6];\n";
    pre_main += "#define gl_ClipDistance wg_ClipDistance\n";
    location += 6;
  }
  pre_main += "#ifdef WG_POINT_EXPANSION\n";
  pre_main += "  wg_point_fetch();\n";
  pre_main += "#endif\n";

  /* Layered rendering isn't supported, writes are ignored. */
  if (flag_is_set(info.builtins_combined(), BuiltinBits::LAYER)) {
    ss << "int gpu_Layer;\n";
  }
  if (flag_is_set(info.builtins_combined(), BuiltinBits::VIEWPORT_INDEX)) {
    ss << "int gpu_ViewportIndex;\n";
  }
  ss << "\n";

  /* Retarget depth from -1..1 to 0..1 and flip Y (see file description of `wg_shader.hh`). */
  post_main += "gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
  post_main += "gl_Position.y = -gl_Position.y;\n";
  post_main += "#ifdef WG_POINT_EXPANSION\n";
  post_main += "  wg_point_expand();\n";
  post_main += "#endif\n";

  ss << main_function_wrapper(pre_main, post_main);
  return ss.str();
}

std::string WGShader::fragment_interface_declare(const ShaderCreateInfo &info) const
{
  std::stringstream ss;
  std::string pre_main;
  const bool uses_clip_distances = flag_is_set(info.builtins_combined(),
                                               BuiltinBits::CLIP_DISTANCES);

  if (uses_clip_distances) {
    pre_main += "  for (int i = 0; i < 6; i++) {\n";
    pre_main += "    if (wg_ClipDistance[i] < 0.0) { discard; }\n";
    pre_main += "  }\n";
  }

  /* Interfaces. */
  int location = 0;
  for (const StageInterfaceInfo *iface : info.vertex_out_interfaces_) {
    print_interface(ss, "in", *iface, location);
  }
  if (uses_clip_distances) {
    ss << "layout(location=" << location << ") in float wg_ClipDistance[6];\n";
    location += 6;
  }
  ss << "#ifdef WG_POINT_EXPANSION\n";
  ss << "layout(location=" << location << ") in vec2 wg_point_coord;\n";
  ss << "#endif\n";

  if (flag_is_set(info.builtins_combined(), BuiltinBits::LAYER)) {
    ss << "#define gpu_Layer 0\n";
  }
  if (flag_is_set(info.builtins_combined(), BuiltinBits::VIEWPORT_INDEX)) {
    ss << "#define gpu_ViewportIndex 0\n";
  }
  if (flag_is_set(info.builtins_combined(), BuiltinBits::BARYCENTRIC_COORD)) {
    /* TODO: Not supported by WebGPU, would need to be passed by the vertex shader. */
    ss << "#define gpu_BaryCoord vec3(1.0 / 3.0)\n";
    ss << "#define gpu_BaryCoordNoPersp vec3(1.0 / 3.0)\n";
  }

  const bool use_gl_frag_depth = info.depth_write_ != DepthWrite::UNCHANGED &&
                                 info.fragment_source_.find("gl_FragDepth") != StringRef::not_found;
  if (use_gl_frag_depth) {
    ss << "layout(" << to_string(info.depth_write_) << ") out float gl_FragDepth;\n";
  }

  /* Sub-pass Inputs, read from the attachment bound as texture. */
  const WGShaderInterface &wg_interface = interface_get();
  for (const ShaderCreateInfo::SubpassIn &input : info.subpass_inputs_) {
    std::string image_name = "gpu_subpass_img_";
    image_name += std::to_string(input.index);

    /* Declare global for input. */
    ss << to_string(input.type) << " " << input.name << ";\n";

    const bool is_layered_input = ELEM(
        input.img_type, ImageType::Uint2DArray, ImageType::Int2DArray, ImageType::Float2DArray);
    using Resource = ShaderCreateInfo::Resource;
    Resource res(info, Resource::BindType::SAMPLER, input.index, nullptr);
    res.sampler.type = input.img_type;
    res.sampler.name = image_name;
    print_resource(ss, wg_interface.subpass_input_binding_get(input.index), -1, res);
    ss << "\n";

    char swizzle[] = "xyzw";
    swizzle[to_component_count(input.type)] = '\0';
    const std::string texel_co = is_layered_input ? "ivec3(gl_FragCoord.xy, 0)" :
                                                    "ivec2(gl_FragCoord.xy)";
    std::stringstream ss_pre;
    /* Populate the global before main using texel fetch. */
    ss_pre << "  " << input.name << " = texelFetch(" << image_name << ", " << texel_co << ", 0)."
           << swizzle << ";\n";
    pre_main += ss_pre.str();
  }

  /* Outputs. */
  const bool dual_source_blending = WGBackend::get().device.info().dual_source_blending;
  for (const ShaderCreateInfo::FragOut &output : info.fragment_outputs_) {
    if (output.blend == DualBlend::SRC_1 && !dual_source_blending) {
      /* Without the feature the second source is dropped, the blending is approximated
       * (see #wg_blend_state). */
      ss << to_string(output.type) << " " << output.name << ";\n";
      continue;
    }
    ss << "layout(location = " << output.index;
    switch (output.blend) {
      case DualBlend::SRC_0:
        ss << ", index = 0";
        break;
      case DualBlend::SRC_1:
        ss << ", index = 1";
        break;
      default:
        break;
    }
    ss << ") ";
    ss << "out " << to_string(output.type) << " " << output.name << ";\n";
  }
  ss << "\n";

  if (!pre_main.empty()) {
    std::string post_main;
    ss << main_function_wrapper(pre_main, post_main);
  }
  return ss.str();
}

std::string WGShader::compute_layout_declare(const ShaderCreateInfo &info) const
{
  std::stringstream ss;
  ss << "layout(";
  ss << "  local_size_x = " << info.compute_layout_.local_size_x;
  ss << ", local_size_y = " << info.compute_layout_.local_size_y;
  ss << ", local_size_z = " << info.compute_layout_.local_size_z;
  ss << ") in;\n";
  ss << "\n";
  return ss.str();
}

/** Header of all shader stages, like the Vulkan back-end's (see `VKDevice::extensions_define`). */
static std::string wg_glsl_patch(const char *stage_define)
{
  std::stringstream ss;
  ss << "#version 450\n";
  ss << "#extension GL_GOOGLE_cpp_style_line_directive : enable\n";
  ss << "#define GPU_ARB_clip_control\n";
  ss << "#define GPU_ARB_derivative_control\n";
  ss << "#define gl_VertexID gl_VertexIndex\n";
  ss << "#define gpu_BaseInstance (0)\n";
  ss << "#define gpu_InstanceIndex (gl_InstanceIndex)\n";
  ss << "#define gl_InstanceID (gpu_InstanceIndex)\n";
  /* Flat varyings use the first vertex. */
  ss << "#define GPU_PROVOKING_VERTEX_LAST\n";
  ss << stage_define;
  if (STREQ(stage_define, "#define GPU_VERTEX_SHADER\n")) {
    /* WebGPU points are always one pixel, Tint only accepts constant point sizes.
     * Larger points are drawn as quads by a variant of the shader. */
    ss << "float wg_PointSize = 1.0;\n";
    ss << "#define gl_PointSize wg_PointSize\n";
    ss << "#ifdef WG_POINT_EXPANSION\n";
    /* The vertex index of the point, the real one is the index of the quad's vertex. */
    ss << "int wg_quad_vertex_index() { return gl_VertexIndex; }\n";
    ss << "int wg_vertex_index;\n";
    ss << "#define gl_VertexIndex wg_vertex_index\n";
    ss << "#endif\n";
  }

  if (STREQ(stage_define, "#define GPU_FRAGMENT_SHADER\n")) {
    ss << "#ifdef WG_POINT_EXPANSION\n";
    ss << "#define gl_PointCoord wg_point_coord\n";
    ss << "#else\n";
    /* WebGPU points are always one pixel, without point coordinates. */
    ss << "#define gl_PointCoord vec2(0.5)\n";
    ss << "#endif\n";
  }

  shader::GeneratedSourceList sources{
      shader::GeneratedSource{"gpu_shader_glsl_extension.glsl", {}, ss.str()}};
  std::string result = fmt::to_string(fmt::join(
      gpu_shader_dependency_get_resolved_source("gpu_shader_compat_glsl.glsl", sources), ""));
  /* WGSL has no `isnan` and `isinf`, test the bits. */
  result += R"(
bool wg_isnan(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u; }
bvec2 wg_isnan(vec2 x) { return greaterThan(floatBitsToUint(x) & 0x7FFFFFFFu, uvec2(0x7F800000u)); }
bvec3 wg_isnan(vec3 x) { return greaterThan(floatBitsToUint(x) & 0x7FFFFFFFu, uvec3(0x7F800000u)); }
bvec4 wg_isnan(vec4 x) { return greaterThan(floatBitsToUint(x) & 0x7FFFFFFFu, uvec4(0x7F800000u)); }
bool wg_isinf(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) == 0x7F800000u; }
bvec2 wg_isinf(vec2 x) { return equal(floatBitsToUint(x) & 0x7FFFFFFFu, uvec2(0x7F800000u)); }
bvec3 wg_isinf(vec3 x) { return equal(floatBitsToUint(x) & 0x7FFFFFFFu, uvec3(0x7F800000u)); }
bvec4 wg_isinf(vec4 x) { return equal(floatBitsToUint(x) & 0x7FFFFFFFu, uvec4(0x7F800000u)); }
#define isnan wg_isnan
#define isinf wg_isinf
)";
  /* WGSL can't represent NaN constants, read the value from a variable (also see
   * #wg_glsl_workarounds). */
  result += "#undef NAN_FLT\n";
  result += "uint wg_nan_bits = 0x7FC00000u;\n";
  result += "#define NAN_FLT uintBitsToFloat(wg_nan_bits)\n";
  return result;
}

static bool wg_is_identifier_char(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/**
 * Replace calls of `function` with one of `names` as first argument by calls of `replacement`.
 */
static void wg_replace_texture_calls(std::string &glsl,
                                     const StringRef function,
                                     const StringRef replacement,
                                     const Set<std::string> &names)
{
  const std::string pattern = function + "(";
  size_t pos = 0;
  while ((pos = glsl.find(pattern, pos)) != std::string::npos) {
    if (pos > 0 && wg_is_identifier_char(glsl[pos - 1])) {
      pos += pattern.size();
      continue;
    }
    size_t arg_start = pos + pattern.size();
    while (arg_start < glsl.size() && isspace(glsl[arg_start])) {
      arg_start++;
    }
    size_t arg_end = arg_start;
    while (arg_end < glsl.size() && wg_is_identifier_char(glsl[arg_end])) {
      arg_end++;
    }
    size_t next = arg_end;
    while (next < glsl.size() && isspace(glsl[next])) {
      next++;
    }
    if (next < glsl.size() && glsl[next] == ',' &&
        names.contains(glsl.substr(arg_start, arg_end - arg_start)))
    {
      glsl.replace(pos, function.size(), replacement);
      pos += replacement.size();
      continue;
    }
    pos += pattern.size();
  }
}

/**
 * WGSL has no buffer textures, they are 2D textures of #WG_BUFFER_TEXTURE_WIDTH texels per row
 * (see #WGTexture::source_vbo_).
 */
static const char *WG_BUFFER_TEXTURE_FUNCTIONS = R"(
#define samplerBuffer sampler2D
#define isamplerBuffer isampler2D
#define usamplerBuffer usampler2D
vec4 texelFetch(sampler2D s, int i) {
  int w = textureSize(s, 0).x;
  return texelFetch(s, ivec2(i % w, i / w), 0);
}
ivec4 texelFetch(isampler2D s, int i) {
  int w = textureSize(s, 0).x;
  return texelFetch(s, ivec2(i % w, i / w), 0);
}
uvec4 texelFetch(usampler2D s, int i) {
  int w = textureSize(s, 0).x;
  return texelFetch(s, ivec2(i % w, i / w), 0);
}
int textureSize(sampler2D s) {
  ivec2 size = textureSize(s, 0);
  return size.x * size.y;
}
int textureSize(isampler2D s) {
  ivec2 size = textureSize(s, 0);
  return size.x * size.y;
}
int textureSize(usampler2D s) {
  ivec2 size = textureSize(s, 0);
  return size.x * size.y;
}
)";

/** Integer textures can't be sampled in WGSL, fetch the texel instead (nearest filtering). */
static const char *WG_INTEGER_TEXTURE_FUNCTIONS = R"(
uvec4 wg_texture_int(usampler2D s, vec2 co) {
  ivec2 size = textureSize(s, 0);
  return texelFetch(s, clamp(ivec2(co * vec2(size)), ivec2(0), size - 1), 0);
}
ivec4 wg_texture_int(isampler2D s, vec2 co) {
  ivec2 size = textureSize(s, 0);
  return texelFetch(s, clamp(ivec2(co * vec2(size)), ivec2(0), size - 1), 0);
}
uvec4 wg_texture_int(usampler2DArray s, vec3 co) {
  ivec3 size = textureSize(s, 0);
  return texelFetch(s, ivec3(clamp(ivec2(co.xy * vec2(size.xy)), ivec2(0), size.xy - 1), int(co.z)), 0);
}
ivec4 wg_texture_int(isampler2DArray s, vec3 co) {
  ivec3 size = textureSize(s, 0);
  return texelFetch(s, ivec3(clamp(ivec2(co.xy * vec2(size.xy)), ivec2(0), size.xy - 1), int(co.z)), 0);
}
uvec4 wg_texture_int(usampler3D s, vec3 co) {
  ivec3 size = textureSize(s, 0);
  return texelFetch(s, clamp(ivec3(co * vec3(size)), ivec3(0), size - 1), 0);
}
ivec4 wg_texture_int(isampler3D s, vec3 co) {
  ivec3 size = textureSize(s, 0);
  return texelFetch(s, clamp(ivec3(co * vec3(size)), ivec3(0), size - 1), 0);
}
uvec4 wg_texture_lod_int(usampler2D s, vec2 co, float lod) {
  ivec2 size = textureSize(s, int(lod));
  return texelFetch(s, clamp(ivec2(co * vec2(size)), ivec2(0), size - 1), int(lod));
}
ivec4 wg_texture_lod_int(isampler2D s, vec2 co, float lod) {
  ivec2 size = textureSize(s, int(lod));
  return texelFetch(s, clamp(ivec2(co * vec2(size)), ivec2(0), size - 1), int(lod));
}
)";

static bool wg_is_integer_image_type(const ImageType type)
{
  switch (type) {
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Int3D:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Uint3D:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
      return true;
    default:
      return false;
  }
}

/** Source changes for constructs WGSL or Tint don't support. */
static void wg_glsl_workarounds(const ShaderCreateInfo &info, std::string &glsl)
{
  /* Emulated images (see #WGImageMode): their image functions become the helpers. */
  Set<std::string> emulated_images, emulated_samplers;
  for (const Span<ShaderCreateInfo::Resource> resources :
       {info.pass_resources_.as_span(),
        info.batch_resources_.as_span(),
        info.geometry_resources_.as_span()})
  {
    for (const ShaderCreateInfo::Resource &res : resources) {
      if (res.bind_type == ShaderCreateInfo::Resource::BindType::IMAGE &&
          WGShaderInterface::image_mode(res) != WGImageMode::Native)
      {
        emulated_images.add(res.image.name);
      }
      else if (res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER &&
               WGShaderInterface::image_mode(res) == WGImageMode::Buffer)
      {
        emulated_samplers.add(res.sampler.name);
      }
    }
  }
  if (!emulated_images.is_empty()) {
    for (const char *function : {"imageLoad",
                                 "imageStore",
                                 "imageSize",
                                 "imageAtomicAdd",
                                 "imageAtomicMin",
                                 "imageAtomicMax",
                                 "imageAtomicAnd",
                                 "imageAtomicOr",
                                 "imageAtomicXor",
                                 "imageAtomicExchange",
                                 "imageAtomicCompSwap"})
    {
      wg_rewrite_image_calls(glsl, function, emulated_images, function);
    }
    /* Blender's variants without bounds checks. */
    wg_rewrite_image_calls(glsl, "imageLoadFast", emulated_images, "imageLoad");
    wg_rewrite_image_calls(glsl, "imageStoreFast", emulated_images, "imageStore");
  }
  if (!emulated_samplers.is_empty()) {
    for (const char *function : {"texelFetch", "textureSize"}) {
      wg_rewrite_image_calls(glsl, function, emulated_samplers, function);
    }
  }

  Set<std::string> integer_samplers;
  for (const Span<ShaderCreateInfo::Resource> resources :
       {info.pass_resources_.as_span(),
        info.batch_resources_.as_span(),
        info.geometry_resources_.as_span()})
  {
    for (const ShaderCreateInfo::Resource &res : resources) {
      if (res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER &&
          wg_is_integer_image_type(res.sampler.type))
      {
        integer_samplers.add(res.sampler.name);
      }
    }
  }
  if (!integer_samplers.is_empty()) {
    wg_replace_texture_calls(glsl, "texture", "wg_texture_int", integer_samplers);
    wg_replace_texture_calls(glsl, "textureLod", "wg_texture_lod_int", integer_samplers);
  }

  /* NaN constants, also expanded in sources preprocessed when building. */
  const StringRef nan_constant = "uintBitsToFloat(0x7FC00000u)";
  for (int64_t pos = StringRef(glsl).find(nan_constant); pos != StringRef::not_found;
       pos = StringRef(glsl).find(nan_constant, pos))
  {
    glsl.replace(pos, nan_constant.size(), "uintBitsToFloat(wg_nan_bits)");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compilation
 * \{ */

class WGLogParser : public GPULogParser {
 public:
  const char *parse_line(const char * /*source_combined*/,
                         const char *log_line,
                         GPULogItem &log_item) override
  {
    return skip_severity(log_line, log_item, "ERROR", "WARNING", "NOTE");
  }
};

WGShader::WGShader(const char *name) : Shader(name) {}

WGShader::~WGShader()
{
  for (WGObjectID pipeline : render_pipelines_.values()) {
    wg_object_destroy(pipeline);
  }
  for (WGObjectID pipeline : compute_pipelines_.values()) {
    wg_object_destroy(pipeline);
  }
  for (const std::unique_ptr<WGPointVariant> &variant : point_variants_.values()) {
    if (variant) {
      wg_object_destroy(variant->pipeline_layout);
      wg_object_destroy(variant->bind_group_layout);
      wg_object_destroy(variant->vertex_module);
    }
  }
  wg_object_destroy(point_fragment_module_);
  wg_object_destroy(pipeline_layout_);
  wg_object_destroy(bind_group_layouts_[0]);
  wg_object_destroy(bind_group_layouts_[1]);
  wg_object_destroy(vertex_module_);
  wg_object_destroy(fragment_module_);
  wg_object_destroy(compute_module_);
}

void WGShader::init(const ShaderCreateInfo &info, bool /*is_codegen_only*/)
{
  WGShaderInterface *wg_interface = new WGShaderInterface();
  wg_interface->init(info);
  interface = wg_interface;
  is_compute_ = !info.compute_source_.is_empty() || !info.compute_source_generated.empty();
  push_constants_data_.resize(wg_interface->push_constants_layout().size_in_bytes, 0);
  for (const ShaderCreateInfo::FragOut &output : info.fragment_outputs_) {
    fragment_outputs_mask_ |= 1u << output.index;
  }
}

static bool glsl_uses_integer_samplers(const ShaderCreateInfo &info)
{
  for (const Span<ShaderCreateInfo::Resource> resources :
       {info.pass_resources_.as_span(),
        info.batch_resources_.as_span(),
        info.geometry_resources_.as_span()})
  {
    for (const ShaderCreateInfo::Resource &res : resources) {
      if (res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER &&
          wg_is_integer_image_type(res.sampler.type))
      {
        return true;
      }
    }
  }
  return false;
}

/** Copy the shader name into the label of a creation command. */
template<typename T> static T wg_labeled(T cmd, const char *name)
{
  BLI_strncpy(cmd.label, name, sizeof(cmd.label));
  return cmd;
}

/** Samplers use the binding of their texture in the sampler group. */
static Vector<WSC_SamplerMapping> wg_sampler_mappings(const WGShaderInterface &interface)
{
  Vector<WSC_SamplerMapping> sampler_mappings;
  for (const WGResourceBinding &resource : interface.resources()) {
    if (ELEM(resource.type,
             WGResourceBinding::Type::Sampler,
             WGResourceBinding::Type::SubpassInput) ||
        resource.image_as_texture)
    {
      sampler_mappings.append({0, resource.binding, WG_SAMPLER_GROUP, resource.binding});
    }
  }
  return sampler_mappings;
}

/** Blender's preprocessor requires sources without comments. */
static std::string wg_strip_comments(const std::string &source)
{
  std::string result;
  result.reserve(source.size());
  for (size_t i = 0; i < source.size();) {
    if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      const size_t end = source.find("*/", i + 2);
      i = (end == std::string::npos) ? source.size() : end + 2;
      continue;
    }
    if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      const size_t end = source.find('\n', i);
      i = (end == std::string::npos) ? source.size() : end;
      continue;
    }
    result += source[i++];
  }
  return result;
}

/** Add a define after the `#version` line. */
static std::string wg_glsl_define(const std::string &glsl, const char *define)
{
  const size_t version_end = glsl.find('\n') + 1;
  return glsl.substr(0, version_end) + "#define " + define + "\n" + glsl.substr(version_end);
}

/** Translate GLSL to a WGSL module, for variants (errors are only logged). */
WGObjectID WGShader::module_from_glsl(const std::string &source, const int stage)
{
  const std::string glsl = skip_preprocessor ? source :
                                               run_preprocessor(wg_strip_comments(source),
                                                                G.debug & G_DEBUG_GPU_SHADER_NO_DCE);
  const Vector<WSC_SamplerMapping> sampler_mappings = wg_sampler_mappings(interface_get());
  WSC_Result result = WSC_glsl_to_wgsl(glsl.c_str(),
                                       WSC_Stage(stage),
                                       sampler_mappings.data(),
                                       int(sampler_mappings.size()));
  if (result.wgsl == nullptr) {
    CLOG_WARN(&LOG, "Shader %s: variant failed to compile:\n%s", name, result.log);
    if (G.debug & G_DEBUG_GPU) {
      printf("WebGPU GLSL of %s (variant):\n%s\nEND OF GLSL\n", name, glsl.c_str());
    }
    WSC_result_free(&result);
    return WG_NULL_ID;
  }
  const WGObjectID module = wg_object_id_alloc();
  {
    WGRecord record;
    record.stream.push(WGCommandType::CreateShaderModule,
                       wg_labeled(WGCmdCreateShaderModule{module}, name),
                       result.wgsl,
                       strlen(result.wgsl) + 1);
  }
  WSC_result_free(&result);
  return module;
}

void WGShader::build_module(const ShaderCreateInfo &info,
                            MutableSpan<StringRefNull> sources,
                            const int stage,
                            WGObjectID &r_module)
{
  const char *stage_define = stage == WSC_STAGE_VERTEX   ? "#define GPU_VERTEX_SHADER\n" :
                             stage == WSC_STAGE_FRAGMENT ? "#define GPU_FRAGMENT_SHADER\n" :
                                                           "#define GPU_COMPUTE_SHADER\n";
  const char *stage_name = stage == WSC_STAGE_VERTEX   ? "vertex" :
                           stage == WSC_STAGE_FRAGMENT ? "fragment" :
                                                         "compute";
  std::string patch = wg_glsl_patch(stage_define);
  if (glsl_uses_integer_samplers(info)) {
    patch += WG_INTEGER_TEXTURE_FUNCTIONS;
  }
  bool uses_point_size = false;
  for (const int i : sources.index_range().drop_front(1)) {
    if (sources[i].find("samplerBuffer") != StringRef::not_found) {
      patch += WG_BUFFER_TEXTURE_FUNCTIONS;
      break;
    }
  }
  if (stage == WSC_STAGE_VERTEX) {
    for (const int i : sources.index_range().drop_front(1)) {
      if (sources[i].find("gl_PointSize") != StringRef::not_found) {
        uses_point_size = true;
        break;
      }
    }
  }
  sources[SOURCES_INDEX_VERSION] = patch;
  std::string glsl = fmt::to_string(fmt::join(sources, ""));
  wg_glsl_workarounds(info, glsl);

  if (stage == WSC_STAGE_VERTEX && uses_point_size) {
    for (const ShaderCreateInfo::VertIn &attr : info.vertex_inputs_) {
      vertex_inputs_.append({uint32_t(attr.index), attr.type, attr.name});
    }
    point_vertex_glsl_ = wg_glsl_define(glsl, "WG_POINT_EXPANSION");
    point_coord_location_ = uint32_t(wg_varying_locations(info));
  }
  if (stage == WSC_STAGE_FRAGMENT && has_point_size()) {
    point_fragment_module_ = module_from_glsl(wg_glsl_define(glsl, "WG_POINT_EXPANSION"),
                                              stage);
    if (point_fragment_module_ == WG_NULL_ID) {
      point_vertex_glsl_.clear();
    }
  }
  /* Evaluate static branches (`#if` on constants) and remove dead code, like the other
   * back-ends. The point variants are preprocessed when they are built. */
  if (!skip_preprocessor) {
    glsl = run_preprocessor(wg_strip_comments(glsl), G.debug & G_DEBUG_GPU_SHADER_NO_DCE);
  }

  const Vector<WSC_SamplerMapping> sampler_mappings = wg_sampler_mappings(interface_get());
  WSC_Result result = WSC_glsl_to_wgsl(glsl.c_str(),
                                       WSC_Stage(stage),
                                       sampler_mappings.data(),
                                       int(sampler_mappings.size()));
  if (result.wgsl == nullptr) {
    WGLogParser parser;
    /* The log printer only handles complete lines. */
    std::string log = result.log ? result.log : "Unknown error";
    if (!log.ends_with('\n')) {
      log += '\n';
    }
    print_log(sources, log.c_str(), stage_name, true, &parser);
    if (G.debug & G_DEBUG_GPU) {
      /* The translated source, to reproduce the error with `web/shader_compiler/wsc_test`.
       * The browser truncates long console lines, break them at statements. */
      std::string wrapped;
      size_t line_len = 0;
      for (size_t i = 0; i < glsl.size(); i++) {
        wrapped += glsl[i];
        line_len = (glsl[i] == '\n') ? 0 : line_len + 1;
        if (line_len > 160 && ELEM(glsl[i], ';', ',') && i + 1 < glsl.size() &&
            glsl[i + 1] == ' ')
        {
          wrapped += "\n   ";
          line_len = 0;
        }
      }
      printf("WebGPU GLSL of %s (%s):\n%s\nEND OF GLSL\n", name, stage_name, wrapped.c_str());
    }
    compilation_failed_ = true;
    WSC_result_free(&result);
    return;
  }

  /* Overrides that are used, pipelines can only set those. */
  const StringRef wgsl = result.wgsl;
  for (int64_t pos = wgsl.find("@id("); pos != StringRef::not_found;
       pos = wgsl.find("@id(", pos + 1))
  {
    const uint32_t id = uint32_t(atoi(result.wgsl + pos + 4));
    used_constant_ids_.append_non_duplicates(id);
  }

  r_module = wg_object_id_alloc();
  {
    WGRecord record;
    record.stream.push(WGCommandType::CreateShaderModule,
                       wg_labeled(WGCmdCreateShaderModule{r_module}, name),
                       result.wgsl,
                       strlen(result.wgsl) + 1);
  }
  WSC_result_free(&result);
}

void WGShader::vertex_shader_from_glsl(const ShaderCreateInfo &info,
                                       MutableSpan<StringRefNull> sources)
{
  build_module(info, sources, WSC_STAGE_VERTEX, vertex_module_);
}

void WGShader::geometry_shader_from_glsl(const ShaderCreateInfo & /*info*/,
                                         MutableSpan<StringRefNull> /*sources*/)
{
  CLOG_WARN(&LOG, "Shader %s: geometry shaders are not supported by WebGPU", name);
  compilation_failed_ = true;
}

void WGShader::fragment_shader_from_glsl(const ShaderCreateInfo &info,
                                         MutableSpan<StringRefNull> sources)
{
  build_module(info, sources, WSC_STAGE_FRAGMENT, fragment_module_);
}

void WGShader::compute_shader_from_glsl(const ShaderCreateInfo &info,
                                        MutableSpan<StringRefNull> sources)
{
  build_module(info, sources, WSC_STAGE_COMPUTE, compute_module_);
}

bool WGShader::finalize(const ShaderCreateInfo *info)
{
  if (info == nullptr || compilation_failed_) {
    return false;
  }

  Vector<WGBindingLayout> group_entries[2];
  for (const WGResourceBinding &resource : interface_get().resources()) {
    group_entries[0].append(resource.layout);
    if (ELEM(resource.type,
             WGResourceBinding::Type::Sampler,
             WGResourceBinding::Type::SubpassInput) ||
        resource.image_as_texture)
    {
      WGBindingLayout sampler = {};
      sampler.binding = resource.binding;
      sampler.kind = resource.sampler_kind;
      sampler.visibility = resource.layout.visibility;
      group_entries[1].append(sampler);
    }
  }

  bind_group_entries_ = group_entries[0];

  WGRecord record;
  for (int group : IndexRange(2)) {
    bind_group_layouts_[group] = wg_object_id_alloc();
    record.stream.push(WGCommandType::CreateBindGroupLayout,
                       wg_labeled(WGCmdCreateBindGroupLayout{bind_group_layouts_[group],
                                                             uint32_t(group_entries[group].size())},
                                  name),
                       group_entries[group].data(),
                       group_entries[group].size() * sizeof(WGBindingLayout));
  }
  pipeline_layout_ = wg_object_id_alloc();
  WGCmdCreatePipelineLayout cmd = wg_labeled(WGCmdCreatePipelineLayout{}, name);
  cmd.layout = pipeline_layout_;
  cmd.bind_group_layouts[0] = bind_group_layouts_[0];
  cmd.bind_group_layouts[1] = bind_group_layouts_[1];
  record.stream.push(WGCommandType::CreatePipelineLayout, cmd);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Binding and Uniforms
 * \{ */

void WGShader::bind(const SpecializationConstants *constants_state)
{
  WGContext::get()->specialization_constants_set(constants_state);
}

template<typename T>
static void push_constant_set(const WGPushConstantsLayout &layout,
                              MutableSpan<uint8_t> data,
                              int32_t location,
                              int32_t comp_len,
                              int32_t array_size,
                              const T *input_data)
{
  const WGPushConstantsLayout::PushConstant *push_constant = layout.find(location);
  if (push_constant == nullptr) {
    return;
  }
  T *dst = reinterpret_cast<T *>(&data[push_constant->offset]);
  const int inner_row_padding = push_constant->inner_row_padding;
  const bool is_tightly_std140_packed = (comp_len % 4) == 0;
  if (inner_row_padding == 0 &&
      (array_size == 0 || push_constant->array_size == 0 || is_tightly_std140_packed))
  {
    const size_t copy_size = comp_len * max_ii(array_size, 1) * sizeof(T);
    BLI_assert(push_constant->offset + copy_size <= data.size());
    memcpy(dst, input_data, copy_size);
    return;
  }

  /* In std140 arrays elements have a stride of 16 bytes. */
  const T *src = input_data;
  if (inner_row_padding == 0) {
    for (const int i : IndexRange(array_size)) {
      UNUSED_VARS(i);
      memcpy(dst, src, comp_len * sizeof(T));
      src += comp_len;
      dst += 4;
    }
  }
  else {
    for (const int component_index : IndexRange(comp_len)) {
      *dst = *src;
      dst += 1;
      src += 1;
      if ((component_index % inner_row_padding) == (inner_row_padding - 1)) {
        dst += 1;
      }
    }
  }
}

void WGShader::uniform_float(int location, int comp_len, int array_size, const float *data)
{
  push_constant_set(interface_get().push_constants_layout(),
                    push_constants_data_.as_mutable_span(),
                    location,
                    comp_len,
                    array_size,
                    data);
}

void WGShader::uniform_int(int location, int comp_len, int array_size, const int *data)
{
  push_constant_set(interface_get().push_constants_layout(),
                    push_constants_data_.as_mutable_span(),
                    location,
                    comp_len,
                    array_size,
                    data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipelines
 * \{ */

Vector<uint8_t> WGShader::pipeline_constants(const SpecializationConstants *constants) const
{
  Vector<uint8_t> result;
  if (constants == nullptr) {
    constants = this->constants.get();
  }
  if (constants == nullptr) {
    return result;
  }
  for (const int i : constants->values.index_range()) {
    if (!used_constant_ids_.contains(uint32_t(i))) {
      continue;
    }
    WGPipelineConstant constant = {};
    constant.id = uint32_t(i);
    switch (constants->types[i]) {
      case Type::int_t:
        constant.value = double(constants->values[i].i);
        break;
      default:
        /* Floats are stored as their bits in an uint. */
        constant.value = double(constants->values[i].u);
        break;
    }
    result.extend(Span(reinterpret_cast<const uint8_t *>(&constant), sizeof(constant)));
  }
  return result;
}

uint64_t wg_constants_hash(const SpecializationConstants *constants)
{
  if (constants == nullptr || constants->values.is_empty()) {
    return 0;
  }
  return wg_hash_bytes(constants->values.data(),
                       constants->values.size() * sizeof(SpecializationConstant::Value)) |
         1;
}

static WGPUCompareFunction wg_depth_compare(const GPUDepthTest test)
{
  switch (test) {
    case GPU_DEPTH_LESS:
      return WGPUCompareFunction_Less;
    case GPU_DEPTH_LESS_EQUAL:
      return WGPUCompareFunction_LessEqual;
    case GPU_DEPTH_EQUAL:
      return WGPUCompareFunction_Equal;
    case GPU_DEPTH_GREATER:
      return WGPUCompareFunction_Greater;
    case GPU_DEPTH_GREATER_EQUAL:
      return WGPUCompareFunction_GreaterEqual;
    case GPU_DEPTH_ALWAYS:
    case GPU_DEPTH_NONE:
    default:
      return WGPUCompareFunction_Always;
  }
}

static void wg_stencil_state(const GPUState state,
                             WGPUStencilFaceState &r_front,
                             WGPUStencilFaceState &r_back)
{
  WGPUStencilFaceState face = {};
  face.compare = WGPUCompareFunction_Always;
  face.failOp = WGPUStencilOperation_Keep;
  face.depthFailOp = WGPUStencilOperation_Keep;
  face.passOp = WGPUStencilOperation_Keep;
  switch (GPUStencilTest(state.stencil_test)) {
    case GPU_STENCIL_NEQUAL:
      face.compare = WGPUCompareFunction_NotEqual;
      break;
    case GPU_STENCIL_EQUAL:
      face.compare = WGPUCompareFunction_Equal;
      break;
    default:
      break;
  }
  r_front = face;
  r_back = face;
  switch (GPUStencilOp(state.stencil_op)) {
    case GPU_STENCIL_OP_REPLACE:
      r_front.passOp = r_back.passOp = WGPUStencilOperation_Replace;
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_PASS:
      r_front.passOp = WGPUStencilOperation_DecrementWrap;
      r_back.passOp = WGPUStencilOperation_IncrementWrap;
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_FAIL:
      r_front.depthFailOp = WGPUStencilOperation_IncrementWrap;
      r_back.depthFailOp = WGPUStencilOperation_DecrementWrap;
      break;
    default:
      break;
  }
}

static void wg_blend_state(const GPUBlend blend,
                           WGPUBlendComponent &r_color,
                           WGPUBlendComponent &r_alpha)
{
  r_color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};
  r_alpha = r_color;
  auto set = [&](WGPUBlendFactor src_color,
                 WGPUBlendFactor dst_color,
                 WGPUBlendFactor src_alpha,
                 WGPUBlendFactor dst_alpha) {
    r_color.srcFactor = src_color;
    r_color.dstFactor = dst_color;
    r_alpha.srcFactor = src_alpha;
    r_alpha.dstFactor = dst_alpha;
  };
  using F = WGPUBlendFactor;
  switch (blend) {
    case GPU_BLEND_ALPHA:
      set(F::WGPUBlendFactor_SrcAlpha,
          F::WGPUBlendFactor_OneMinusSrcAlpha,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_OneMinusSrcAlpha);
      break;
    case GPU_BLEND_ALPHA_PREMULT:
      set(F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_OneMinusSrcAlpha,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_OneMinusSrcAlpha);
      break;
    case GPU_BLEND_ADDITIVE:
      set(F::WGPUBlendFactor_SrcAlpha,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_One);
      break;
    case GPU_BLEND_MIN:
    case GPU_BLEND_MAX:
    case GPU_BLEND_SUBTRACT:
    case GPU_BLEND_ADDITIVE_PREMULT:
      set(F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_One);
      break;
    case GPU_BLEND_MULTIPLY:
      set(F::WGPUBlendFactor_Dst,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_DstAlpha,
          F::WGPUBlendFactor_Zero);
      break;
    case GPU_BLEND_INVERT:
      set(F::WGPUBlendFactor_OneMinusDst,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_One);
      break;
    case GPU_BLEND_OIT:
      set(F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_OneMinusSrcAlpha);
      break;
    case GPU_BLEND_BACKGROUND:
      set(F::WGPUBlendFactor_OneMinusDstAlpha,
          F::WGPUBlendFactor_SrcAlpha,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_SrcAlpha);
      break;
    case GPU_BLEND_ALPHA_UNDER_PREMUL:
      set(F::WGPUBlendFactor_OneMinusDstAlpha,
          F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_OneMinusDstAlpha,
          F::WGPUBlendFactor_One);
      break;
    case GPU_BLEND_CUSTOM:
      if (WGBackend::get().device.info().dual_source_blending) {
        set(F::WGPUBlendFactor_One,
            F::WGPUBlendFactor_Src1,
            F::WGPUBlendFactor_One,
            F::WGPUBlendFactor_Src1Alpha);
      }
      else {
        /* Approximation without the second source (its value is unknown). */
        set(F::WGPUBlendFactor_One,
            F::WGPUBlendFactor_OneMinusSrcAlpha,
            F::WGPUBlendFactor_One,
            F::WGPUBlendFactor_OneMinusSrcAlpha);
      }
      break;
    case GPU_BLEND_OVERLAY_MASK_FROM_ALPHA:
      set(F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_OneMinusSrcAlpha,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_OneMinusSrcAlpha);
      break;
    case GPU_BLEND_TRANSPARENCY:
      set(F::WGPUBlendFactor_One,
          F::WGPUBlendFactor_SrcAlpha,
          F::WGPUBlendFactor_Zero,
          F::WGPUBlendFactor_SrcAlpha);
      break;
    default:
      break;
  }
  if (blend == GPU_BLEND_MIN) {
    r_color.operation = r_alpha.operation = WGPUBlendOperation_Min;
    /* Factors must be one for min and max. */
  }
  else if (blend == GPU_BLEND_MAX) {
    r_color.operation = r_alpha.operation = WGPUBlendOperation_Max;
  }
  else if (blend == GPU_BLEND_SUBTRACT) {
    r_color.operation = r_alpha.operation = WGPUBlendOperation_ReverseSubtract;
  }
}

WGObjectID WGShader::render_pipeline_get(const WGRenderPipelineKey &key,
                                         const WGVertexLayout &vertex_layout,
                                         const SpecializationConstants *constants,
                                         const WGPointVariant *point_variant)
{
  std::lock_guard lock(pipelines_mutex_);
  if (const WGObjectID *pipeline = render_pipelines_.lookup_ptr(key)) {
    return *pipeline;
  }

  const GPUState state{.data = key.state};
  WGCmdCreateRenderPipeline cmd = wg_labeled(WGCmdCreateRenderPipeline{}, name);
  cmd.pipeline = wg_object_id_alloc();
  cmd.layout = point_variant ? point_variant->pipeline_layout : pipeline_layout_;
  cmd.vertex_module = point_variant ? point_variant->vertex_module : vertex_module_;
  cmd.fragment_module = point_variant ? point_fragment_module_ : fragment_module_;
  cmd.topology = key.topology;
  cmd.strip_index_format = key.strip_index_format;
  /* Y is flipped by the vertex shader, which flips the winding. */
  cmd.front_face = state.invert_facing ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
  switch (GPUFaceCullTest(state.culling_test)) {
    case GPU_CULL_FRONT:
      cmd.cull_mode = WGPUCullMode_Front;
      break;
    case GPU_CULL_BACK:
      cmd.cull_mode = WGPUCullMode_Back;
      break;
    default:
      cmd.cull_mode = WGPUCullMode_None;
      break;
  }
  /* Line and point topologies can't be culled. */
  if (point_variant ||
      !ELEM(key.topology, WGPUPrimitiveTopology_TriangleList, WGPUPrimitiveTopology_TriangleStrip))
  {
    cmd.cull_mode = WGPUCullMode_None;
  }

  WGPUBlendComponent blend_color, blend_alpha;
  wg_blend_state(GPUBlend(state.blend), blend_color, blend_alpha);
  WGPUColorWriteMask write_mask = WGPUColorWriteMask_None;
  if (state.write_mask & GPU_WRITE_RED) {
    write_mask |= WGPUColorWriteMask_Red;
  }
  if (state.write_mask & GPU_WRITE_GREEN) {
    write_mask |= WGPUColorWriteMask_Green;
  }
  if (state.write_mask & GPU_WRITE_BLUE) {
    write_mask |= WGPUColorWriteMask_Blue;
  }
  if (state.write_mask & GPU_WRITE_ALPHA) {
    write_mask |= WGPUColorWriteMask_Alpha;
  }
  cmd.color_targets_len = key.color_len;
  for (const int i : IndexRange(key.color_len)) {
    WGColorTarget &target = cmd.color_targets[i];
    target.format = key.color_formats[i];
    /* Targets not written by the shader must not be written. */
    target.write_mask = (fragment_outputs_mask_ & (1u << i)) ? write_mask :
                                                               WGPUColorWriteMask_None;
    /* Integer formats can't be blended. */
    target.blend_enabled = state.blend != GPU_BLEND_NONE &&
                           !wg_texture_format_is_integer(target.format);
    target.blend_color = blend_color;
    target.blend_alpha = blend_alpha;
  }

  cmd.depth_stencil_format = key.depth_format;
  if (key.depth_format != WGPUTextureFormat_Undefined) {
    cmd.depth_write = (state.write_mask & GPU_WRITE_DEPTH) != 0;
    cmd.depth_compare = wg_depth_compare(GPUDepthTest(state.depth_test));
    if (key.has_stencil && state.stencil_test != GPU_STENCIL_NONE) {
      wg_stencil_state(state, cmd.stencil_front, cmd.stencil_back);
      cmd.stencil_read_mask = key.stencil_compare_mask;
      cmd.stencil_write_mask = (state.write_mask & GPU_WRITE_STENCIL) ? key.stencil_write_mask :
                                                                        0;
    }
    else {
      wg_stencil_state(GPUState{}, cmd.stencil_front, cmd.stencil_back);
      cmd.stencil_read_mask = 0xFF;
      cmd.stencil_write_mask = 0;
    }
  }
  cmd.sample_count = key.sample_count;

  const Vector<uint8_t> constants_data = pipeline_constants(constants);
  cmd.constants_len = constants_data.size() / sizeof(WGPipelineConstant);

  Vector<uint8_t> trailing;
  /* Point variants read the vertex buffers as storage buffers. */
  if (point_variant == nullptr) {
    cmd.vertex_buffers_len = vertex_layout.buffers.size();
    cmd.attributes_len = vertex_layout.attributes.size();
    trailing.extend(Span(reinterpret_cast<const uint8_t *>(vertex_layout.buffers.data()),
                         vertex_layout.buffers.size() * sizeof(WGVertexBufferLayout)));
    trailing.extend(Span(reinterpret_cast<const uint8_t *>(vertex_layout.attributes.data()),
                         vertex_layout.attributes.size() * sizeof(WGVertexAttribute)));
  }
  trailing.extend(constants_data);
  {
    WGRecord record;
    record.stream.push(
        WGCommandType::CreateRenderPipeline, cmd, trailing.data(), trailing.size());
  }
  render_pipelines_.add(key, cmd.pipeline);
  return cmd.pipeline;
}

WGObjectID WGShader::compute_pipeline_get(const SpecializationConstants *constants)
{
  std::lock_guard lock(pipelines_mutex_);
  const uint64_t key = wg_constants_hash(constants ? constants : this->constants.get());
  if (const WGObjectID *pipeline = compute_pipelines_.lookup_ptr(key)) {
    return *pipeline;
  }
  WGCmdCreateComputePipeline cmd = wg_labeled(WGCmdCreateComputePipeline{}, name);
  cmd.pipeline = wg_object_id_alloc();
  cmd.layout = pipeline_layout_;
  cmd.module = compute_module_;
  const Vector<uint8_t> constants_data = pipeline_constants(constants);
  cmd.constants_len = constants_data.size() / sizeof(WGPipelineConstant);
  {
    WGRecord record;
    record.stream.push(WGCommandType::CreateComputePipeline,
                       cmd,
                       constants_data.data(),
                       constants_data.size());
  }
  compute_pipelines_.add(key, cmd.pipeline);
  return cmd.pipeline;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Point Variants
 * \{ */

/**
 * GLSL expression reading an attribute of format `format` at word `w` of `buffer`, as a 4
 * component vector (missing components are 0, except the 4th which is 1, like vertex fetching).
 * Returns an empty string for unsupported formats.
 */
static std::string wg_attribute_fetch(const WGPUVertexFormat format,
                                      const std::string &buffer,
                                      const std::string &w)
{
  const std::string word0 = buffer + "[" + w + "]";
  const std::string word1 = buffer + "[" + w + " + 1u]";
  const std::string word2 = buffer + "[" + w + " + 2u]";
  const std::string word3 = buffer + "[" + w + " + 3u]";
  auto f = [](const std::string &word) { return "uintBitsToFloat(" + word + ")"; };
  auto i = [](const std::string &word) { return "int(" + word + ")"; };
  auto bits = [](const std::string &word, const int offset, const int len) {
    return "bitfieldExtract(int(" + word + "), " + std::to_string(offset) + ", " +
           std::to_string(len) + ")";
  };
  switch (format) {
    case WGPUVertexFormat_Float32:
      return "vec4(" + f(word0) + ", 0.0, 0.0, 1.0)";
    case WGPUVertexFormat_Float32x2:
      return "vec4(" + f(word0) + ", " + f(word1) + ", 0.0, 1.0)";
    case WGPUVertexFormat_Float32x3:
      return "vec4(" + f(word0) + ", " + f(word1) + ", " + f(word2) + ", 1.0)";
    case WGPUVertexFormat_Float32x4:
      return "vec4(" + f(word0) + ", " + f(word1) + ", " + f(word2) + ", " + f(word3) + ")";
    case WGPUVertexFormat_Uint32:
      return "uvec4(" + word0 + ", 0u, 0u, 1u)";
    case WGPUVertexFormat_Uint32x2:
      return "uvec4(" + word0 + ", " + word1 + ", 0u, 1u)";
    case WGPUVertexFormat_Uint32x3:
      return "uvec4(" + word0 + ", " + word1 + ", " + word2 + ", 1u)";
    case WGPUVertexFormat_Uint32x4:
      return "uvec4(" + word0 + ", " + word1 + ", " + word2 + ", " + word3 + ")";
    case WGPUVertexFormat_Sint32:
      return "ivec4(" + i(word0) + ", 0, 0, 1)";
    case WGPUVertexFormat_Sint32x2:
      return "ivec4(" + i(word0) + ", " + i(word1) + ", 0, 1)";
    case WGPUVertexFormat_Sint32x3:
      return "ivec4(" + i(word0) + ", " + i(word1) + ", " + i(word2) + ", 1)";
    case WGPUVertexFormat_Sint32x4:
      return "ivec4(" + i(word0) + ", " + i(word1) + ", " + i(word2) + ", " + i(word3) + ")";
    case WGPUVertexFormat_Unorm8x4:
      return "unpackUnorm4x8(" + word0 + ")";
    case WGPUVertexFormat_Snorm8x4:
      return "unpackSnorm4x8(" + word0 + ")";
    case WGPUVertexFormat_Uint8x4:
      return "uvec4(" + word0 + " & 0xFFu, (" + word0 + " >> 8u) & 0xFFu, (" + word0 +
             " >> 16u) & 0xFFu, " + word0 + " >> 24u)";
    case WGPUVertexFormat_Sint8x4:
      return "ivec4(" + bits(word0, 0, 8) + ", " + bits(word0, 8, 8) + ", " + bits(word0, 16, 8) +
             ", " + bits(word0, 24, 8) + ")";
    case WGPUVertexFormat_Unorm16x2:
      return "vec4(unpackUnorm2x16(" + word0 + "), 0.0, 1.0)";
    case WGPUVertexFormat_Unorm16x4:
      return "vec4(unpackUnorm2x16(" + word0 + "), unpackUnorm2x16(" + word1 + "))";
    case WGPUVertexFormat_Snorm16x2:
      return "vec4(unpackSnorm2x16(" + word0 + "), 0.0, 1.0)";
    case WGPUVertexFormat_Snorm16x4:
      return "vec4(unpackSnorm2x16(" + word0 + "), unpackSnorm2x16(" + word1 + "))";
    case WGPUVertexFormat_Float16x2:
      return "vec4(unpackHalf2x16(" + word0 + "), 0.0, 1.0)";
    case WGPUVertexFormat_Float16x4:
      return "vec4(unpackHalf2x16(" + word0 + "), unpackHalf2x16(" + word1 + "))";
    case WGPUVertexFormat_Uint16x2:
      return "uvec4(" + word0 + " & 0xFFFFu, " + word0 + " >> 16u, 0u, 1u)";
    case WGPUVertexFormat_Uint16x4:
      return "uvec4(" + word0 + " & 0xFFFFu, " + word0 + " >> 16u, " + word1 + " & 0xFFFFu, " +
             word1 + " >> 16u)";
    case WGPUVertexFormat_Sint16x2:
      return "ivec4(" + bits(word0, 0, 16) + ", " + bits(word0, 16, 16) + ", 0, 1)";
    case WGPUVertexFormat_Sint16x4:
      return "ivec4(" + bits(word0, 0, 16) + ", " + bits(word0, 16, 16) + ", " +
             bits(word1, 0, 16) + ", " + bits(word1, 16, 16) + ")";
    case WGPUVertexFormat_Unorm10_10_10_2:
      return "(vec4(" + word0 + " & 1023u, (" + word0 + " >> 10u) & 1023u, (" + word0 +
             " >> 20u) & 1023u, " + word0 + " >> 30u) / vec4(1023.0, 1023.0, 1023.0, 3.0))";
    default:
      return "";
  }
}

std::unique_ptr<WGPointVariant> WGShader::point_variant_create(const WGVertexLayout &vertex_layout,
                                                               const WGPUIndexFormat index_format)
{
  const WGPULimits &limits = WGBackend::get().device.info().limits;
  const bool is_indexed = index_format != WGPUIndexFormat_Undefined;

  /* Storage buffers of the vertex stage must stay within the limits. */
  uint32_t storage_buffers_len = uint32_t(vertex_layout.buffers.size()) + (is_indexed ? 1 : 0);
  uint32_t uniform_buffers_len = 1;
  for (const WGBindingLayout &entry : bind_group_entries_) {
    if (!(entry.visibility & WGPUShaderStage_Vertex)) {
      continue;
    }
    if (ELEM(entry.kind,
             WGBindingLayout::Kind::StorageBuffer,
             WGBindingLayout::Kind::ReadOnlyStorageBuffer))
    {
      storage_buffers_len++;
    }
    else if (entry.kind == WGBindingLayout::Kind::UniformBuffer) {
      uniform_buffers_len++;
    }
  }
  if (storage_buffers_len > limits.maxStorageBuffersPerShaderStage ||
      uniform_buffers_len > limits.maxUniformBuffersPerShaderStage ||
      vertex_layout.buffers.size() > WG_MAX_VERTEX_BUFFERS ||
      point_coord_location_ >= limits.maxInterStageShaderVariables)
  {
    return nullptr;
  }

  std::stringstream ss;
  ss << "\n";
  for (const int buffer_index : vertex_layout.buffers.index_range()) {
    ss << "layout(binding = " << WG_POINT_BINDING + buffer_index << ", std430) readonly buffer "
       << "wg_vbo_" << buffer_index << "_ { uint wg_vbo_" << buffer_index << "[]; };\n";
  }
  if (is_indexed) {
    ss << "layout(binding = " << WG_POINT_INDEX_BINDING
       << ", std430) readonly buffer wg_ibo_ { uint wg_ibo[]; };\n";
  }
  ss << "layout(binding = " << WG_POINT_DRAW_DATA_BINDING << ", std140) uniform wg_point_draw_ {\n";
  ss << "  ivec4 wg_point_draw;\n";
  ss << "  vec4 wg_point_viewport;\n";
  ss << "  uvec4 wg_vbo_offsets[" << WG_MAX_VERTEX_BUFFERS / 4 << "];\n";
  ss << "};\n";
  ss << "bool wg_point_skip = false;\n";

  /* Read the vertex attributes, `wg_vertex_index` is the vertex of the point. */
  ss << "void wg_point_fetch() {\n";
  ss << "  uint point = uint(wg_quad_vertex_index()) / 6u;\n";
  ss << "  uint index = uint(wg_point_draw.x) + point;\n";
  if (index_format == WGPUIndexFormat_Uint32) {
    ss << "  uint vertex = wg_ibo[index];\n";
    ss << "  wg_point_skip = vertex == 0xFFFFFFFFu;\n";
    ss << "  vertex = uint(int(vertex) + wg_point_draw.y);\n";
  }
  else if (index_format == WGPUIndexFormat_Uint16) {
    ss << "  uint word = wg_ibo[index >> 1u];\n";
    ss << "  uint vertex = ((index & 1u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);\n";
    ss << "  wg_point_skip = vertex == 0xFFFFu;\n";
    ss << "  vertex = uint(int(vertex) + wg_point_draw.y);\n";
  }
  else {
    ss << "  uint vertex = index;\n";
  }
  ss << "  wg_vertex_index = int(vertex);\n";
  ss << "  uint instance = uint(gl_InstanceIndex);\n";
  for (const int buffer_index : vertex_layout.buffers.index_range()) {
    const WGVertexBufferLayout &buffer = vertex_layout.buffers[buffer_index];
    if (buffer.stride % 4 != 0) {
      return nullptr;
    }
    const std::string buffer_name = "wg_vbo_" + std::to_string(buffer_index);
    for (const WGVertexAttribute &attribute :
         vertex_layout.attributes.as_span().slice(buffer.attributes_start, buffer.attributes_len))
    {
      if (attribute.offset % 4 != 0) {
        return nullptr;
      }
      const VertexInput *input = nullptr;
      for (const VertexInput &vertex_input : vertex_inputs_) {
        if (vertex_input.location == attribute.location) {
          input = &vertex_input;
        }
      }
      if (input == nullptr) {
        continue;
      }
      const std::string fetch = wg_attribute_fetch(attribute.format, buffer_name, "w");
      const int component_len = to_component_count(input->type);
      if (fetch.empty() || component_len > 4 ||
          ELEM(input->type, Type::bool_t, Type::float3x3_t, Type::float4x4_t))
      {
        return nullptr;
      }
      const char *swizzle[] = {"", "x", "xy", "xyz", "xyzw"};
      ss << "  {\n";
      ss << "    uint w = ("
         << (buffer.step_mode == WGPUVertexStepMode_Instance ? "instance" : "vertex") << " * "
         << buffer.stride << "u + " << attribute.offset << "u + wg_vbo_offsets["
         << buffer_index / 4 << "][" << buffer_index % 4 << "]) >> 2u;\n";
      ss << "    " << input->name << " = " << to_string(input->type) << "((" << fetch << ")."
         << swizzle[component_len] << ");\n";
      ss << "  }\n";
    }
  }
  ss << "}\n";

  /* Move the vertices to the corners of the point, two triangles per point. */
  ss << "void wg_point_expand() {\n";
  ss << "  const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),\n";
  ss << "                                  vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));\n";
  ss << "  vec2 corner = corners[uint(wg_quad_vertex_index()) % 6u];\n";
  ss << "  gl_Position.xy += corner * wg_PointSize * wg_point_viewport.xy * gl_Position.w;\n";
  ss << "  wg_point_coord = corner * 0.5 + 0.5;\n";
  ss << "  if (wg_point_skip) {\n";
  ss << "    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n";
  ss << "  }\n";
  ss << "}\n";

  std::unique_ptr<WGPointVariant> variant = std::make_unique<WGPointVariant>();
  variant->vertex_module = module_from_glsl(point_vertex_glsl_ + ss.str(), WSC_STAGE_VERTEX);
  if (variant->vertex_module == WG_NULL_ID) {
    return nullptr;
  }

  Vector<WGBindingLayout> entries = bind_group_entries_;
  auto add_entry = [&](const uint32_t binding, const WGBindingLayout::Kind kind) {
    WGBindingLayout entry = {};
    entry.binding = binding;
    entry.kind = kind;
    entry.visibility = WGPUShaderStage_Vertex;
    entries.append(entry);
  };
  for (const int buffer_index : vertex_layout.buffers.index_range()) {
    add_entry(WG_POINT_BINDING + buffer_index, WGBindingLayout::Kind::ReadOnlyStorageBuffer);
  }
  if (is_indexed) {
    add_entry(WG_POINT_INDEX_BINDING, WGBindingLayout::Kind::ReadOnlyStorageBuffer);
  }
  add_entry(WG_POINT_DRAW_DATA_BINDING, WGBindingLayout::Kind::UniformBuffer);

  WGRecord record;
  variant->bind_group_layout = wg_object_id_alloc();
  record.stream.push(
      WGCommandType::CreateBindGroupLayout,
      wg_labeled(WGCmdCreateBindGroupLayout{variant->bind_group_layout, uint32_t(entries.size())},
                 name),
      entries.data(),
      entries.size() * sizeof(WGBindingLayout));
  variant->pipeline_layout = wg_object_id_alloc();
  WGCmdCreatePipelineLayout cmd = wg_labeled(WGCmdCreatePipelineLayout{}, name);
  cmd.layout = variant->pipeline_layout;
  cmd.bind_group_layouts[0] = variant->bind_group_layout;
  cmd.bind_group_layouts[1] = bind_group_layouts_[1];
  record.stream.push(WGCommandType::CreatePipelineLayout, cmd);
  return variant;
}

const WGPointVariant *WGShader::point_variant_get(const WGVertexLayout &vertex_layout,
                                                  const WGPUIndexFormat index_format)
{
  if (!has_point_size() || point_fragment_module_ == WG_NULL_ID) {
    return nullptr;
  }
  const uint64_t hash = vertex_layout.hash() * 33 ^ uint64_t(index_format);
  std::lock_guard lock(pipelines_mutex_);
  return point_variants_
      .lookup_or_add_cb(hash,
                        [&]() {
                          std::unique_ptr<WGPointVariant> variant = point_variant_create(
                              vertex_layout, index_format);
                          if (variant) {
                            variant->hash = hash;
                          }
                          return variant;
                        })
      .get();
}

/** \} */

}  // namespace blender::gpu
