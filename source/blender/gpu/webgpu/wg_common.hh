/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <mutex>

#include "GPU_texture.hh"
#include "GPU_vertex_format.hh"

#include "wg_commands.hh"

namespace blender::gpu {

/**
 * The stream all GPU work is recorded in, shared by all contexts (and threads).
 * Draws refer to their render pass, so interleaved recording from several contexts is fine.
 */
class WGRecorder {
 public:
  std::mutex mutex;
  WGCommandStream stream;
  /** Render pass ids, see #WGCmdBeginRenderPass. */
  uint32_t next_pass_id = 1;
  /** Incremented when the stream is submitted, see #WGUniformBuf. */
  uint64_t generation = 1;

  /** Take the recorded commands for submission, the recorder must be locked. */
  void take(WGCommandStream &r_stream)
  {
    std::swap(r_stream, stream);
    stream.clear();
    generation++;
  }
};

WGRecorder &wg_recorder();

/** Lock the recorder for the scope of the returned object. */
struct WGRecord {
  std::lock_guard<std::mutex> lock;
  WGCommandStream &stream;
  WGRecord() : lock(wg_recorder().mutex), stream(wg_recorder().stream) {}
};

WGObjectID wg_object_id_alloc();
void wg_object_destroy(WGObjectID id);

/** WebGPU format used for `format`, which may be different (e.g. RGB is stored as RGBA). */
TextureFormat wg_device_format(TextureFormat format);
WGPUTextureFormat wg_texture_format(TextureFormat device_format);
bool wg_texture_format_is_renderable(TextureFormat device_format);
bool wg_texture_format_is_storage(TextureFormat device_format);

/**
 * Layout of the texels of a format in the buffer of an emulated image (see `wg_shader.hh`):
 * the bits of the texture's format, padded to 32 bit words.
 */
struct WGTexelLayout {
  enum class Kind : uint8_t {
    Float32,
    Float16,
    Unorm16,
    Snorm16,
    Uint16,
    Sint16,
    Unorm8,
    Snorm8,
    Uint8,
    Sint8,
    Uint32,
    Sint32,
    UFloat11_11_10,
    Unorm10_10_10_2,
    Uint10_10_10_2,
  };
  Kind kind;
  int components;
  /** 32 bit words per texel. */
  int words;

  bool is_uint() const
  {
    return ELEM(kind, Kind::Uint16, Kind::Uint8, Kind::Uint32, Kind::Uint10_10_10_2);
  }
  bool is_sint() const
  {
    return ELEM(kind, Kind::Sint16, Kind::Sint8, Kind::Sint32);
  }
};
/** False when the format can't be emulated. */
bool wg_texel_layout_get(TextureFormat device_format, WGTexelLayout &r_layout);
WGPUTextureSampleType wg_texture_sample_type(TextureFormat device_format);
bool wg_texture_format_is_integer(WGPUTextureFormat format);
bool wg_texture_format_has_stencil(WGPUTextureFormat format);

uint64_t wg_hash_bytes(const void *data, size_t size);

/** #WGPUVertexFormat_Force32 when there is no matching WebGPU format. */
WGPUVertexFormat wg_vertex_format(gpu::VertAttrType type);

}  // namespace blender::gpu
