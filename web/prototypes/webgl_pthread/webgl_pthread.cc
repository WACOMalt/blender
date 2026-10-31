/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Prototype of the presentation model planned for Blender's WebGL2 backend (see `build.sh`):
 * WebGL2 runs on Blender's pthread, which never returns to its event loop. The context draws
 * into an OffscreenCanvas created on that thread; each frame is handed to the page as an
 * ImageBitmap (`transferToImageBitmap`, posted without copying). */

#include <GLES3/gl3.h>
#include <cstdio>
#include <unistd.h>

#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>

/* Creates a WebGL2 context on an OffscreenCanvas owned by the calling thread and registers it
 * with Emscripten's GL layer, returns its handle (0 on failure). */
EM_JS(int, web_gl_create_context, (int width, int height), {
  var canvas = new OffscreenCanvas(width, height);
  var attributes = {majorVersion : 2, minorVersion : 0, alpha : false, depth : true,
                    stencil : true, antialias : false, premultipliedAlpha : true,
                    preserveDrawingBuffer : false, enableExtensionsByDefault : true};
  var gl = canvas.getContext('webgl2', attributes);
  if (!gl) {
    return 0;
  }
  return GL.registerContext(gl, attributes);
});

/* Hands the frame drawn so far to the page (`Module.webglPresent` on the main thread). */
EM_JS(void, web_gl_present, (), {
  var bitmap = GLctx.canvas.transferToImageBitmap();
  /* `cmd: 9` is Emscripten's CMD_CALL_HANDLER: the main thread calls Module[handler](...args). */
  postMessage({cmd : 9, handler : 'webglPresent', args : [bitmap]}, [bitmap]);
});

static const char *vertex_src = R"(#version 300 es
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";
static const char *fragment_src = R"(#version 300 es
precision mediump float;
uniform vec4 color;
out vec4 frag;
void main() { frag = color; }
)";

static GLuint compile(GLenum type, const char *src)
{
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    printf("shader error: %s\n", log);
  }
  return shader;
}

int main()
{
  const int handle = web_gl_create_context(320, 240);
  if (handle == 0 ||
      emscripten_webgl_make_context_current(handle) != EMSCRIPTEN_RESULT_SUCCESS) {
    printf("RESULT: no WebGL2 context\n");
    return 1;
  }
  printf("GL_VERSION: %s\n", (const char *)glGetString(GL_VERSION));

  GLuint program = glCreateProgram();
  glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_src));
  glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_src));
  glBindAttribLocation(program, 0, "pos");
  glLinkProgram(program);
  glUseProgram(program);

  const float triangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  GLuint vbo, vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  const GLint color = glGetUniformLocation(program, "color");

  /* A blocking loop like Blender's main loop: this thread never returns to its event loop. */
  for (int frame = 0; frame < 30; frame++) {
    const float t = frame / 29.0f;
    glViewport(0, 0, 320, 240);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(color, 1.0f, t * 0.5f, 0.25f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    web_gl_present();
    usleep(16000);
  }
  printf("frames done\n");
  /* Keep the thread blocked, as Blender's would be. */
  for (;;) {
    usleep(1000000);
  }
}
