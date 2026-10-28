/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_SystemEmscripten.hh"
#include "GHOST_ContextNone.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventKey.hh"
#include "GHOST_EventWheel.hh"
#include "GHOST_TimerManager.hh"
#include "GHOST_WindowEmscripten.hh"
#include "GHOST_WindowManager.hh"

#include <chrono>
#include <cmath>
#include <cstring>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

/**
 * The page is expected to contain a `<canvas id="canvas">` filling the page from its top left
 * corner (see `web/shell/`), so DOM client coordinates are canvas coordinates.
 */
static const char *CANVAS_TARGET = "#canvas";

/* -------------------------------------------------------------------- */
/** \name DOM Event Callbacks (Browser Main Thread)
 *
 * These only copy the event, all GHOST state is handled on Blender's thread.
 * \{ */

static uint64_t web_event_time_ms()
{
  return uint64_t(emscripten_get_now());
}

static void web_event_mouse_fill(GHOST_WebEvent &event, const EmscriptenMouseEvent *mouse_event)
{
  const double ratio = emscripten_get_device_pixel_ratio();
  event.time_ms = web_event_time_ms();
  event.x = int32_t(std::lround(mouse_event->clientX * ratio));
  event.y = int32_t(std::lround(mouse_event->clientY * ratio));
  event.button = mouse_event->button;
  event.shift = mouse_event->shiftKey;
  event.ctrl = mouse_event->ctrlKey;
  event.alt = mouse_event->altKey;
  event.meta = mouse_event->metaKey;
}

static bool web_on_mouse(int event_type, const EmscriptenMouseEvent *mouse_event, void *user_data)
{
  GHOST_WebEvent event = {};
  switch (event_type) {
    case EMSCRIPTEN_EVENT_MOUSEMOVE:
      event.type = GHOST_WebEvent::Type::MouseMove;
      break;
    case EMSCRIPTEN_EVENT_MOUSEDOWN:
      event.type = GHOST_WebEvent::Type::MouseDown;
      break;
    case EMSCRIPTEN_EVENT_MOUSEUP:
      event.type = GHOST_WebEvent::Type::MouseUp;
      break;
    default:
      return false;
  }
  web_event_mouse_fill(event, mouse_event);
  static_cast<GHOST_SystemEmscripten *>(user_data)->queueWebEvent(event);
  /* Prevent text selection and other default actions on the canvas. */
  return event_type != EMSCRIPTEN_EVENT_MOUSEMOVE;
}

static bool web_on_wheel(int /*event_type*/, const EmscriptenWheelEvent *wheel_event, void *user_data)
{
  /* Pixel based deltas (track-pads, smooth scrolling) are accumulated into steps. */
  constexpr double pixels_per_step = 50.0;
  static double accum_x = 0.0, accum_y = 0.0;

  GHOST_WebEvent event = {};
  event.type = GHOST_WebEvent::Type::Wheel;
  web_event_mouse_fill(event, &wheel_event->mouse);

  auto to_steps = [&](const double delta, double &accum) -> int {
    if (wheel_event->deltaMode != DOM_DELTA_PIXEL) {
      return (delta > 0.0) ? 1 : ((delta < 0.0) ? -1 : 0);
    }
    accum += delta;
    const int steps = int(accum / pixels_per_step);
    accum -= steps * pixels_per_step;
    return steps;
  };
  /* DOM deltas are positive when scrolling down/right, GHOST uses the opposite for vertical. */
  event.wheel_x = to_steps(wheel_event->deltaX, accum_x);
  event.wheel_y = -to_steps(wheel_event->deltaY, accum_y);
  if (event.wheel_x || event.wheel_y) {
    static_cast<GHOST_SystemEmscripten *>(user_data)->queueWebEvent(event);
  }
  /* Prevent scrolling (and zooming with Control) the page. */
  return true;
}

template<size_t N> static void copy_string(char (&dst)[N], const char *src)
{
  strncpy(dst, src, N - 1);
  dst[N - 1] = '\0';
}

