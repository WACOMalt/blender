/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * The WebGPU device, owned by the browser's main thread, which executes command streams
 * recorded by Blender's threads. See #WGCommandStream.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "BLI_map.hh"

#include "wg_commands.hh"

namespace blender::gpu {

/** Device limits and features, read on Blender's thread after initialization. */
struct WGDeviceInfo {
  WGPULimits limits = {};
  bool float32_filterable = false;
  bool depth32float_stencil8 = false;
  bool dual_source_blending = false;
  bool rg11b10ufloat_renderable = false;
  std::string adapter_vendor;
  std::string adapter_architecture;
  std::string adapter_description;
};

class WGDevice {
 public:
  /** Create the device on the browser's main thread, blocks until done. */
  bool init();
  bool is_initialized() const
  {
    return initialized_;
  }
  const WGDeviceInfo &info() const
  {
    return info_;
  }

  WGObjectID object_id_alloc()
  {
    return next_object_id_.fetch_add(1);
  }

  /** Execute `stream` on the browser's main thread without waiting. The stream is emptied. */
  void submit(WGCommandStream &stream);
  /** Execute `stream` and wait until it is done, including reading back data to host memory. */
  void submit_and_wait(WGCommandStream &stream);

  /** Format of the canvas, valid after #init. */
  WGPUTextureFormat surface_format() const
  {
    return surface_format_;
  }

  /* -------------------------------------------------------------------- */
  /** \name Main Thread
   * \{ */

  /** Execute the commands, called on the browser's main thread. */
  void execute(WGCommandStream &stream, void *wait_ctx);

  /* Initialization callbacks (main thread). */
  void init_on_main_thread(void *ctx);
  void on_adapter(WGPUAdapter adapter, void *ctx);
  void on_device(WGPUDevice device, void *ctx);

  /** \} */

 private:
  struct TextureEntry {
    WGPUTexture texture = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    WGPUTextureDimension dimension = WGPUTextureDimension_2D;
    uint32_t mip_count = 1;
    uint32_t layers = 1;
    uint32_t width = 1, height = 1;
    uint32_t sample_count = 1;
  };

  struct ExecuteState;

  WGPUTextureView texture_view_get(const WGObjectID texture,
                                   WGPUTextureViewDimension dimension,
                                   WGPUTextureFormat format,
                                   WGPUTextureAspect aspect,
                                   uint32_t base_mip,
                                   uint32_t mip_count,
                                   uint32_t base_layer,
                                   uint32_t layer_count,
                                   ExecuteState &state);
  WGPUTexture texture_get(const WGObjectID texture, ExecuteState &state);
  WGPUBuffer buffer_get(const WGObjectID buffer, const ExecuteState &state);
  WGPUBindGroup bind_group_create(WGObjectID pipeline_layout,
                                  uint32_t group,
                                  const WGBindingResource *resources,
                                  uint32_t resources_len,
                                  ExecuteState &state);

  void execute_create_render_pipeline(const WGCmdCreateRenderPipeline &cmd, const uint8_t *trailing);
  /** Make `pass_id` the active render pass, resuming it if needed. */
  bool render_pass_activate(uint32_t pass_id, ExecuteState &state);
  void execute_draw(const WGCmdDraw &cmd, const uint8_t *trailing, ExecuteState &state);
  void execute_dispatch(const WGCmdDispatch &cmd, const uint8_t *trailing, ExecuteState &state);
  void execute_clear_attachments(const WGCmdClearAttachments &cmd, ExecuteState &state);
  void execute_present(const WGCmdPresent &cmd, ExecuteState &state);
  WGPUShaderModule shader_module_create(const std::string &wgsl);
  void destroy_object(WGObjectID id);

  bool initialized_ = false;
  WGDeviceInfo info_;
  std::atomic<WGObjectID> next_object_id_ = 1;
  /** Number of submitted streams not yet executed, to limit how far Blender gets ahead. */
  std::atomic<int> streams_in_flight_ = 0;

  /* Main thread only. */
  WGPUInstance instance_ = nullptr;
  WGPUAdapter adapter_ = nullptr;
  WGPUDevice device_ = nullptr;
  WGPUQueue queue_ = nullptr;
  WGPUSurface surface_ = nullptr;
  WGPUTextureFormat surface_format_ = WGPUTextureFormat_BGRA8Unorm;
  uint32_t surface_width_ = 0, surface_height_ = 0;

  /** Render passes begun and not ended yet, see #WGCmdBeginRenderPass. */
  Map<uint32_t, WGCmdBeginRenderPass> render_passes_;

  Map<WGObjectID, WGPUBuffer> buffers_;
  Map<WGObjectID, TextureEntry> textures_;
  Map<WGObjectID, WGPUSampler> samplers_;
  Map<WGObjectID, WGPUShaderModule> shader_modules_;
  Map<WGObjectID, WGPUBindGroupLayout> bind_group_layouts_;
  struct PipelineLayoutEntry {
    WGPUPipelineLayout layout;
    WGPUBindGroupLayout groups[2];
  };
  Map<WGObjectID, PipelineLayoutEntry> pipeline_layouts_;
  Map<WGObjectID, WGPURenderPipeline> render_pipelines_;
  Map<WGObjectID, WGPUComputePipeline> compute_pipelines_;

  /** Pipelines of #execute_clear_attachments, keyed by the pass formats. */
  Map<std::string, WGPURenderPipeline> clear_pipelines_;
  WGPURenderPipeline present_pipeline_ = nullptr;
};

}  // namespace blender::gpu
