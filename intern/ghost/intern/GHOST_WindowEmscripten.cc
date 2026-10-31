/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_WindowEmscripten.hh"
#include "GHOST_ContextNone.hh"
#ifdef WITH_WEBGPU_BACKEND
#  include "GHOST_ContextWebGPU.hh"
#endif
#ifdef WITH_OPENGL_BACKEND
#  include "GHOST_ContextWebGL.hh"
#endif
#include "GHOST_SystemEmscripten.hh"

#include <cmath>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

GHOST_WindowEmscripten::GHOST_WindowEmscripten(GHOST_SystemEmscripten *system,
                                               const char *title,
                                               GHOST_TDrawingContextType type,
                                               const GHOST_ContextParams &context_params)
    : GHOST_Window(0, 0, GHOST_kWindowStateMaximized, context_params, false), system_(system)
{
  valid_ = setDrawingContextType(type) == GHOST_kSuccess;
  setTitle(title);
}

GHOST_WindowEmscripten::~GHOST_WindowEmscripten()
{
  releaseNativeHandles();
}

bool GHOST_WindowEmscripten::getValid() const
{
  return GHOST_Window::getValid() && valid_;
}

GHOST_Context *GHOST_WindowEmscripten::newDrawingContext(GHOST_TDrawingContextType type)
{
  switch (type) {
    case GHOST_kDrawingContextTypeNone: {
      GHOST_Context *context = new GHOST_ContextNone(want_context_params_);
      if (context->initializeDrawingContext()) {
        return context;
      }
      delete context;
      return nullptr;
    }
#ifdef WITH_WEBGPU_BACKEND
    case GHOST_kDrawingContextTypeWebGPU: {
      GHOST_Context *context = new GHOST_ContextWebGPU(want_context_params_);
      if (context->initializeDrawingContext()) {
        return context;
      }
      delete context;
      return nullptr;
    }
#endif
#ifdef WITH_OPENGL_BACKEND
    case GHOST_kDrawingContextTypeOpenGL: {
      GHOST_SystemEmscripten *system = system_;
      GHOST_Context *context = new GHOST_ContextWebGL(
          want_context_params_,
          [system](uint32_t &width, uint32_t &height) { system->getCanvasSize(width, height); },
          true);
      if (context->initializeDrawingContext()) {
        return context;
      }
      delete context;
      return nullptr;
    }
#endif
    default:
      return nullptr;
  }
}

void GHOST_WindowEmscripten::setTitle(const char *title)
{
  title_ = title;
  MAIN_THREAD_EM_ASM({ document.title = UTF8ToString($0); }, title_.c_str());
}

std::string GHOST_WindowEmscripten::getTitle() const
{
  return title_;
}

void GHOST_WindowEmscripten::getWindowBounds(GHOST_Rect &bounds) const
{
  getClientBounds(bounds);
}

void GHOST_WindowEmscripten::getClientBounds(GHOST_Rect &bounds) const
{
  uint32_t width, height;
  system_->getCanvasSize(width, height);
  bounds.set(0, 0, int32_t(width), int32_t(height));
}

