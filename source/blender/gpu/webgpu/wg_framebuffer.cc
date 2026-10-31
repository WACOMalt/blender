/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_math_base.hh"
#include "BLI_string.hh"

#include "GPU_texture.hh"

#include "wg_common.hh"
#include "wg_context.hh"
#include <cmath>

#include "wg_framebuffer.hh"
#include "wg_shader.hh"
#include "wg_texture.hh"

namespace blender::gpu {

WGFrameBuffer::WGFrameBuffer(const char *name) : FrameBuffer(name)
{
  load_ops_reset();
}

WGFrameBuffer::~WGFrameBuffer()
{
  render_pass_end();
  WGContext *context = WGContext::get();
  if (context && context->active_fb == this) {
    context->active_fb = nullptr;
  }
}

void WGFrameBuffer::load_ops_reset()
{
  for (int i = 0; i < WG_MAX_COLOR_TARGETS; i++) {
    color_load_ops_[i] = WGPULoadOp_Load;
    color_store_ops_[i] = WGPUStoreOp_Store;
    clear_colors_[i] = double4(0.0);
  }
  depth_load_op_ = WGPULoadOp_Load;
  stencil_load_op_ = WGPULoadOp_Load;
  depth_store_op_ = WGPUStoreOp_Store;
}

WGTexture *WGFrameBuffer::color_texture(int slot) const
{
  return static_cast<WGTexture *>(color_tex(slot));
}

WGTexture *WGFrameBuffer::depth_texture() const
{
  return static_cast<WGTexture *>(depth_tex());
}

/* -------------------------------------------------------------------- */
/** \name Binding and Render Passes
 * \{ */

void WGFrameBuffer::bind(bool enabled_srgb)
{
  WGContext *context = WGContext::get();
  if (context->active_fb != this) {
    if (WGFrameBuffer *previous = static_cast<WGFrameBuffer *>(context->active_fb)) {
      previous->render_pass_end();
    }
    context->active_fb = this;
  }
  if (dirty_attachments_) {
    /* The size comes from the attachments. */
    for (const GPUAttachment &attachment : attachments_) {
      if (attachment.tex) {
        int size[3];
        GPU_texture_get_mipmap_size(attachment.tex, attachment.mip, size);
        size_set(size[0], size[1]);
        break;
      }
    }
    dirty_attachments_ = false;
    render_pass_record_end();
  }
  viewport_reset();
  scissor_reset();
  if (use_explicit_load_store_) {
    /* Load operations are set after binding, they apply to a new pass. */
    render_pass_end();
  }
  srgb_ = false;
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    if (WGTexture *texture = color_texture(slot)) {
      srgb_ = (texture->format_flag_get() & GPU_FORMAT_SRGB) != 0;
      break;
    }
  }
  if (enabled_srgb_ != enabled_srgb && srgb_) {
    /* The attachment views change. */
    render_pass_end();
  }
  enabled_srgb_ = enabled_srgb;
  Shader::set_framebuffer_srgb_target(enabled_srgb && srgb_);

  if (dirty_state_) {
    dirty_state_ = false;
  }
}

bool WGFrameBuffer::check(char err_out[256])
{
  bool has_attachment = false;
  for (const GPUAttachment &attachment : attachments_) {
    has_attachment |= attachment.tex != nullptr;
  }
  if (!has_attachment && (width_ == 0 || height_ == 0)) {
    BLI_snprintf(err_out, 256, "Framebuffer '%s' has no attachments", name_);
    return false;
  }
  return true;
}