static bool web_on_key(int event_type, const EmscriptenKeyboardEvent *key_event, void *user_data)
{
  GHOST_WebEvent event = {};
  event.type = (event_type == EMSCRIPTEN_EVENT_KEYDOWN) ? GHOST_WebEvent::Type::KeyDown :
                                                           GHOST_WebEvent::Type::KeyUp;
  event.time_ms = web_event_time_ms();
  copy_string(event.code, key_event->code);
  copy_string(event.key, key_event->key);
  event.repeat = key_event->repeat;
  event.shift = key_event->shiftKey;
  event.ctrl = key_event->ctrlKey;
  event.alt = key_event->altKey;
  event.meta = key_event->metaKey;
  static_cast<GHOST_SystemEmscripten *>(user_data)->queueWebEvent(event);

  /* Let the browser handle reloading the page and its developer tools,
   * everything else is for Blender (e.g. Control-S must not save the page). */
  if (strcmp(key_event->code, "F5") == 0 ||
      (key_event->ctrlKey && key_event->shiftKey && strcmp(key_event->code, "KeyI") == 0))
  {
    return false;
  }
  return true;
}

static bool web_on_resize(int /*event_type*/, const EmscriptenUiEvent * /*ui_event*/, void *user_data)
{
  GHOST_SystemEmscripten *system = static_cast<GHOST_SystemEmscripten *>(user_data);
  system->updateCanvasSizeOnMainThread();
  GHOST_WebEvent event = {};
  event.type = GHOST_WebEvent::Type::Resize;
  event.time_ms = web_event_time_ms();
  system->queueWebEvent(event);
  return false;
}

static bool web_on_focus(int event_type, const EmscriptenFocusEvent * /*focus_event*/, void *user_data)
{
  GHOST_WebEvent event = {};
  event.type = (event_type == EMSCRIPTEN_EVENT_FOCUS) ? GHOST_WebEvent::Type::Focus :
                                                         GHOST_WebEvent::Type::Blur;
  event.time_ms = web_event_time_ms();
  static_cast<GHOST_SystemEmscripten *>(user_data)->queueWebEvent(event);
  return false;
}

