/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_framebuffer_private.hh"

#include "wg_commands.hh"

namespace blender::gpu {

class WGTexture;
struct WGRenderPipelineKey;

/**
 * A framebuffer records a render pass when drawn in. The pass ends when the framebuffer is
 * unbound or its attachments change. Clears before the first draw are load operations of the
 * pass, later or scissored clears are draws (#WGCmdClearAttachments).
 */
class WGFrameBuffer : public FrameBuffer {
  /** Render pass being recorded, 0 when none. */
  uint32_t pass_id_ = 0;

  /** Load operations of the next render pass. */
  WGPULoadOp color_load_ops_[WG_MAX_COLOR_TARGETS];
  double4 clear_colors_[WG_MAX_COLOR_TARGETS];
  WGPULoadOp depth_load_op_ = WGPULoadOp_Load;
  WGPULoadOp stencil_load_op_ = WGPULoadOp_Load;
  WGPUStoreOp color_store_ops_[WG_MAX_COLOR_TARGETS];
  WGPUStoreOp depth_store_op_ = WGPUStoreOp_Store;
  float clear_depth_ = 1.0f;
  uint32_t clear_stencil_ = 0;

  /** Color attachments read as textures by sub-passes, excluded from the render pass. */
  uint32_t color_read_mask_ = 0;

  bool srgb_ = false;
  bool enabled_srgb_ = false;

 public:
  WGFrameBuffer(const char *name);
  ~WGFrameBuffer() override;

  void bind(bool enabled_srgb) override;
  bool check(char err_out[256]) override;
  void clear(GPUFrameBufferBits buffers,
             const double4 clear_color,
             float clear_depth,
             uint clear_stencil) override;
  void clear_multi(Span<double4> clear_colors) override;
  void clear_attachment(GPUAttachmentType type, const double4 clear_value) override;
  void attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls) override;
  void read(GPUFrameBufferBits planes,
            eGPUDataFormat format,
            const int area[4],
            int channel_len,
            int slot,
            void *r_data) override;
  void blit_to(GPUFrameBufferBits planes,
               int src_slot,
               FrameBuffer *dst,
               int dst_slot,
               int dst_offset_x,
               int dst_offset_y) override;

  /** Record the beginning of the render pass if needed, returns the pass id. */
  uint32_t render_pass_ensure();
  /** Record the end of the render pass if one is active. */
  void render_pass_end();

  /** Fill the attachment formats of a pipeline key. */
  void pipeline_formats_get(WGRenderPipelineKey &r_key) const;
  /** Viewport and scissor of draws, in framebuffer pixels. */
  void viewport_get_wg(float r_viewport[4]) const;
  void scissor_get_wg(uint32_t r_scissor[4]) const;

  WGTexture *color_texture(int slot) const;
  WGTexture *depth_texture() const;

  bool srgb_get() const
  {
    return srgb_ && enabled_srgb_;
  }
  void srgb_set(bool srgb)
  {
    srgb_ = srgb;
  }

 protected:
  void subpass_transition_impl(const GPUAttachmentState depth_attachment_state,
                               Span<GPUAttachmentState> color_attachment_states) override;

 private:
  void load_ops_reset();
  /** Clear with a draw, inside the current render pass. */
  void clear_by_draw(uint32_t color_mask,
                     Span<double4> clear_colors,
                     bool clear_depth,
                     float depth,
                     bool clear_stencil,
                     uint32_t stencil);
  bool scissor_covers_framebuffer() const;
  bool has_pending_clears() const;
};

}  // namespace blender::gpu