uint32_t WGFrameBuffer::render_pass_ensure()
{
  if (dirty_attachments_) {
    dirty_attachments_ = false;
    render_pass_record_end();
  }
  if (pass_id_ != 0) {
    return pass_id_;
  }

  WGCmdBeginRenderPass cmd = {};
  bool has_attachment = false;
  uint32_t non_finite_clears = 0;
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    const GPUAttachment &attachment = attachments_[GPU_FB_COLOR_ATTACHMENT0 + slot];
    WGTexture *texture = static_cast<WGTexture *>(attachment.tex);
    if (texture == nullptr || !texture->is_usable() || (color_read_mask_ & (1u << slot))) {
      continue;
    }
    WGAttachment &color = cmd.color[slot];
    color.texture = texture->id_get();
    color.view_format = attachment_view_format(texture);
    color.mip = uint32_t(texture->view_mip_start() + attachment.mip);
    color.layer = uint32_t(texture->view_layer_start() + max_ii(attachment.layer, 0));
    color.load_op = color_load_ops_[slot];
    color.store_op = color_store_ops_[slot];
    for (int i = 0; i < 4; i++) {
      color.clear_color[i] = clear_colors_[slot][i];
      /* The browser rejects non-finite clear values, these slots are cleared by a draw below. */
      if (color.load_op == WGPULoadOp_Clear && !std::isfinite(color.clear_color[i])) {
        color.clear_color[i] = 0.0;
        non_finite_clears |= 1u << slot;
      }
    }
    cmd.color_len = slot + 1;
    has_attachment = true;
  }
  const GPUAttachment &depth_attachment = this->depth_attachment();
  WGTexture *depth = static_cast<WGTexture *>(depth_attachment.tex);
  if (depth && depth->is_usable()) {
    cmd.depth.texture = depth->id_get();
    cmd.depth.view_format = depth->wg_format_get();
    cmd.depth.mip = uint32_t(depth->view_mip_start() + depth_attachment.mip);
    cmd.depth.layer = uint32_t(depth->view_layer_start() + max_ii(depth_attachment.layer, 0));
    cmd.depth.load_op = depth_load_op_;
    cmd.depth.store_op = depth_store_op_;
    cmd.clear_depth = clear_depth_;
    cmd.has_stencil = wg_texture_format_has_stencil(depth->wg_format_get());
    cmd.stencil_load_op = stencil_load_op_;
    cmd.stencil_store_op = depth_store_op_;
    cmd.clear_stencil = clear_stencil_;
    has_attachment = true;
  }
  if (!has_attachment) {
    /* WebGPU render passes need an attachment. */
    return 0;
  }

  const double4 clear_colors[WG_MAX_COLOR_TARGETS] = {clear_colors_[0],
                                                       clear_colors_[1],
                                                       clear_colors_[2],
                                                       clear_colors_[3],
                                                       clear_colors_[4],
                                                       clear_colors_[5],
                                                       clear_colors_[6],
                                                       clear_colors_[7]};
  {
    WGRecord record;
    pass_id_ = wg_recorder().next_pass_id++;
    cmd.pass_id = pass_id_;
    record.stream.push(WGCommandType::BeginRenderPass, cmd);
  }
  load_ops_reset();
  if (non_finite_clears != 0) {
    /* Like the load operation, the clear covers the whole attachment. */
    const bool scissor_test = scissor_test_;
    scissor_test_ = false;
    clear_by_draw(non_finite_clears,
                  Span<double4>(clear_colors, WG_MAX_COLOR_TARGETS),
                  false,
                  0.0f,
                  false,
                  0);
    scissor_test_ = scissor_test;
  }
  return pass_id_;
}

bool WGFrameBuffer::has_pending_clears() const
{
  bool has_clear = depth_load_op_ == WGPULoadOp_Clear || stencil_load_op_ == WGPULoadOp_Clear;
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    has_clear |= color_load_ops_[slot] == WGPULoadOp_Clear;
  }
  return has_clear;
}

void WGFrameBuffer::render_pass_end()
{
  if (pass_id_ == 0 && has_pending_clears()) {
    /* Nothing was drawn, the clears still need to happen. */
    render_pass_ensure();
  }
  render_pass_record_end();
}

void WGFrameBuffer::render_pass_record_end()
{
  if (pass_id_ == 0) {
    return;
  }
  WGRecord record;
  record.stream.push(WGCommandType::EndRenderPass, WGCmdEndRenderPass{pass_id_});
  pass_id_ = 0;
}

WGPUTextureFormat WGFrameBuffer::attachment_view_format(const WGTexture *texture) const
{
  /* Without sRGB, sRGB textures are written as is. */
  if (!enabled_srgb_ && texture->wg_format_get() == WGPUTextureFormat_RGBA8UnormSrgb) {
    return WGPUTextureFormat_RGBA8Unorm;
  }
  return texture->wg_format_get();
}

void WGFrameBuffer::subpass_transition_impl(const GPUAttachmentState /*depth_attachment_state*/,
                                            Span<GPUAttachmentState> color_attachment_states)
{
  /* Attachments read by the fragment shader can't be attachments of the pass. */
  uint32_t read_mask = 0;
  for (const int i : color_attachment_states.index_range()) {
    if (color_attachment_states[i] == GPU_ATTACHMENT_READ) {
      read_mask |= 1u << i;
    }
  }
  if (read_mask != color_read_mask_) {
    render_pass_end();
    color_read_mask_ = read_mask;
  }
}

