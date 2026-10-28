/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Small WebGPU backend classes: fences, work in flight, queries and pixel buffers.
 */

#pragma once

#include "BLI_vector.hh"

#include "GPU_work_in_flight.hh"
#include "gpu_query.hh"
#include "gpu_state_private.hh"
#include "gpu_texture_private.hh"

namespace blender::gpu {

/**
 * Command streams are executed in order on the browser's main thread, waiting for all
 * submitted work is the only synchronization needed.
 */
class WGFence : public Fence {
 public:
  void signal() override;
  void wait() override;
};

class WGWorkInFlight : public WorkInFlight {
 public:
  void reset() override {}
  void begin_work() override {}
  void end_work() override;
};

/** Occlusion queries are not supported yet: all results are "visible". */
class WGQueryPool : public QueryPool {
  int queries_len_ = 0;

 public:
  void init(GPUQueryType /*type*/) override {}
  void begin_query() override
  {
    queries_len_++;
  }
  void end_query() override {}
  void get_occlusion_result(MutableSpan<uint32_t> r_values) override
  {
    r_values.fill(1);
  }
};

/** Pixel buffers are host memory, uploaded by the texture update. */
class WGPixelBuffer : public PixelBuffer {
  Vector<uint8_t> data_;

 public:
  WGPixelBuffer(size_t size) : PixelBuffer(size), data_(size) {}
  void *map() override
  {
    return data_.data();
  }
  void unmap() override {}
  GPUPixelBufferNativeHandle get_native_handle() override
  {
    return {};
  }
  size_t get_size() override
  {
    return size_;
  }
  const void *data() const
  {
    return data_.data();
  }
};

}  // namespace blender::gpu
