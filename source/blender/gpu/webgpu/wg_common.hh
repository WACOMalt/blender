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
WGPUTextureSampleType wg_texture_sample_type(TextureFormat device_format);
bool wg_texture_format_is_integer(WGPUTextureFormat format);
bool wg_texture_format_has_stencil(WGPUTextureFormat format);

uint64_t wg_hash_bytes(const void *data, size_t size);

/** #WGPUVertexFormat_Force32 when there is no matching WebGPU format. */
WGPUVertexFormat wg_vertex_format(gpu::VertAttrType type);

}  // namespace blender::gpu
