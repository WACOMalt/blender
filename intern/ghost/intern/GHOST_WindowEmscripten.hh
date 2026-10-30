/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_WindowEmscripten class: the page's canvas.
 */

#pragma once

#include "GHOST_Window.hh"

class GHOST_SystemEmscripten;

class GHOST_WindowEmscripten : public GHOST_Window {
 public:
  GHOST_WindowEmscripten(GHOST_SystemEmscripten *system,
                         const char *title,
                         GHOST_TDrawingContextType type,
                         const GHOST_ContextParams &context_params);
  ~GHOST_WindowEmscripten() override;

  bool getValid() const override;

  void setTitle(const char *title) override;
  std::string getTitle() const override;
  void setPath(const char * /*filepath*/) override {}

  void getWindowBounds(GHOST_Rect &bounds) const override;
  void getClientBounds(GHOST_Rect &bounds) const override;
  GHOST_TSuccess setClientWidth(uint32_t width) override;
  GHOST_TSuccess setClientHeight(uint32_t height) override;
  GHOST_TSuccess setClientSize(uint32_t width, uint32_t height) override;

  /* The canvas is at the origin of the "screen". */
  void screenToClient(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override
  {
    outX = inX;
    outY = inY;
  }
  void clientToScreen(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override
  {
    outX = inX;
    outY = inY;
  }

  GHOST_TWindowState getState() const override;
  GHOST_TSuccess setState(GHOST_TWindowState state) override;
  GHOST_TSuccess setOrder(GHOST_TWindowOrder /*order*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess invalidate() override;

  uint16_t getDPIHint() override;

  GHOST_TSuccess hasCursorShape(GHOST_TStandardCursor shape) override;

 protected:
  GHOST_TSuccess setWindowCursorGrab(GHOST_TGrabCursorMode mode) override;
  GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor shape) override;
  GHOST_TSuccess setWindowCustomCursorShape(const uint8_t *bitmap,
                                            const uint8_t *mask,
                                            const int size[2],
                                            const int hot_spot[2],
                                            bool can_invert_color) override;
  GHOST_TSuccess setWindowCursorVisibility(bool visible) override;

 private:
  GHOST_Context *newDrawingContext(GHOST_TDrawingContextType type) override;
  /** Set the CSS cursor of the canvas, `css_cursor` is a static string. */
  void setCanvasCursor(const char *css_cursor);

  GHOST_SystemEmscripten *system_;
  std::string title_;
  bool valid_ = false;
  GHOST_TStandardCursor cursor_shape_ = GHOST_kStandardCursorDefault;
  bool cursor_visible_ = true;
};