void WGFrameBuffer::pipeline_formats_get(WGRenderPipelineKey &r_key) const
{
  r_key.color_len = 0;
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    WGTexture *texture = color_texture(slot);
    r_key.color_formats[slot] = WGPUTextureFormat_Undefined;
    if (texture == nullptr || !texture->is_usable() || (color_read_mask_ & (1u << slot))) {
      continue;
    }
    r_key.color_formats[slot] = attachment_view_format(texture);
    r_key.color_len = slot + 1;
  }
  WGTexture *depth = depth_texture();
  if (depth && depth->is_usable()) {
    r_key.depth_format = depth->wg_format_get();
    r_key.has_stencil = wg_texture_format_has_stencil(r_key.depth_format);
  }
  else {
    r_key.depth_format = WGPUTextureFormat_Undefined;
    r_key.has_stencil = false;
  }
}

void WGFrameBuffer::viewport_get_wg(float r_viewport[4]) const
{
  /* WebGPU requires the viewport to be inside the attachments. */
  const int *viewport = viewport_[0];
  const int x = clamp_i(viewport[0], 0, max_ii(width_ - 1, 0));
  const int y = clamp_i(viewport[1], 0, max_ii(height_ - 1, 0));
  r_viewport[0] = float(x);
  r_viewport[1] = float(y);
  r_viewport[2] = float(max_ii(min_ii(viewport[0] + viewport[2], width_) - x, 1));
  r_viewport[3] = float(max_ii(min_ii(viewport[1] + viewport[3], height_) - y, 1));
}

void WGFrameBuffer::scissor_get_wg(uint32_t r_scissor[4]) const
{
  int rect[4] = {0, 0, width_, height_};
  if (scissor_test_) {
    const int x_min = clamp_i(scissor_[0], 0, width_);
    const int y_min = clamp_i(scissor_[1], 0, height_);
    const int x_max = clamp_i(scissor_[0] + scissor_[2], 0, width_);
    const int y_max = clamp_i(scissor_[1] + scissor_[3], 0, height_);
    rect[0] = x_min;
    rect[1] = y_min;
    rect[2] = max_ii(x_max - x_min, 0);
    rect[3] = max_ii(y_max - y_min, 0);
  }
  for (int i = 0; i < 4; i++) {
    r_scissor[i] = uint32_t(rect[i]);
  }
}

bool WGFrameBuffer::scissor_covers_framebuffer() const
{
  if (!scissor_test_) {
    return true;
  }
  return scissor_[0] <= 0 && scissor_[1] <= 0 && scissor_[0] + scissor_[2] >= width_ &&
         scissor_[1] + scissor_[3] >= height_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clears
 * \{ */

void WGFrameBuffer::clear_by_draw(const uint32_t color_mask,
                                  Span<double4> clear_colors,
                                  const bool clear_depth,
                                  const float depth,
                                  const bool clear_stencil,
                                  const uint32_t stencil)
{
  const uint32_t pass_id = render_pass_ensure();
  if (pass_id == 0) {
    return;
  }
  /* Clear values as bits matching the attachment formats. */
  uint32_t data[WG_CLEAR_DATA_SIZE / 4] = {};
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    if (!(color_mask & (1u << slot))) {
      continue;
    }
    WGTexture *texture = color_texture(slot);
    const double4 &color = clear_colors[clear_colors.size() == 1 ? 0 : slot];
    const bool is_integer = texture && wg_texture_format_is_integer(texture->wg_format_get());
    const bool is_signed = texture && wg_texture_sample_type(texture->device_format_get()) ==
                                          WGPUTextureSampleType_Sint;
    for (int i = 0; i < 4; i++) {
      if (!is_integer) {
        const float value = float(color[i]);
        memcpy(&data[slot * 4 + i], &value, sizeof(float));
      }
      else if (is_signed) {
        const int32_t value = int32_t(color[i]);
        memcpy(&data[slot * 4 + i], &value, sizeof(int32_t));
      }
      else {
        data[slot * 4 + i] = uint32_t(color[i]);
      }
    }
  }
  memcpy(&data[8 * 4], &depth, sizeof(float));

  WGCmdClearAttachments cmd = {};
  cmd.pass_id = pass_id;
  cmd.color_mask = color_mask;
  cmd.clear_depth = clear_depth;
  cmd.clear_stencil = clear_stencil;
  cmd.stencil = stencil;
  scissor_get_wg(cmd.scissor);
  WGRecord record;
  cmd.data_offset = record.stream.staging_push(data, sizeof(data));
  record.stream.push(WGCommandType::ClearAttachments, cmd);
}

void WGFrameBuffer::clear(GPUFrameBufferBits buffers,
                          const double4 clear_color,
                          float clear_depth,
                          uint clear_stencil)
{
  uint32_t color_mask = 0;
  if (buffers & GPU_COLOR_BIT) {
    for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
      if (color_texture(slot)) {
        color_mask |= 1u << slot;
      }
    }
  }
  const bool do_depth = (buffers & GPU_DEPTH_BIT) && depth_texture();
  const bool do_stencil = (buffers & GPU_STENCIL_BIT) && depth_texture();

  if (pass_id_ != 0 || !scissor_covers_framebuffer()) {
    clear_by_draw(color_mask,
                  Span<double4>(&clear_color, 1),
                  do_depth,
                  clear_depth,
                  do_stencil,
                  clear_stencil);
    return;
  }
  /* Clear when the render pass begins. */
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    if (color_mask & (1u << slot)) {
      color_load_ops_[slot] = WGPULoadOp_Clear;
      clear_colors_[slot] = clear_color;
    }
  }
  if (do_depth) {
    depth_load_op_ = WGPULoadOp_Clear;
    clear_depth_ = clear_depth;
  }
  if (do_stencil) {
    stencil_load_op_ = WGPULoadOp_Clear;
    clear_stencil_ = clear_stencil;
  }
}

