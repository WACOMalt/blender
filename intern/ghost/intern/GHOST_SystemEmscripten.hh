/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_SystemEmscripten class: a web page canvas as the only window.
 *
 * Blender's `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`), while DOM events are delivered to
 * the browser's main thread. Event callbacks run on the main thread and only push a copy of the
 * event into a queue, #processEvents converts them to GHOST events on Blender's thread.
 */

#pragma once

#include "../GHOST_Types.hh"
#include "GHOST_System.hh"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

class GHOST_WindowEmscripten;

/** A DOM event, copied on the browser's main thread. */
struct GHOST_WebEvent {
  enum class Type {
    MouseMove,
    MouseDown,
    MouseUp,
    Wheel,
    KeyDown,
    KeyUp,
    Resize,
    Focus,
    Blur,
  };
  Type type;
  uint64_t time_ms;
  /** Canvas position in device pixels (mouse events). */
  int32_t x, y;
  /** DOM `MouseEvent.button`. */
  int button;
  /** Wheel steps (positive: away from the user, like other GHOST backends). */
  int wheel_x, wheel_y;
  /** DOM `KeyboardEvent.code` (physical key) and `KeyboardEvent.key` (text). */
  char code[32];
  char key[32];
  bool repeat;
  bool shift, ctrl, alt, meta;
};

class GHOST_SystemEmscripten : public GHOST_System {
 public:
  GHOST_SystemEmscripten();
  ~GHOST_SystemEmscripten() override;

  GHOST_TSuccess init() override;

  bool processEvents(bool waitForEvent) override;

  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }
  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override;
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override;
  GHOST_TCapabilityFlag getCapabilities() const override;

  char *getClipboard(bool selection) const override;
  void putClipboard(const char *buffer, bool selection) const override;

  uint64_t getMilliSeconds() const override;

  uint8_t getNumDisplays() const override
  {
    return 1;
  }
  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override;
  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override;

  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override;
  GHOST_TSuccess setCursorPosition(int32_t x, int32_t y) override;

  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpu_settings) override;
  GHOST_TSuccess disposeContext(GHOST_IContext *context) override;

  GHOST_IWindow *createWindow(const char *title,
                              int32_t left,
                              int32_t top,
                              uint32_t width,
                              uint32_t height,
                              GHOST_TWindowState state,
                              GHOST_GPUSettings gpu_settings,
                              const bool exclusive = false,
                              const bool is_dialog = false,
                              const GHOST_IWindow *parent_window = nullptr) override;

  GHOST_IWindow *getWindowUnderCursor(int32_t x, int32_t y) override;
  GHOST_TSuccess disposeWindow(GHOST_IWindow *window) override;

  /** Size of the canvas in device pixels. */
  void getCanvasSize(uint32_t &width, uint32_t &height) const;
  /** Device pixel ratio of the page (`window.devicePixelRatio`). */
  float getDevicePixelRatio() const
  {
    return device_pixel_ratio_;
  }

  /** Called on the browser's main thread by the DOM event callbacks. */
  void queueWebEvent(const GHOST_WebEvent &event);
  /** Read the canvas size from the page (browser's main thread). */
  void updateCanvasSizeOnMainThread();

 private:
  void processWebEvent(const GHOST_WebEvent &event);

  /** The page has a single canvas, so there is at most one window. */
  GHOST_WindowEmscripten *window_ = nullptr;

  std::mutex web_events_mutex_;
  std::condition_variable web_events_cond_;
  std::deque<GHOST_WebEvent> web_events_;

  /* Written by the browser's main thread, under `web_events_mutex_`. */
  uint32_t canvas_width_ = 1280;
  uint32_t canvas_height_ = 720;
  float device_pixel_ratio_ = 1.0f;

  /* State as seen by Blender's thread, updated by #processWebEvent. */
  int32_t cursor_x_ = 0;
  int32_t cursor_y_ = 0;
  GHOST_Buttons buttons_;
  GHOST_ModifierKeys modifier_keys_;

  /** Only used inside Blender, the system clipboard needs asynchronous browser APIs. */
  mutable std::string clipboard_;
};
