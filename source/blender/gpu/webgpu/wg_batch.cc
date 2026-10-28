/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "wg_batch.hh"
#include "wg_buffer.hh"
#include "wg_context.hh"

namespace blender::gpu {

bool WGBatch::draw_call_prepare(WGDrawCall &r_call)
{
  WGContext *context = WGContext::get();
  const WGShader *shader = static_cast<const WGShader *>(context->shader);
  if (shader == nullptr || !shader->is_valid()) {
    return false;
  }
  const WGShaderInterface &interface = shader->interface_get();
  r_call.prim_type = prim_type;

  uint16_t occupied = 0;
  for (int v = 0; v < GPU_BATCH_VBO_MAX_LEN; v++) {
    WGVertBuf *vbo = static_cast<WGVertBuf *>(verts[v]);
    if (vbo == nullptr) {
      continue;
    }
    const GPUVertFormat *format;
    const WGBufferBinding binding = vbo->vertex_binding_ensure(format);
    wg_vertex_layout_add(
        *format, binding.buffer, vbo->vertex_len, false, interface, occupied, r_call);
  }
  wg_vertex_layout_finish(interface, occupied, r_call);

  if (WGIndexBuf *index_buffer = static_cast<WGIndexBuf *>(elem)) {
    r_call.index_buffer = index_buffer->binding_ensure().buffer;
    r_call.index_format = index_buffer->wg_index_format();
    r_call.base_vertex = int32_t(index_buffer->index_base_get());
  }
  return true;
}

void WGBatch::draw(int vertex_first, int vertex_count, int instance_first, int instance_count)
{
  WGDrawCall call;
  if (!draw_call_prepare(call)) {
    return;
  }
  call.first = uint32_t(vertex_first);
  if (WGIndexBuf *index_buffer = static_cast<WGIndexBuf *>(elem)) {
    call.first += index_buffer->index_start_get();
  }
  call.count = uint32_t(vertex_count);
  call.first_instance = uint32_t(instance_first);
  call.instance_count = uint32_t(instance_count);
  WGContext::get()->draw(call);
}

void WGBatch::draw_indirect(StorageBuf *indirect_buf, intptr_t offset)
{
  WGDrawCall call;
  if (!draw_call_prepare(call)) {
    return;
  }
  call.indirect_buffer = static_cast<WGStorageBuf *>(indirect_buf)->binding_ensure().buffer;
  call.indirect_offset = uint64_t(offset);
  WGContext::get()->draw(call);
}

void WGBatch::multi_draw_indirect(StorageBuf *indirect_buf,
                                  int count,
                                  intptr_t offset,
                                  intptr_t stride)
{
  /* WebGPU has no multi-draw, issue the draws one by one. */
  for (int i = 0; i < count; i++) {
    draw_indirect(indirect_buf, offset + i * stride);
  }
}

}  // namespace blender::gpu
