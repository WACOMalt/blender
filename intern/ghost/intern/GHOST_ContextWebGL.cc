/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_ContextWebGL.hh"

#include <cstdio>
#include <pthread.h>

#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>

/* Creates a WebGL2 context on an OffscreenCanvas owned by the calling thread and registers it
 * with Emscripten's GL layer. Returns its handle, 0 on failure. */
EM_JS(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE, ghost_webgl_create, (int width, int height), {
  var canvas = new OffscreenCanvas(Math.max(width, 1), Math.max(height, 1));
  var attributes = {
    majorVersion : 2,
    minorVersion : 0,
    alpha : false,
    depth : true,
    stencil : true,
    antialias : false,
    premultipliedAlpha : true,
    preserveDrawingBuffer : false,
    powerPreference : 'high-performance',
    enableExtensionsByDefault : true,
  };
  var gl = canvas.getContext('webgl2', attributes);
  if (!gl) {
    return 0;
  }
  return GL.registerContext(gl, attributes);
});

/* Resizes the drawing buffer of the current context (clears it). */
EM_JS(void, ghost_webgl_resize, (int width, int height), {
  var canvas = GLctx.canvas;
  if (canvas.width != width || canvas.height != height) {
    canvas.width = width;
    canvas.height = height;
  }
});

/* Hands the frame drawn into the current context to the page. `cmd: 9` is Emscripten's
 * `CMD_CALL_HANDLER`: the main thread calls `Module.webglPresent(bitmap)`. The bitmap is
 * transferred, not copied. */
EM_JS(void, ghost_webgl_present, (), {
  var bitmap = GLctx.canvas.transferToImageBitmap();
  postMessage({cmd : 9, handler : 'webglPresent', args : [bitmap]}, [bitmap]);
});

/** The WebGL context shared by all GHOST contexts, and the thread owning it. */
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE shared_context = 0;
static pthread_t shared_context_thread;
static int shared_context_users = 0;

GHOST_ContextWebGL::GHOST_ContextWebGL(const GHOST_ContextParams &context_params,
                                       CanvasSizeFn canvas_size,
                                       const bool presents)
    : GHOST_Context(context_params), canvas_size_(std::move(canvas_size)), presents_(presents)
{
}

GHOST_ContextWebGL::~GHOST_ContextWebGL()
{
  releaseNativeHandles();
}

GHOST_TSuccess GHOST_ContextWebGL::initializeDrawingContext()
{
  if (shared_context == 0) {
    uint32_t width = 1, height = 1;
    canvas_size_(width, height);
    shared_context = ghost_webgl_create(int(width), int(height));
    if (shared_context == 0) {
      fprintf(stderr, "GHOST: WebGL2 is not available\n");
      return GHOST_kFailure;
    }
    shared_context_thread = pthread_self();
  }
  else if (!pthread_equal(shared_context_thread, pthread_self())) {
    /* A WebGL context can't be used from other threads, and contexts can't share resources. */
    fprintf(stderr, "GHOST: WebGL contexts can only be created on the main thread\n");
    return GHOST_kFailure;
  }
  shared_context_users++;
  uses_shared_context_ = true;
  return activateDrawingContext();
}

GHOST_TSuccess GHOST_ContextWebGL::releaseNativeHandles()
{
  if (!uses_shared_context_) {
    return GHOST_kSuccess;
  }
  uses_shared_context_ = false;
  if (--shared_context_users == 0) {
    emscripten_webgl_make_context_current(0);
    emscripten_webgl_destroy_context(shared_context);
    shared_context = 0;
  }
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextWebGL::activateDrawingContext()
{
  if (shared_context == 0 || !pthread_equal(shared_context_thread, pthread_self())) {
    return GHOST_kFailure;
  }
  if (emscripten_webgl_get_current_context() != shared_context &&
      emscripten_webgl_make_context_current(shared_context) != EMSCRIPTEN_RESULT_SUCCESS)
  {
    return GHOST_kFailure;
  }
  active_context_ = this;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextWebGL::releaseDrawingContext()
{
  /* The WebGL context stays current: the other contexts are views of the same one. */
  active_context_ = nullptr;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextWebGL::swapBufferAcquire()
{
  if (presents_ && activateDrawingContext() == GHOST_kSuccess) {
    /* The drawing buffer follows the page's canvas, resized between frames. */
    uint32_t width = 1, height = 1;
    canvas_size_(width, height);
    ghost_webgl_resize(int(width), int(height));
  }
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextWebGL::swapBufferRelease()
{
  if (!presents_ || activateDrawingContext() != GHOST_kSuccess) {
    return GHOST_kFailure;
  }
  ghost_webgl_present();
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextWebGL::setSwapInterval(const int interval)
{
  swap_interval_ = interval;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextWebGL::getSwapInterval(int &interval_out)
{
  interval_out = swap_interval_;
  return GHOST_kSuccess;
}