static void web_register_callbacks(void *user_data)
{
  const pthread_t thread = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
  const char *window = EMSCRIPTEN_EVENT_TARGET_WINDOW;

  /* Presses start on the canvas, motion and release are tracked on the window,
   * so dragging outside of the canvas keeps working. */
  emscripten_set_mousedown_callback_on_thread(CANVAS_TARGET, user_data, true, web_on_mouse, thread);
  emscripten_set_mousemove_callback_on_thread(window, user_data, true, web_on_mouse, thread);
  emscripten_set_mouseup_callback_on_thread(window, user_data, true, web_on_mouse, thread);
  emscripten_set_wheel_callback_on_thread(CANVAS_TARGET, user_data, true, web_on_wheel, thread);
  emscripten_set_keydown_callback_on_thread(window, user_data, true, web_on_key, thread);
  emscripten_set_keyup_callback_on_thread(window, user_data, true, web_on_key, thread);
  emscripten_set_resize_callback_on_thread(window, user_data, true, web_on_resize, thread);
  emscripten_set_focus_callback_on_thread(window, user_data, true, web_on_focus, thread);
  emscripten_set_blur_callback_on_thread(window, user_data, true, web_on_focus, thread);

  /* The right mouse button is used by Blender, not for the browser's context menu. */
  EM_ASM({
    const canvas = document.querySelector(UTF8ToString($0));
    canvas.addEventListener('contextmenu', event => event.preventDefault());
  }, CANVAS_TARGET);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Key Conversion
 * \{ */

static GHOST_TKey web_convert_key(const char *code)
{
  /* `KeyboardEvent.code` describes the physical key, independent of the keyboard layout. */
  if (strncmp(code, "Key", 3) == 0 && code[3] >= 'A' && code[3] <= 'Z' && code[4] == '\0') {
    return GHOST_TKey(GHOST_kKeyA + (code[3] - 'A'));
  }
  if (strncmp(code, "Digit", 5) == 0 && code[5] >= '0' && code[5] <= '9' && code[6] == '\0') {
    return GHOST_TKey(GHOST_kKey0 + (code[5] - '0'));
  }
  if (strncmp(code, "Numpad", 6) == 0 && code[6] >= '0' && code[6] <= '9' && code[7] == '\0') {
    return GHOST_TKey(GHOST_kKeyNumpad0 + (code[6] - '0'));
  }
  if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
    const int number = atoi(code + 1);
    if (number >= 1 && number <= 24) {
      return GHOST_TKey(GHOST_kKeyF1 + (number - 1));
    }
  }

  static const struct {
    const char *code;
    GHOST_TKey key;
  } table[] = {
      {"Backspace", GHOST_kKeyBackSpace},
      {"Tab", GHOST_kKeyTab},
      {"Enter", GHOST_kKeyEnter},
      {"Escape", GHOST_kKeyEsc},
      {"Space", GHOST_kKeySpace},
      {"Quote", GHOST_kKeyQuote},
      {"Comma", GHOST_kKeyComma},
      {"Minus", GHOST_kKeyMinus},
      {"Period", GHOST_kKeyPeriod},
      {"Slash", GHOST_kKeySlash},
      {"Semicolon", GHOST_kKeySemicolon},
      {"Equal", GHOST_kKeyEqual},
      {"BracketLeft", GHOST_kKeyLeftBracket},
      {"BracketRight", GHOST_kKeyRightBracket},
      {"Backslash", GHOST_kKeyBackslash},
      {"Backquote", GHOST_kKeyAccentGrave},
      {"IntlBackslash", GHOST_kKeyGrLess},
      {"ShiftLeft", GHOST_kKeyLeftShift},
      {"ShiftRight", GHOST_kKeyRightShift},
      {"ControlLeft", GHOST_kKeyLeftControl},
      {"ControlRight", GHOST_kKeyRightControl},
      {"AltLeft", GHOST_kKeyLeftAlt},
      {"AltRight", GHOST_kKeyRightAlt},
      {"MetaLeft", GHOST_kKeyLeftOS},
      {"MetaRight", GHOST_kKeyRightOS},
      {"ContextMenu", GHOST_kKeyApp},
      {"CapsLock", GHOST_kKeyCapsLock},
      {"NumLock", GHOST_kKeyNumLock},
      {"ScrollLock", GHOST_kKeyScrollLock},
      {"ArrowLeft", GHOST_kKeyLeftArrow},
      {"ArrowRight", GHOST_kKeyRightArrow},
      {"ArrowUp", GHOST_kKeyUpArrow},
      {"ArrowDown", GHOST_kKeyDownArrow},
      {"PrintScreen", GHOST_kKeyPrintScreen},
      {"Pause", GHOST_kKeyPause},
      {"Insert", GHOST_kKeyInsert},
      {"Delete", GHOST_kKeyDelete},
      {"Home", GHOST_kKeyHome},
      {"End", GHOST_kKeyEnd},
      {"PageUp", GHOST_kKeyUpPage},
      {"PageDown", GHOST_kKeyDownPage},
      {"NumpadDecimal", GHOST_kKeyNumpadPeriod},
      {"NumpadEnter", GHOST_kKeyNumpadEnter},
      {"NumpadAdd", GHOST_kKeyNumpadPlus},
      {"NumpadSubtract", GHOST_kKeyNumpadMinus},
      {"NumpadMultiply", GHOST_kKeyNumpadAsterisk},
      {"NumpadDivide", GHOST_kKeyNumpadSlash},
      {"MediaPlayPause", GHOST_kKeyMediaPlay},
      {"MediaStop", GHOST_kKeyMediaStop},
      {"MediaTrackPrevious", GHOST_kKeyMediaFirst},
      {"MediaTrackNext", GHOST_kKeyMediaLast},
  };
  for (const auto &item : table) {
    if (strcmp(code, item.code) == 0) {
      return item.key;
    }
  }
  return GHOST_kKeyUnknown;
}

/**
 * `KeyboardEvent.key` is the text a key produces, or a name for keys without text
 * (e.g. "Enter", "Shift"). Text is a single code-point, which is what GHOST expects.
 */
static void web_key_text(const char *key, char r_utf8_buf[6])
{
  r_utf8_buf[0] = '\0';
  const size_t len = strlen(key);
  if (len == 0 || len > 4) {
    return;
  }
  const unsigned char c = key[0];
  const size_t codepoint_len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
  if (codepoint_len != len) {
    return;
  }
  memcpy(r_utf8_buf, key, len);
  r_utf8_buf[len] = '\0';
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name System
 * \{ */

GHOST_SystemEmscripten::GHOST_SystemEmscripten() : GHOST_System() {}

GHOST_SystemEmscripten::~GHOST_SystemEmscripten() = default;

static void web_update_canvas_size(void *user_data)
{
  static_cast<GHOST_SystemEmscripten *>(user_data)->updateCanvasSizeOnMainThread();
}

GHOST_TSuccess GHOST_SystemEmscripten::init()
{
  if (GHOST_System::init() == GHOST_kFailure) {
    return GHOST_kFailure;
  }
  /* DOM access is only possible on the browser's main thread. */
  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, web_update_canvas_size, this);
  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, web_register_callbacks, this);
  return GHOST_kSuccess;
}

void GHOST_SystemEmscripten::updateCanvasSizeOnMainThread()
{
  double css_width = 0.0, css_height = 0.0;
  emscripten_get_element_css_size(CANVAS_TARGET, &css_width, &css_height);
  const double ratio = emscripten_get_device_pixel_ratio();
  const uint32_t width = std::max(1u, uint32_t(std::lround(css_width * ratio)));
  const uint32_t height = std::max(1u, uint32_t(std::lround(css_height * ratio)));
  /* The drawing buffer matches the displayed size in device pixels, for sharp HiDPI drawing. */
  emscripten_set_canvas_element_size(CANVAS_TARGET, int(width), int(height));

  std::lock_guard lock(web_events_mutex_);
  canvas_width_ = width;
  canvas_height_ = height;
  device_pixel_ratio_ = float(ratio);
}

void GHOST_SystemEmscripten::getCanvasSize(uint32_t &width, uint32_t &height) const
{
  std::lock_guard lock(const_cast<std::mutex &>(web_events_mutex_));
  width = canvas_width_;
  height = canvas_height_;
}

void GHOST_SystemEmscripten::queueWebEvent(const GHOST_WebEvent &event)
{
  {
    std::lock_guard lock(web_events_mutex_);
    web_events_.push_back(event);
  }
  web_events_cond_.notify_one();
}

bool GHOST_SystemEmscripten::processEvents(bool waitForEvent)
{
  bool any_processed = false;
  do {
    GHOST_TimerManager *timer_manager = getTimerManager();
    std::deque<GHOST_WebEvent> events;
    {
      std::unique_lock lock(web_events_mutex_);
      if (waitForEvent && web_events_.empty()) {
        const uint64_t next = timer_manager->nextFireTime();
        if (next == GHOST_kFireTimeNever) {
          web_events_cond_.wait(lock, [&] { return !web_events_.empty(); });
        }
        else {
          const int64_t wait_ms = int64_t(next) - int64_t(getMilliSeconds());
          if (wait_ms > 0) {
            web_events_cond_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
              return !web_events_.empty();
            });
          }
        }
      }
      events.swap(web_events_);
    }

    if (timer_manager->fireTimers(getMilliSeconds())) {
      any_processed = true;
    }
    for (const GHOST_WebEvent &event : events) {
      processWebEvent(event);
      any_processed = true;
    }
  } while (waitForEvent && !any_processed);

  return any_processed;
}

