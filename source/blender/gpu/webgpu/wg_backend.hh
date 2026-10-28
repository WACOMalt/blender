/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU backend, for running Blender in a web browser (Emscripten).
 *
 * WebGPU is used on the browser's main thread while Blender runs on a pthread, see
 * #WGCommandStream for how the two communicate. Shaders are generated as Vulkan GLSL (like the
 * Vulkan backend) and compiled to WGSL with glslang and Tint (`web/shader_compiler/`).
 */

#pragma once

#include "gpu_backend.hh"

#include "wg_device.hh"

namespace blender::gpu {

class WGBackend : public GPUBackend {
 public:
  WGDevice device;

  WGBackend();
  ~WGBackend() override;

  /** True when the browser supports WebGPU. */
  static bool is_supported();

  static WGBackend &get()
  {
    return *static_cast<WGBackend *>(GPUBackend::get());
  }

  void init_resources() override;
  void delete_resources() override;

  void compute_dispatch(int groups_x_len, int groups_y_len, int groups_z_len) override;
  void compute_dispatch_indirect(StorageBuf *indirect_buf) override;

  Context *context_alloc(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context) override;

  Batch *batch_alloc() override;
  Fence *fence_alloc() override;
  WorkInFlight *work_in_flight_alloc(unsigned int max_in_flight) override;
  FrameBuffer *framebuffer_alloc(const char *name) override;
  IndexBuf *indexbuf_alloc() override;
  PixelBuffer *pixelbuf_alloc(size_t size) override;
  QueryPool *querypool_alloc() override;
  Shader *shader_alloc(const char *name) override;
  Texture *texture_alloc(const char *name) override;
  TexturePool *texturepool_alloc() override;
  UniformBuf *uniformbuf_alloc(size_t size, const char *name) override;
  StorageBuf *storagebuf_alloc(size_t size, GPUUsageType usage, const char *name) override;
  VertBuf *vertbuf_alloc() override;
  TopLevelAS *tlas_alloc(const char *name) override;
  BottomLevelAS *blas_alloc(const char *name) override;

  void shader_cache_dir_clear_old() override {}

  void render_begin() override {}
  void render_end() override {}
  void render_step(bool force_resource_release) override;

 private:
  void capabilities_init();
  void platform_init();
};

}  // namespace blender::gpu
