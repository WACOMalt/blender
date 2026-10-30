/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <array>

#include "gpu_state_private.hh"

#include "wg_buffer.hh"

namespace blender::gpu {

class WGTexture;
class WGUniformBuf;

constexpr int WG_MAX_TEXTURE_UNITS = 64;
constexpr int WG_MAX_IMAGE_UNITS = GPU_MAX_IMAGE;
constexpr int WG_MAX_BUFFER_SLOTS = 16;

/**
 * Pipeline state and resource bindings. Nothing is applied when changed, draws read the current
 * state (see #WGContext::draw).
 */
class WGStateManager : public StateManager {
 public:
  struct TextureBinding {
    WGTexture *texture = nullptr;
    GPUSamplerState sampler = GPUSamplerState::default_sampler();
  };
  /** A uniform buffer binding is resolved when drawing (see #WGUniformBuf). */
  struct UniformBinding {
    WGUniformBuf *uniform_buffer = nullptr;
    WGBufferBinding buffer;
  };

  std::array<TextureBinding, WG_MAX_TEXTURE_UNITS> textures;
  std::array<WGTexture *, WG_MAX_IMAGE_UNITS> images = {};
  std::array<UniformBinding, WG_MAX_BUFFER_SLOTS> uniform_buffers;
  std::array<UniformBinding, WG_MAX_BUFFER_SLOTS> storage_buffers;

  void apply_state() override {}
  void force_state() override {}
  /* Barriers are not needed, WebGPU tracks resource usage. */
  void issue_barrier(GPUBarrier /*barrier_bits*/) override {}

  void texture_bind(Texture *tex, GPUSamplerState sampler, int unit) override;
  void texture_unbind(Texture *tex) override;
  void texture_unbind_all() override;

  void image_bind(Texture *tex, int unit) override;
  void image_unbind(Texture *tex) override;
  void image_unbind_all() override;

  void uniform_buffer_bind(WGUniformBuf *uniform_buffer, int slot);
  void uniform_buffer_unbind(WGUniformBuf *uniform_buffer);
  void storage_buffer_bind(const WGBufferBinding &binding, int slot);
  void storage_buffer_bind(WGUniformBuf *uniform_buffer, int slot);
  void storage_buffer_unbind(WGObjectID buffer);
};

}  // namespace blender::gpu
