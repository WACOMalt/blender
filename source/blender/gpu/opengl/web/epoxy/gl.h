/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * libepoxy's API for the WebAssembly build, where WebGL2 provides OpenGL ES 3.0 directly
 * (Emscripten's GL library). The OpenGL backend runs in its GLES mode there
 * (#GLContext::is_gles): features beyond OpenGL ES 3.0 are disabled by its capability flags.
 *
 * The desktop OpenGL enums and functions the backend names are declared so it compiles.
 * Functions WebGL2 doesn't have are implemented in `gl_web_compat.cc`: a few map onto
 * OpenGL ES 3.0 (1D textures become 2D textures one texel high), the others report an error,
 * they must not be reached in GLES mode.
 */

#pragma once

#ifndef GL_GLEXT_PROTOTYPES
#  define GL_GLEXT_PROTOTYPES 1
#endif

#include <GLES3/gl3.h>

#include <GLES2/gl2ext.h>

/* Desktop OpenGL types used by `glext.h`. */
typedef double GLdouble;
typedef double GLclampd;

/* Desktop OpenGL enums and function declarations. */
#include <GL/glext.h>

/* OpenGL 1.x enums, not in `glext.h`. */
#define GL_FRONT_LEFT 0x0400
#define GL_FRONT_RIGHT 0x0401
#define GL_BACK_LEFT 0x0402
#define GL_BACK_RIGHT 0x0403
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_LINE_SMOOTH 0x0B20
#define GL_POLYGON_SMOOTH 0x0B41
#define GL_STEREO 0x0C33
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_STENCIL_INDEX 0x1901
#define GL_RGB16 0x8054
#define GL_RGBA16 0x805B
#define GL_PROXY_TEXTURE_1D 0x8063
#define GL_PROXY_TEXTURE_2D 0x8064
#define GL_VERTEX_ARRAY 0x8074

#include <cstring>

/* OpenGL 1.x functions, not in `glext.h`. */
extern "C" {
void glClearDepth(GLdouble depth);
void glDrawBuffer(GLenum buf);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glPointSize(GLfloat size);
void glTexImage1D(GLenum target,
                  GLint level,
                  GLint internalformat,
                  GLsizei width,
                  GLint border,
                  GLenum format,
                  GLenum type,
                  const void *pixels);
void glTexSubImage1D(GLenum target,
                     GLint level,
                     GLint xoffset,
                     GLsizei width,
                     GLenum format,
                     GLenum type,
                     const void *pixels);
}

/* -------------------------------------------------------------------- */
/** \name libepoxy API
 * \{ */

/** The GL version as libepoxy reports it (major * 10 + minor): OpenGL ES 3.0. */
inline int epoxy_gl_version()
{
  return 30;
}

inline bool epoxy_is_desktop_gl()
{
  return false;
}

/** WebGL extensions, as reported by Emscripten with a "GL_" prefix. */
inline bool epoxy_has_gl_extension(const char *extension)
{
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; i++) {
    const char *name = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
    if (name && strcmp(name, extension) == 0) {
      return true;
    }
  }
  return false;
}

/** \} */
