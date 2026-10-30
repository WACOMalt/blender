/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "wg_state.hh"
#include "wg_backend.hh"
#include "wg_common.hh"
#include "wg_misc.hh"
#include "wg_texture.hh"

namespace blender::gpu {

void WGStateManager::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  if (unit < 0 || unit >= WG_MAX_TEXTURE_UNITS) {
    return;
  }
  textures[unit] = {static_cast<WGTexture *>(tex), sampler};
}

void WGStateManager::texture_unbind(Texture *tex)
{
  for (TextureBinding &binding : textures) {
    if (binding.texture == tex) {
      binding = {};
    }
  }
}

void WGStateManager::texture_unbind_all()
{
  textures.fill({});
}

void WGStateManager::image_bind(Texture *tex, int unit)
{
  if (unit < 0 || unit >= WG_MAX_IMAGE_UNITS) {
    return;
  }
  images[unit] = static_cast<WGTexture *>(tex);
}

void WGStateManager::image_unbind(Texture *tex)
{
  for (WGTexture *&image : images) {
    if (image == tex) {
      image = nullptr;
    }
  }
}

void WGStateManager::image_unbind_all()
{
  images.fill(nullptr);
}

void WGStateManager::uniform_buffer_bind(WGUniformBuf *uniform_buffer, int slot)
{
  if (slot >= 0 && slot < WG_MAX_BUFFER_SLOTS) {
    uniform_buffers[slot] = {uniform_buffer, {}};
  }
}

void WGStateManager::uniform_buffer_unbind(WGUniformBuf *uniform_buffer)
{
  for (UniformBinding &binding : uniform_buffers) {
    if (binding.uniform_buffer == uniform_buffer) {
      binding = {};
    }
  }
}

void WGStateManager::storage_buffer_bind(const WGBufferBinding &buffer, int slot)
{
  if (slot >= 0 && slot < WG_MAX_BUFFER_SLOTS) {
    storage_buffers[slot] = {nullptr, buffer};
  }
}

void WGStateManager::storage_buffer_bind(WGUniformBuf *uniform_buffer, int slot)
{
  if (slot >= 0 && slot < WG_MAX_BUFFER_SLOTS) {
    storage_buffers[slot] = {uniform_buffer, {}};
  }
}

void WGStateManager::storage_buffer_unbind(WGObjectID buffer)
{
  for (UniformBinding &binding : storage_buffers) {
    if (binding.uniform_buffer == nullptr && binding.buffer.buffer == buffer) {
      binding = {};
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Fences
 * \{ */

static void wg_wait_for_device()
{
  WGCommandStream stream;
  {
    WGRecord record;
    wg_recorder().take(stream);
  }
  /* An empty stream still waits for all previously submitted ones. */
  WGBackend::get().device.submit_and_wait(stream);
}

void WGFence::signal()
{
  signalled_ = true;
}

void WGFence::wait()
{
  if (signalled_) {
    wg_wait_for_device();
    signalled_ = false;
  }
}

void WGWorkInFlight::end_work() {}

/** \} */

}  // namespace blender::gpu