void GHOST_SystemEmscripten::processWebEvent(const GHOST_WebEvent &event)
{
  GHOST_WindowEmscripten *window = window_;
  if (window == nullptr) {
    return;
  }

  switch (event.type) {
    case GHOST_WebEvent::Type::MouseMove: {
      cursor_x_ = event.x;
      cursor_y_ = event.y;
      pushEvent(std::make_unique<GHOST_EventCursor>(
          event.time_ms, GHOST_kEventCursorMove, window, event.x, event.y, GHOST_TABLET_DATA_NONE));
      break;
    }
    case GHOST_WebEvent::Type::MouseDown:
    case GHOST_WebEvent::Type::MouseUp: {
      GHOST_TButton button;
      switch (event.button) {
        case 0:
          button = GHOST_kButtonMaskLeft;
          break;
        case 1:
          button = GHOST_kButtonMaskMiddle;
          break;
        case 2:
          button = GHOST_kButtonMaskRight;
          break;
        case 3:
          button = GHOST_kButtonMaskButton4;
          break;
        case 4:
          button = GHOST_kButtonMaskButton5;
          break;
        default:
          return;
      }
      const bool down = event.type == GHOST_WebEvent::Type::MouseDown;
      buttons_.set(button, down);
      /* The cursor may have moved without motion events (e.g. focusing the page). */
      if (event.x != cursor_x_ || event.y != cursor_y_) {
        cursor_x_ = event.x;
        cursor_y_ = event.y;
        pushEvent(std::make_unique<GHOST_EventCursor>(event.time_ms,
                                                      GHOST_kEventCursorMove,
                                                      window,
                                                      event.x,
                                                      event.y,
                                                      GHOST_TABLET_DATA_NONE));
      }
      pushEvent(std::make_unique<GHOST_EventButton>(event.time_ms,
                                                    down ? GHOST_kEventButtonDown :
                                                           GHOST_kEventButtonUp,
                                                    window,
                                                    button,
                                                    GHOST_TABLET_DATA_NONE));
      break;
    }
    case GHOST_WebEvent::Type::Wheel: {
      if (event.wheel_y) {
        pushEvent(std::make_unique<GHOST_EventWheel>(
            event.time_ms, window, GHOST_kEventWheelAxisVertical, event.wheel_y));
      }
      if (event.wheel_x) {
        pushEvent(std::make_unique<GHOST_EventWheel>(
            event.time_ms, window, GHOST_kEventWheelAxisHorizontal, event.wheel_x));
      }
      break;
    }
    case GHOST_WebEvent::Type::KeyDown:
    case GHOST_WebEvent::Type::KeyUp: {
      const GHOST_TKey key = web_convert_key(event.code);
      const bool down = event.type == GHOST_WebEvent::Type::KeyDown;
      switch (key) {
        case GHOST_kKeyLeftShift:
          modifier_keys_.set(GHOST_kModifierKeyLeftShift, down);
          break;
        case GHOST_kKeyRightShift:
          modifier_keys_.set(GHOST_kModifierKeyRightShift, down);
          break;
        case GHOST_kKeyLeftControl:
          modifier_keys_.set(GHOST_kModifierKeyLeftControl, down);
          break;
        case GHOST_kKeyRightControl:
          modifier_keys_.set(GHOST_kModifierKeyRightControl, down);
          break;
        case GHOST_kKeyLeftAlt:
          modifier_keys_.set(GHOST_kModifierKeyLeftAlt, down);
          break;
        case GHOST_kKeyRightAlt:
          modifier_keys_.set(GHOST_kModifierKeyRightAlt, down);
          break;
        case GHOST_kKeyLeftOS:
          modifier_keys_.set(GHOST_kModifierKeyLeftOS, down);
          break;
        case GHOST_kKeyRightOS:
          modifier_keys_.set(GHOST_kModifierKeyRightOS, down);
          break;
        default:
          break;
      }
      char utf8_buf[sizeof(GHOST_TEventKeyData::utf8_buf)];
      utf8_buf[0] = '\0';
      if (down) {
        web_key_text(event.key, utf8_buf);
      }
      pushEvent(std::make_unique<GHOST_EventKey>(event.time_ms,
                                                 down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp,
                                                 window,
                                                 key,
                                                 event.repeat,
                                                 utf8_buf));
      break;
    }
    case GHOST_WebEvent::Type::Resize: {
      pushEvent(std::make_unique<GHOST_Event>(event.time_ms, GHOST_kEventWindowSize, window));
      break;
    }
    case GHOST_WebEvent::Type::Focus: {
      window_manager_->setActiveWindow(window);
      pushEvent(std::make_unique<GHOST_Event>(event.time_ms, GHOST_kEventWindowActivate, window));
      break;
    }
    case GHOST_WebEvent::Type::Blur: {
      /* Releasing keys outside of the page is not reported, forget all held keys. */
      modifier_keys_.clear();
      buttons_.clear();
      window_manager_->setWindowInactive(window);
      pushEvent(
          std::make_unique<GHOST_Event>(event.time_ms, GHOST_kEventWindowDeactivate, window));
      break;
    }
  }
}

