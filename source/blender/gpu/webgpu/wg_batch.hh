/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_batch.hh"

namespace blender::gpu {

struct WGDrawCall;

class WGBatch : public Batch {
 public:
  void draw(int vertex_first, int vertex_count, int instance_first, int instance_count) override;
  void draw_indirect(StorageBuf *indirect_buf, intptr_t offset) override;
  void multi_draw_indirect(StorageBuf *indirect_buf,
                           int count,
                           intptr_t offset,
                           intptr_t stride) override;

 private:
  /** Fill the vertex input and index buffer of a draw of this batch. */
  bool draw_call_prepare(WGDrawCall &r_call);
};

}  // namespace blender::gpu
