/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Immediate mode vertices are copied to the stream data of the draw.
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_immediate_private.hh"

namespace blender::gpu {

class WGUniformBuf;

class WGImmediate : public Immediate {
  Vector<uint8_t> data_;
  /** Vertices of polyline shaders, which read them from storage buffers. */
  WGUniformBuf *polyline_buffer_ = nullptr;

 public:
  ~WGImmediate() override;

  uchar *begin() override;
  void end() override;
};

}  // namespace blender::gpu