GHOST_TSuccess GHOST_SystemEmscripten::getModifierKeys(GHOST_ModifierKeys &keys) const
{
  keys = modifier_keys_;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemEmscripten::getButtons(GHOST_Buttons &buttons) const
{
  buttons = buttons_;
  return GHOST_kSuccess;
}

GHOST_TCapabilityFlag GHOST_SystemEmscripten::getCapabilities() const
{
  return GHOST_TCapabilityFlag(
      GHOST_CAPABILITY_FLAG_ALL &
      /* NOTE: order the following flags as they they're declared in the source. */
      ~(
          /* A page can't move itself. */
          GHOST_kCapabilityWindowPosition |
          /* Browsers don't allow moving the cursor (pointer lock is used for grabbing). */
          GHOST_kCapabilityCursorWarp |
          /* Only the X11 style primary selection, which browsers don't have. */
          GHOST_kCapabilityClipboardPrimary |
          /* Not yet supported. */
          GHOST_kCapabilityClipboardImage |
          /* Reading screen pixels outside of the page is not allowed. */
          GHOST_kCapabilityDesktopSample |
          /* Not yet supported. */
          GHOST_kCapabilityInputIME |
          /* Not applicable. */
          GHOST_kCapabilityWindowDecorationStyles |
          /* Not applicable. */
          GHOST_kCapabilityKeyboardHyperKey |
          /* Not yet supported. */
          GHOST_kCapabilityCursorRGBA |
          /* Not yet supported. */
          GHOST_kCapabilityCursorGenerator |
          /* There is a single page. */
          GHOST_kCapabilityMultiMonitorPlacement |
          /* Not applicable. */
          GHOST_kCapabilityWindowPath));
}

char *GHOST_SystemEmscripten::getClipboard(bool selection) const
{
  if (selection || clipboard_.empty()) {
    return nullptr;
  }
  char *result = static_cast<char *>(malloc(clipboard_.size() + 1));
  memcpy(result, clipboard_.c_str(), clipboard_.size() + 1);
  return result;
}

void GHOST_SystemEmscripten::putClipboard(const char *buffer, bool selection) const
{
  if (!selection) {
    clipboard_ = buffer;
  }
}

uint64_t GHOST_SystemEmscripten::getMilliSeconds() const
{
  return uint64_t(emscripten_get_now());
}

void GHOST_SystemEmscripten::getMainDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  getCanvasSize(width, height);
}