/* The canvas size follows the page, it can't be changed by Blender. */
GHOST_TSuccess GHOST_WindowEmscripten::setClientWidth(uint32_t /*width*/)
{
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowEmscripten::setClientHeight(uint32_t /*height*/)
{
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowEmscripten::setClientSize(uint32_t /*width*/, uint32_t /*height*/)
{
  return GHOST_kFailure;
}

GHOST_TWindowState GHOST_WindowEmscripten::getState() const
{
  return GHOST_kWindowStateMaximized;
}

GHOST_TSuccess GHOST_WindowEmscripten::setState(GHOST_TWindowState state)
{
  /* Full-screen needs a user gesture in browsers, the page provides a button for it. */
  return (state == GHOST_kWindowStateMaximized) ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowEmscripten::invalidate()
{
  /* Blender redraws continuously while the window needs it, nothing to request. */
  return GHOST_kSuccess;
}

uint16_t GHOST_WindowEmscripten::getDPIHint()
{
  /* HiDPI is handled like on Windows/X11: the canvas is in device pixels, the UI is scaled. */
  return uint16_t(std::lround(96.0f * system_->getDevicePixelRatio()));
}

static const char *web_css_cursor(GHOST_TStandardCursor shape)
{
  switch (shape) {
    case GHOST_kStandardCursorDefault:
    case GHOST_kStandardCursorRightArrow:
    case GHOST_kStandardCursorLeftArrow:
      return "default";
    case GHOST_kStandardCursorInfo:
    case GHOST_kStandardCursorHelp:
      return "help";
    case GHOST_kStandardCursorWait:
      return "wait";
    case GHOST_kStandardCursorText:
      return "text";
    case GHOST_kStandardCursorCrosshair:
    case GHOST_kStandardCursorCrosshairA:
    case GHOST_kStandardCursorCrosshairB:
    case GHOST_kStandardCursorCrosshairC:
    case GHOST_kStandardCursorPencil:
    case GHOST_kStandardCursorEraser:
    case GHOST_kStandardCursorKnife:
    case GHOST_kStandardCursorEyedropper:
    case GHOST_kStandardCursorBlade:
      return "crosshair";
    case GHOST_kStandardCursorUpArrow:
    case GHOST_kStandardCursorDownArrow:
    case GHOST_kStandardCursorNSScroll:
    case GHOST_kStandardCursorUpDown:
    case GHOST_kStandardCursorVerticalSplit:
      return "ns-resize";
    case GHOST_kStandardCursorHorizontalSplit:
    case GHOST_kStandardCursorEWScroll:
    case GHOST_kStandardCursorLeftRight:
    case GHOST_kStandardCursorSlip:
      return "ew-resize";
    case GHOST_kStandardCursorZoomIn:
      return "zoom-in";
    case GHOST_kStandardCursorZoomOut:
      return "zoom-out";
    case GHOST_kStandardCursorMove:
    case GHOST_kStandardCursorNSEWScroll:
      return "move";
    case GHOST_kStandardCursorStop:
    case GHOST_kStandardCursorDestroy:
      return "not-allowed";
    case GHOST_kStandardCursorTopSide:
      return "n-resize";
    case GHOST_kStandardCursorBottomSide:
      return "s-resize";
    case GHOST_kStandardCursorLeftSide:
      return "w-resize";
    case GHOST_kStandardCursorRightSide:
      return "e-resize";
    case GHOST_kStandardCursorTopLeftCorner:
      return "nw-resize";
    case GHOST_kStandardCursorTopRightCorner:
      return "ne-resize";
    case GHOST_kStandardCursorBottomRightCorner:
      return "se-resize";
    case GHOST_kStandardCursorBottomLeftCorner:
      return "sw-resize";
    case GHOST_kStandardCursorCopy:
      return "copy";
    case GHOST_kStandardCursorHandOpen:
      return "grab";
    case GHOST_kStandardCursorHandClosed:
      return "grabbing";
    case GHOST_kStandardCursorHandPoint:
      return "pointer";
    default:
      return nullptr;
  }
}

void GHOST_WindowEmscripten::setCanvasCursor(const char *css_cursor)
{
  MAIN_THREAD_ASYNC_EM_ASM(
      { document.querySelector('#canvas').style.cursor = UTF8ToString($0); }, css_cursor);
}

GHOST_TSuccess GHOST_WindowEmscripten::hasCursorShape(GHOST_TStandardCursor shape)
{
  return web_css_cursor(shape) ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowEmscripten::setWindowCursorShape(GHOST_TStandardCursor shape)
{
  const char *css_cursor = web_css_cursor(shape);
  if (css_cursor == nullptr) {
    return GHOST_kFailure;
  }
  cursor_shape_ = shape;
  if (cursor_visible_) {
    setCanvasCursor(css_cursor);
  }
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowEmscripten::setWindowCustomCursorShape(const uint8_t * /*bitmap*/,
                                                                  const uint8_t * /*mask*/,
                                                                  const int /*size*/[2],
                                                                  const int /*hot_spot*/[2],
                                                                  bool /*can_invert_color*/)
{
  /* TODO: CSS cursors from a data URL. */
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_WindowEmscripten::setWindowCursorVisibility(bool visible)
{
  cursor_visible_ = visible;
  setCanvasCursor(visible ? web_css_cursor(cursor_shape_) : "none");
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowEmscripten::setWindowCursorGrab(GHOST_TGrabCursorMode mode)
{
  /* Continuous grabs use the Pointer Lock API: the browser hides the cursor and reports
   * relative motion, the system accumulates it (see #GHOST_SystemEmscripten::processWebEvent).
   * Blender draws a software cursor for the modes that show one, as the warp capability is
   * not available. Locking needs a recent user gesture, which the press starting the grab is;
   * otherwise the request is deferred to the next input event. */
  switch (mode) {
    case GHOST_kGrabWrap:
    case GHOST_kGrabHide:
      system_->setPointerLockWanted(true);
      emscripten_request_pointerlock("#canvas", true);
      break;
    case GHOST_kGrabNormal:
      /* Confining the cursor to the window is not possible in a browser. */
      break;
    case GHOST_kGrabDisable:
      system_->setPointerLockWanted(false);
      emscripten_exit_pointerlock();
      break;
  }
  return GHOST_kSuccess;
}
