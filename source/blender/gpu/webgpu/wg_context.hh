/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "gpu_context_private.hh"

#include "wg_commands.hh"
#include "wg_shader.hh"

namespace blender::gpu {

class WGFrameBuffer;
class WGStateManager;
class WGTexture;
class WGStorageBuf;

/** A draw, see #WGContext::draw. */
struct WGDrawCall {
  GPUPrimType prim_type = GPU_PRIM_TRIS;
  WGVertexLayout vertex_layout;
  /** Vertex buffers, matching `vertex_layout.buffers`. */
  Vector<WGVertexBufferBinding, 4> vertex_buffers;
  /**
   * Vertex data copied to the stream data, used by vertex buffers bound to #WG_STREAM_DATA_ID
   * (their offset is relative to this data). Used by immediate mode.
   */
  Span<uint8_t> stream_vertex_data;

  /** Indexed draw when not #WG_NULL_ID. */
  WGObjectID index_buffer = WG_NULL_ID;
  WGPUIndexFormat index_format = WGPUIndexFormat_Uint32;
  /** Indirect draw when not #WG_NULL_ID. */
  WGObjectID indirect_buffer = WG_NULL_ID;
  uint64_t indirect_offset = 0;

  uint32_t first = 0;
  uint32_t count = 0;
  uint32_t first_instance = 0;
  uint32_t instance_count = 1;
  int32_t base_vertex = 0;
};

class WGContext : public Context {
  /** Specialization constants set by the last shader bind. */
  const shader::SpecializationConstants *constants_ = nullptr;
  /** Color attachment of the window framebuffer, presented at the end of each frame. */
  WGTexture *window_texture_ = nullptr;

 public:
  WGContext(GHOST_IWindow *ghost_window);
  ~WGContext() override;

  static WGContext *get()
  {
    return static_cast<WGContext *>(Context::get());
  }

  void activate() override;
  void deactivate() override;
  void begin_frame() override;
  void end_frame() override;

  void flush() override;
  void finish() override;

  void memory_statistics_get(int *r_total_mem, int *r_free_mem) override;

  bool debug_capture_begin(const char * /*title*/) override
  {
    return false;
  }
  void debug_capture_end() override {}
  void *debug_capture_scope_create(const char * /*name*/) override
  {
    return nullptr;
  }
  bool debug_capture_scope_begin(void * /*scope*/) override
  {
    return false;
  }
  void debug_capture_scope_end(void * /*scope*/) override {}

  void debug_unbind_all_ubo() override {}
  void debug_unbind_all_ssbo() override {}

  void specialization_constants_set(const shader::SpecializationConstants *constants)
  {
    constants_ = constants;
  }

  WGStateManager &state_manager_get() const;
  WGFrameBuffer *active_framebuffer_get() const;

  /** Record a draw with the bound shader, state and framebuffer. */
  void draw(const WGDrawCall &call);
  /** Record a dispatch with the bound compute shader. */
  void dispatch(const uint32_t groups[3], WGStorageBuf *indirect_buffer);

 private:
  /** Resize the window framebuffer to the canvas. */
  void window_size_sync();
  /** Append the bind group entries of the shader resources, the recorder must be locked. */
  bool resources_get(WGShader &shader,
                     WGCommandStream &stream,
                     uint64_t stream_generation,
                     Vector<WGBindingResource, 16> &r_resources);
};

/**
 * Append the vertex buffer layout of `format` for the attributes used by `interface`.
 * `r_occupied` tracks the attribute locations that are already bound.
 */
void wg_vertex_layout_add(const GPUVertFormat &format,
                          WGObjectID buffer,
                          uint32_t vertex_len,
                          bool per_instance,
                          const WGShaderInterface &interface,
                          uint16_t &r_occupied,
                          WGDrawCall &r_call);
/** Bind a zero buffer to the attributes of `interface` without vertex buffer. */
void wg_vertex_layout_finish(const WGShaderInterface &interface,
                             uint16_t occupied,
                             WGDrawCall &r_call);

}  // namespace blender::gpu
