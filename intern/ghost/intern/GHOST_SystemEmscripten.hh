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

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

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
    /** Files dropped on the canvas, written to the file-system by the page. */
    Drop,
    /** The pointer lock (used for cursor grabs) was acquired or released. */
    PointerLock,
  };
  Type type;
  uint64_t time_ms;
  /** Canvas position in device pixels (mouse events). */
  int32_t x, y;
  /** Motion in device pixels, the only position information while the pointer is locked. */
  int32_t dx, dy;
  /** Pointer lock state (#Type::PointerLock). */
  bool locked;
  /** DOM `MouseEvent.button`. */
  int button;
  /** Wheel steps (positive: away from the user, like other GHOST backends). */
  int wheel_x, wheel_y;
  /** DOM `KeyboardEvent.code` (physical key) and `KeyboardEvent.key` (text). */
  char code[32];
  char key[32];
  bool repeat;
  bool shift, ctrl, alt, meta;
  /** File-system paths of dropped files. */
  std::vector<std::string> paths;
};

class GHOST_SystemEmscripten : public GHOST_System {
 public:
  GHOST_SystemEmscripten();
  ~GHOST_SystemEmscripten() override;

  GHOST_TSuccess init() override;

  bool processEvents(bool waitForEvent) override;

  /** The window grabs the cursor with a pointer lock (see #processWebEvent). */
  void setPointerLockWanted(bool wanted)
  {
    pointer_lock_wanted_ = wanted;
  }
  /** Browser's main thread: a pointer lock request failed, retry on the next input event. */
  void onPointerLockError()
  {
    pointer_lock_retry_ = true;
  }
  bool pointerLockRetryNeeded()
  {
    return pointer_lock_wanted_ && pointer_lock_retry_.exchange(false);
  }

  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }
  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override;
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override;
  GHOST_TCapabilityFlag getCapabilities() const override;

  char *getClipboard(bool selection) const override;
  void putClipboard(const char *buffer, bool selection) const override;
  /** Set the text pasted by Blender without writing the browser's clipboard. */
  void putClipboardInternal(const char *text) const
  {
    clipboard_ = text;
  }

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
  /**
   * While the pointer is locked (cursor grab) the browser reports motion only, the position is
   * accumulated: it may leave the canvas, like other platforms' grab with wrapping. The real
   * cursor stays at `lock_x_`, `lock_y_` and reappears there when the lock ends.
   */
  bool pointer_locked_ = false;
  int32_t lock_x_ = 0;
  int32_t lock_y_ = 0;
  GHOST_Buttons buttons_;
  GHOST_ModifierKeys modifier_keys_;

  /* Shared between Blender's thread (the grab) and the browser's main thread (events). */
  std::atomic<bool> pointer_lock_wanted_{false};
  std::atomic<bool> pointer_lock_retry_{false};

  /** Only used inside Blender, the system clipboard needs asynchronous browser APIs. */
  mutable std::string clipboard_;
};
