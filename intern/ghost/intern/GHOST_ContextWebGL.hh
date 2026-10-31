/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Drawing context of the OpenGL back-end on WebGL2 (OpenGL ES 3.0).
 *
 * The WebGL context belongs to the thread that creates it, Blender's main thread, which never
 * returns to the browser's event loop. It draws into an OffscreenCanvas created by that thread;
 * each frame is handed to the page as an ImageBitmap (`Module.webglPresent` in
 * `web/shell/blender_loader.js`), see `web/prototypes/webgl_pthread`.
 *
 * WebGL has no shared contexts: all GHOST contexts (the window's and the offscreen ones) use the
 * same WebGL context, so GPU resources are visible to all of them. They can only be used on the
 * thread owning it.
 */

#pragma once

#include "GHOST_Context.hh"

#include <functional>

class GHOST_ContextWebGL : public GHOST_Context {
 public:
  /** Gets the size of the page's canvas, in device pixels. */
  using CanvasSizeFn = std::function<void(uint32_t &width, uint32_t &height)>;

  /**
   * \param canvas_size: size of the canvas, the context's default frame-buffer follows it.
   * \param presents: the window's context, which hands frames to the page on swap.
   */
  GHOST_ContextWebGL(const GHOST_ContextParams &context_params,
                     CanvasSizeFn canvas_size,
                     bool presents);
  ~GHOST_ContextWebGL() override;

  GHOST_TSuccess swapBufferAcquire() override;
  GHOST_TSuccess swapBufferRelease() override;
  GHOST_TSuccess activateDrawingContext() override;
  GHOST_TSuccess releaseDrawingContext() override;
  GHOST_TSuccess initializeDrawingContext() override;
  GHOST_TSuccess releaseNativeHandles() override;
  GHOST_TSuccess setSwapInterval(int interval) override;
  GHOST_TSuccess getSwapInterval(int &interval_out) override;

 private:
  CanvasSizeFn canvas_size_;
  bool presents_;
  /** The context counts as a user of the shared WebGL context (initialized successfully). */
  bool uses_shared_context_ = false;
  int swap_interval_ = 1;
};