void WGFrameBuffer::clear_multi(Span<double4> clear_colors)
{
  uint32_t color_mask = 0;
  for (const int slot : clear_colors.index_range()) {
    if (slot < WG_MAX_COLOR_TARGETS && color_texture(slot)) {
      color_mask |= 1u << slot;
    }
  }
  if (pass_id_ != 0 || !scissor_covers_framebuffer()) {
    clear_by_draw(color_mask, clear_colors, false, 0.0f, false, 0);
    return;
  }
  for (int slot = 0; slot < WG_MAX_COLOR_TARGETS; slot++) {
    if (color_mask & (1u << slot)) {
      color_load_ops_[slot] = WGPULoadOp_Clear;
      clear_colors_[slot] = clear_colors[slot];
    }
  }
}

void WGFrameBuffer::clear_attachment(GPUAttachmentType type, const double4 clear_value)
{
  if (type == GPU_FB_DEPTH_ATTACHMENT || type == GPU_FB_DEPTH_STENCIL_ATTACHMENT) {
    clear(type == GPU_FB_DEPTH_STENCIL_ATTACHMENT ? GPU_DEPTH_BIT | GPU_STENCIL_BIT :
                                                    GPU_DEPTH_BIT,
          double4(0.0),
          float(clear_value[0]),
          uint(clear_value[1]));
    return;
  }
  const int slot = type - GPU_FB_COLOR_ATTACHMENT0;
  if (slot < 0 || slot >= WG_MAX_COLOR_TARGETS || color_texture(slot) == nullptr) {
    return;
  }
  if (pass_id_ != 0 || !scissor_covers_framebuffer()) {
    double4 colors[WG_MAX_COLOR_TARGETS];
    colors[slot] = clear_value;
    clear_by_draw(1u << slot, Span<double4>(colors, WG_MAX_COLOR_TARGETS), false, 0.0f, false, 0);
    return;
  }
  color_load_ops_[slot] = WGPULoadOp_Clear;
  clear_colors_[slot] = clear_value;
}

