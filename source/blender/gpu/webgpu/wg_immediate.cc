/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_matrix.hh"

#include "gpu_shader_create_info.hh"
#include "gpu_vertex_format_private.hh"

#include "wg_buffer.hh"
#include "wg_context.hh"
#include "wg_immediate.hh"
#include "wg_state.hh"

namespace blender::gpu {

WGImmediate::~WGImmediate()
{
  delete polyline_buffer_;
}

uchar *WGImmediate::begin()
{
  /* Line loops repeat the first vertex. */
  const uint extra_vertices = (prim_type == GPU_PRIM_LINE_LOOP) ? 1 : 0;
  data_.resize(vertex_buffer_size(&vertex_format, vertex_len + extra_vertices));
  return data_.data();
}

void WGImmediate::end()
{
  BLI_assert_msg(prim_type != GPU_PRIM_NONE, "Illegal state: not between an immBegin/End pair.");
  if (vertex_idx == 0) {
    return;
  }
  const uint stride = vertex_format.stride;
  GPUPrimType draw_prim_type = prim_type;
  uint draw_vertex_len = vertex_idx;
  Span<uint8_t> vertex_data(data_.data(), size_t(vertex_idx) * stride);
  Vector<uint8_t> converted;

  if (prim_type == GPU_PRIM_LINE_LOOP) {
    memcpy(data_.data() + size_t(vertex_idx) * stride, data_.data(), stride);
    draw_prim_type = GPU_PRIM_LINE_STRIP;
    draw_vertex_len = vertex_idx + 1;
    vertex_data = Span<uint8_t>(data_.data(), size_t(draw_vertex_len) * stride);
  }
  else if (prim_type == GPU_PRIM_TRI_FAN) {
    /* WebGPU has no triangle fans, convert to a list. */
    if (vertex_idx < 3) {
      return;
    }
    const uint triangles = vertex_idx - 2;
    converted.resize(size_t(triangles) * 3 * stride);
    for (uint i = 0; i < triangles; i++) {
      const uint vertices[3] = {0, i + 1, i + 2};
      for (int j = 0; j < 3; j++) {
        memcpy(converted.data() + (size_t(i) * 3 + j) * stride,
               data_.data() + size_t(vertices[j]) * stride,
               stride);
      }
    }
    draw_prim_type = GPU_PRIM_TRIS;
    draw_vertex_len = triangles * 3;
    vertex_data = converted;
  }

  WGContext &context = *WGContext::get();
  BLI_assert(context.shader == shader);
  if (shader->is_polyline) {
    if (polyline_buffer_ == nullptr) {
      /* The size is set by #WGUniformBuf::host_data_set. */
      polyline_buffer_ = new WGUniformBuf(16, "polyline");
    }
    polyline_buffer_->host_data_set(vertex_data.data(), vertex_data.size());
    WGStateManager &state_manager = context.state_manager_get();
    state_manager.storage_buffer_bind(polyline_buffer_, GPU_SSBO_POLYLINE_POS_BUF_SLOT);
    state_manager.storage_buffer_bind(polyline_buffer_, GPU_SSBO_POLYLINE_COL_BUF_SLOT);
    /* Not used. Satisfy the binding. */
    state_manager.storage_buffer_bind(polyline_buffer_, GPU_SSBO_INDEX_BUF_SLOT);
    /* The workaround reads the vertex count and primitive type. */
    vertex_idx = draw_vertex_len;
    prim_type = draw_prim_type;
    this->polyline_draw_workaround(0);
    return;
  }

  GPU_matrix_bind(shader);
  const WGShader &wg_shader = *static_cast<const WGShader *>(shader);
  if (!wg_shader.is_valid()) {
    return;
  }
  WGDrawCall call;
  call.prim_type = draw_prim_type;
  uint16_t occupied = 0;
  wg_vertex_layout_add(vertex_format,
                       WG_STREAM_DATA_ID,
                       draw_vertex_len,
                       false,
                       wg_shader.interface_get(),
                       occupied,
                       call);
  wg_vertex_layout_finish(wg_shader.interface_get(), occupied, call);
  call.stream_vertex_data = vertex_data;
  call.count = draw_vertex_len;
  context.draw(call);
}

}  // namespace blender::gpu