void GHOST_SystemEmscripten::getAllDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  getCanvasSize(width, height);
}

GHOST_TSuccess GHOST_SystemEmscripten::getCursorPosition(int32_t &x, int32_t &y) const
{
  x = cursor_x_;
  y = cursor_y_;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemEmscripten::setCursorPosition(int32_t /*x*/, int32_t /*y*/)
{
  return GHOST_kFailure;
}

GHOST_IContext *GHOST_SystemEmscripten::createOffscreenContext(GHOST_GPUSettings gpu_settings)
{
  /* GPU contexts are provided by the WebGPU backend, not yet implemented. */
  const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS_OFFSCREEN(
      gpu_settings);
  GHOST_Context *context = new GHOST_ContextNone(context_params);
  if (context->initializeDrawingContext()) {
    return context;
  }
  delete context;
  return nullptr;
}

GHOST_TSuccess GHOST_SystemEmscripten::disposeContext(GHOST_IContext *context)
{
  delete context;
  return GHOST_kSuccess;
}

GHOST_IWindow *GHOST_SystemEmscripten::createWindow(const char *title,
                                                    int32_t /*left*/,
                                                    int32_t /*top*/,
                                                    uint32_t /*width*/,
                                                    uint32_t /*height*/,
                                                    GHOST_TWindowState /*state*/,
                                                    GHOST_GPUSettings gpu_settings,
                                                    const bool /*exclusive*/,
                                                    const bool /*is_dialog*/,
                                                    const GHOST_IWindow * /*parent_window*/)
{
  /* The page has one canvas: additional windows (e.g. preferences, render view) fail,
   * Blender then opens them inside the main window where it can. */
  if (window_) {
    return nullptr;
  }
  const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS(gpu_settings);
  GHOST_WindowEmscripten *window = new GHOST_WindowEmscripten(
      this, title, gpu_settings.context_type, context_params);
  if (!window->getValid()) {
    delete window;
    return nullptr;
  }
  window_ = window;
  window_manager_->addWindow(window);
  window_manager_->setActiveWindow(window);
  pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowSize, window));
  return window;
}

GHOST_TSuccess GHOST_SystemEmscripten::disposeWindow(GHOST_IWindow *window)
{
  if (window == window_) {
    window_ = nullptr;
  }
  return GHOST_System::disposeWindow(window);
}

GHOST_IWindow *GHOST_SystemEmscripten::getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/)
{
  return window_;
}

/** \} */