void WGFrameBuffer::attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls)
{
  /* There is no "don't care" load operation, clearing is the fastest option. */
  const WGPULoadOp load_op = ls.load_action == GPU_LOADACTION_LOAD ? WGPULoadOp_Load :
                                                                     WGPULoadOp_Clear;
  const WGPUStoreOp store_op = ls.store_action == GPU_STOREACTION_DONT_CARE ?
                                   WGPUStoreOp_Discard :
                                   WGPUStoreOp_Store;
  render_pass_end();
  if (type == GPU_FB_DEPTH_ATTACHMENT || type == GPU_FB_DEPTH_STENCIL_ATTACHMENT) {
    depth_load_op_ = load_op;
    stencil_load_op_ = load_op;
    depth_store_op_ = store_op;
    clear_depth_ = ls.load_action == GPU_LOADACTION_CLEAR ? float(ls.clear_value[0]) : 1.0f;
    clear_stencil_ = 0;
    return;
  }
  const int slot = type - GPU_FB_COLOR_ATTACHMENT0;
  if (slot >= 0 && slot < WG_MAX_COLOR_TARGETS) {
    color_load_ops_[slot] = load_op;
    color_store_ops_[slot] = store_op;
    clear_colors_[slot] = ls.load_action == GPU_LOADACTION_CLEAR ?
                              double4(ls.clear_value[0],
                                      ls.clear_value[1],
                                      ls.clear_value[2],
                                      ls.clear_value[3]) :
                              double4(0.0);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Read and Blit
 * \{ */

void WGFrameBuffer::read(GPUFrameBufferBits planes,
                         eGPUDataFormat format,
                         const int area[4],
                         int /*channel_len*/,
                         int slot,
                         void *r_data)
{
  WGTexture *texture = (planes & GPU_COLOR_BIT) ? color_texture(slot) : depth_texture();
  if (texture == nullptr) {
    return;
  }
  /* Make sure pending clears and draws are part of the result. */
  render_pass_end();

  const GPUAttachment &attachment = (planes & GPU_COLOR_BIT) ?
                                        attachments_[GPU_FB_COLOR_ATTACHMENT0 + slot] :
                                        depth_attachment();
  const uint32_t offset[3] = {uint32_t(max_ii(area[0], 0)),
                              uint32_t(max_ii(area[1], 0)),
                              uint32_t(texture->view_layer_start() + max_ii(attachment.layer, 0))};
  const uint32_t extent[3] = {uint32_t(area[2]), uint32_t(area[3]), 1};
  texture->read_region(attachment.mip, offset, extent, format, r_data);
}

void WGFrameBuffer::blit_to(GPUFrameBufferBits planes,
                            int src_slot,
                            FrameBuffer *dst,
                            int dst_slot,
                            int dst_offset_x,
                            int dst_offset_y)
{
  WGFrameBuffer *dst_fb = static_cast<WGFrameBuffer *>(dst);
  WGTexture *src_texture = (planes & GPU_COLOR_BIT) ? color_texture(src_slot) : depth_texture();
  WGTexture *dst_texture = (planes & GPU_COLOR_BIT) ? dst_fb->color_texture(dst_slot) :
                                                      dst_fb->depth_texture();
  if (src_texture == nullptr || dst_texture == nullptr || !src_texture->is_usable() ||
      !dst_texture->is_usable())
  {
    return;
  }
  /* TODO: Blits between different formats need a draw. */
  if (src_texture->wg_format_get() != dst_texture->wg_format_get()) {
    return;
  }
  render_pass_end();
  dst_fb->render_pass_end();

  const GPUAttachment &src_attachment = (planes & GPU_COLOR_BIT) ?
                                            attachments_[GPU_FB_COLOR_ATTACHMENT0 + src_slot] :
                                            depth_attachment();
  const GPUAttachment &dst_attachment =
      (planes & GPU_COLOR_BIT) ? dst_fb->attachments_[GPU_FB_COLOR_ATTACHMENT0 + dst_slot] :
                                 dst_fb->depth_attachment();
  const int src_mip = src_texture->view_mip_start() + src_attachment.mip;
  const int dst_mip = dst_texture->view_mip_start() + dst_attachment.mip;
  const int3 src_size = src_texture->mip_size_get(src_attachment.mip);
  const int3 dst_size = dst_texture->mip_size_get(dst_attachment.mip);
  const int width = min_ii(src_size.x, dst_size.x - dst_offset_x);
  const int height = min_ii(src_size.y, dst_size.y - dst_offset_y);
  if (width <= 0 || height <= 0) {
    return;
  }

  WGCmdCopyTextureToTexture cmd = {};
  cmd.src = src_texture->id_get();
  cmd.dst = dst_texture->id_get();
  cmd.src_mip = uint32_t(src_mip);
  cmd.dst_mip = uint32_t(dst_mip);
  cmd.src_offset[2] = uint32_t(src_texture->view_layer_start() + max_ii(src_attachment.layer, 0));
  cmd.dst_offset[0] = uint32_t(dst_offset_x);
  cmd.dst_offset[1] = uint32_t(dst_offset_y);
  cmd.dst_offset[2] = uint32_t(dst_texture->view_layer_start() + max_ii(dst_attachment.layer, 0));
  cmd.extent[0] = uint32_t(width);
  cmd.extent[1] = uint32_t(height);
  cmd.extent[2] = 1;
  WGRecord record;
  record.stream.push(WGCommandType::CopyTextureToTexture, cmd);
}

/** \} */

}  // namespace blender::gpu
