/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Desktop OpenGL functions the OpenGL backend references that WebGL2 doesn't have, see
 * `web/epoxy/gl.h`. Some map onto OpenGL ES 3.0, the others report an error: they are behind
 * capability flags that are disabled in GLES mode and must not be reached.
 */

#include <epoxy/gl.h>

#include <cstdio>
#include <set>
#include <string>

/** Reports a call to a function WebGL2 doesn't provide, once per function. */
static void web_gl_unsupported(const char *name)
{
  static std::set<std::string> reported;
  if (reported.insert(name).second) {
    fprintf(stderr, "WebGL: %s is not available (called in GLES mode)\n", name);
  }
}

extern "C" {

/* -------------------------------------------------------------------- */
/** \name Functions mapped onto OpenGL ES 3.0
 * \{ */

void glClearDepth(GLdouble depth)
{
  glClearDepthf(GLfloat(depth));
}

/** Selects one draw buffer: the window's back buffer, or one color attachment (OpenGL ES needs
 * attachment `i` at position `i` of the list). */
void glDrawBuffer(GLenum buf)
{
  if (buf >= GL_COLOR_ATTACHMENT0 && buf <= GL_COLOR_ATTACHMENT15) {
    GLenum buffers[16];
    const int index = int(buf - GL_COLOR_ATTACHMENT0);
    for (int i = 0; i < index; i++) {
      buffers[i] = GL_NONE;
    }
    buffers[index] = buf;
    glDrawBuffers(index + 1, buffers);
    return;
  }
  const GLenum buffer = (buf == GL_NONE) ? GL_NONE : GL_BACK;
  glDrawBuffers(1, &buffer);
}

/** OpenGL ES has no per-attachment color masks (without OES_draw_buffers_indexed): the mask
 * applies to all attachments. */
void glColorMaski(GLuint /*index*/, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
  glColorMask(r, g, b, a);
}

/** Point sizes come from `gl_PointSize` in OpenGL ES. */
void glPointSize(GLfloat /*size*/) {}

void *glMapBuffer(GLenum target, GLenum access)
{
  GLint size = 0;
  glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  const GLbitfield bits = (access == GL_READ_ONLY)  ? GL_MAP_READ_BIT :
                          (access == GL_WRITE_ONLY) ? GL_MAP_WRITE_BIT :
                                                      (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
  return glMapBufferRange(target, 0, size, bits);
}

/* 1D textures are 2D textures one texel high (the backend binds them as GL_TEXTURE_2D). */

void glTexImage1D(GLenum /*target*/,
                  GLint level,
                  GLint internalformat,
                  GLsizei width,
                  GLint border,
                  GLenum format,
                  GLenum type,
                  const void *pixels)
{
  glTexImage2D(GL_TEXTURE_2D, level, internalformat, width, 1, border, format, type, pixels);
}

void glTexSubImage1D(GLenum /*target*/,
                     GLint level,
                     GLint xoffset,
                     GLsizei width,
                     GLenum format,
                     GLenum type,
                     const void *pixels)
{
  glTexSubImage2D(GL_TEXTURE_2D, level, xoffset, 0, width, 1, format, type, pixels);
}

void glTexStorage1D(GLenum /*target*/, GLsizei levels, GLenum internalformat, GLsizei width)
{
  glTexStorage2D(GL_TEXTURE_2D, levels, internalformat, width, 1);
}

void glCompressedTexImage1D(GLenum /*target*/,
                            GLint level,
                            GLenum internalformat,
                            GLsizei width,
                            GLint border,
                            GLsizei image_size,
                            const void *data)
{
  glCompressedTexImage2D(GL_TEXTURE_2D, level, internalformat, width, 1, border, image_size, data);
}

void glCompressedTexSubImage1D(GLenum /*target*/,
                               GLint level,
                               GLint xoffset,
                               GLsizei width,
                               GLenum format,
                               GLsizei image_size,
                               const void *data)
{
  glCompressedTexSubImage2D(
      GL_TEXTURE_2D, level, xoffset, 0, width, 1, format, image_size, data);
}

void glGetActiveUniformName(
    GLuint program, GLuint uniform_index, GLsizei buf_size, GLsizei *length, GLchar *name)
{
  GLint size;
  GLenum type;
  glGetActiveUniform(program, uniform_index, buf_size, length, &size, &type, name);
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params)
{
  GLuint value = 0;
  glGetQueryObjectuiv(id, pname, &value);
  *params = GLint(value);
}

void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params)
{
  GLuint value = 0;
  glGetQueryObjectuiv(id, pname, &value);
  *params = value;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions without effect in WebGL
 * \{ */

/* WebGL uses the last vertex of a primitive as provoking vertex, like OpenGL's default. */
void glProvokingVertex(GLenum /*mode*/) {}
/* Synchronization is implicit in WebGL (no image load/store or framebuffer fetch). */
void glFramebufferFetchBarrierEXT() {}
void glTextureBarrier() {}
void glMemoryBarrier(GLbitfield /*barriers*/) {}
/* No debug output in WebGL. */
void glObjectLabel(GLenum /*identifier*/, GLuint /*name*/, GLsizei /*length*/, const GLchar * /*label*/) {}
void glPushDebugGroup(GLenum /*source*/, GLuint /*id*/, GLsizei /*length*/, const GLchar * /*message*/) {}
void glPopDebugGroup() {}
void glDebugMessageInsert(GLenum /*source*/, GLenum /*type*/, GLuint /*id*/, GLenum /*severity*/, GLsizei /*length*/, const GLchar * /*buf*/) {}
void glDebugMessageControl(GLenum /*source*/, GLenum /*type*/, GLenum /*severity*/, GLsizei /*count*/, const GLuint * /*ids*/, GLboolean /*enabled*/) {}
void glDebugMessageCallback(GLDEBUGPROC /*callback*/, const void * /*user_param*/) {}
/* WebGL's clip space is OpenGL's (lower-left origin, depth -1..1): shaders are generated for
 * it in GLES mode. */
void glClipControl(GLenum /*origin*/, GLenum /*depth*/) {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions not available in WebGL
 * \{ */

void glGetUnsignedBytei_vEXT(GLenum /*target*/, GLuint /*index*/, GLubyte * /*data*/)
{
  web_gl_unsupported("glGetUnsignedBytei_vEXT");
}

void glGetTexLevelParameteriv(GLenum /*target*/, GLint /*level*/, GLenum /*pname*/, GLint *params)
{
  web_gl_unsupported("glGetTexLevelParameteriv");
  *params = 0;
}

void glBindImageTexture(GLuint /*unit*/, GLuint /*texture*/, GLint /*level*/, GLboolean /*layered*/, GLint /*layer*/, GLenum /*access*/, GLenum /*format*/)
{
  web_gl_unsupported("glBindImageTexture");
}

void glBindImageTextures(GLuint /*first*/, GLsizei /*count*/, const GLuint * /*textures*/)
{
  web_gl_unsupported("glBindImageTextures");
}

void glBindSamplers(GLuint /*first*/, GLsizei /*count*/, const GLuint * /*samplers*/)
{
  web_gl_unsupported("glBindSamplers");
}

void glBindTextures(GLuint /*first*/, GLsizei /*count*/, const GLuint * /*textures*/)
{
  web_gl_unsupported("glBindTextures");
}

void glBindVertexBuffer(GLuint /*bindingindex*/, GLuint /*buffer*/, GLintptr /*offset*/, GLsizei /*stride*/)
{
  web_gl_unsupported("glBindVertexBuffer");
}

void glBufferStorage(GLenum /*target*/, GLsizeiptr /*size*/, const void * /*data*/, GLbitfield /*flags*/)
{
  web_gl_unsupported("glBufferStorage");
}

void glClearBufferData(GLenum /*target*/, GLenum /*internalformat*/, GLenum /*format*/, GLenum /*type*/, const void * /*data*/)
{
  web_gl_unsupported("glClearBufferData");
}

void glClearNamedBufferData(GLuint /*buffer*/, GLenum /*internalformat*/, GLenum /*format*/, GLenum /*type*/, const void * /*data*/)
{
  web_gl_unsupported("glClearNamedBufferData");
}

void glClearTexImage(GLuint /*texture*/, GLint /*level*/, GLenum /*format*/, GLenum /*type*/, const void * /*data*/)
{
  web_gl_unsupported("glClearTexImage");
}

void glCompressedTextureSubImage1D(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLsizei /*width*/, GLenum /*format*/, GLsizei /*imageSize*/, const void * /*data*/)
{
  web_gl_unsupported("glCompressedTextureSubImage1D");
}

void glCompressedTextureSubImage2D(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLint /*yoffset*/, GLsizei /*width*/, GLsizei /*height*/, GLenum /*format*/, GLsizei /*imageSize*/, const void * /*data*/)
{
  web_gl_unsupported("glCompressedTextureSubImage2D");
}

void glCompressedTextureSubImage3D(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLint /*yoffset*/, GLint /*zoffset*/, GLsizei /*width*/, GLsizei /*height*/, GLsizei /*depth*/, GLenum /*format*/, GLsizei /*imageSize*/, const void * /*data*/)
{
  web_gl_unsupported("glCompressedTextureSubImage3D");
}

void glCopyImageSubData(GLuint /*srcName*/, GLenum /*srcTarget*/, GLint /*srcLevel*/, GLint /*srcX*/, GLint /*srcY*/, GLint /*srcZ*/, GLuint /*dstName*/, GLenum /*dstTarget*/, GLint /*dstLevel*/, GLint /*dstX*/, GLint /*dstY*/, GLint /*dstZ*/, GLsizei /*srcWidth*/, GLsizei /*srcHeight*/, GLsizei /*srcDepth*/)
{
  web_gl_unsupported("glCopyImageSubData");
}

void glCopyNamedBufferSubData(GLuint /*readBuffer*/, GLuint /*writeBuffer*/, GLintptr /*readOffset*/, GLintptr /*writeOffset*/, GLsizeiptr /*size*/)
{
  web_gl_unsupported("glCopyNamedBufferSubData");
}

void glDispatchCompute(GLuint /*num_groups_x*/, GLuint /*num_groups_y*/, GLuint /*num_groups_z*/)
{
  web_gl_unsupported("glDispatchCompute");
}

void glDispatchComputeIndirect(GLintptr /*indirect*/)
{
  web_gl_unsupported("glDispatchComputeIndirect");
}

void glDrawArraysIndirect(GLenum /*mode*/, const void * /*indirect*/)
{
  web_gl_unsupported("glDrawArraysIndirect");
}

void glDrawElementsIndirect(GLenum /*mode*/, GLenum /*type*/, const void * /*indirect*/)
{
  web_gl_unsupported("glDrawElementsIndirect");
}

void glDrawElementsInstancedBaseVertexBaseInstance(GLenum /*mode*/, GLsizei /*count*/, GLenum /*type*/, const void * /*indices*/, GLsizei /*instancecount*/, GLint /*basevertex*/, GLuint /*baseinstance*/)
{
  web_gl_unsupported("glDrawElementsInstancedBaseVertexBaseInstance");
}

void glFramebufferParameteri(GLenum /*target*/, GLenum /*pname*/, GLint /*param*/)
{
  web_gl_unsupported("glFramebufferParameteri");
}

void glFramebufferTexture(GLenum /*target*/, GLenum /*attachment*/, GLuint /*texture*/, GLint /*level*/)
{
  web_gl_unsupported("glFramebufferTexture");
}

void glGenerateTextureMipmap(GLuint /*texture*/)
{
  web_gl_unsupported("glGenerateTextureMipmap");
}

void glGetNamedBufferSubData(GLuint /*buffer*/, GLintptr /*offset*/, GLsizeiptr /*size*/, void * /*data*/)
{
  web_gl_unsupported("glGetNamedBufferSubData");
}

void glGetProgramInterfaceiv(GLuint /*program*/, GLenum /*programInterface*/, GLenum /*pname*/, GLint * /*params*/)
{
  web_gl_unsupported("glGetProgramInterfaceiv");
}

void glGetProgramResourceiv(GLuint /*program*/, GLenum /*programInterface*/, GLuint /*index*/, GLsizei /*propCount*/, const GLenum * /*props*/, GLsizei /*count*/, GLsizei * /*length*/, GLint * /*params*/)
{
  web_gl_unsupported("glGetProgramResourceiv");
}

void glGetProgramResourceName(GLuint /*program*/, GLenum /*programInterface*/, GLuint /*index*/, GLsizei /*bufSize*/, GLsizei * /*length*/, GLchar * /*name*/)
{
  web_gl_unsupported("glGetProgramResourceName");
}

void glGetTextureSubImage(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLint /*yoffset*/, GLint /*zoffset*/, GLsizei /*width*/, GLsizei /*height*/, GLsizei /*depth*/, GLenum /*format*/, GLenum /*type*/, GLsizei /*bufSize*/, void * /*pixels*/)
{
  web_gl_unsupported("glGetTextureSubImage");
}

void glGetUnsignedBytevEXT(GLenum /*pname*/, GLubyte * /*data*/)
{
  web_gl_unsupported("glGetUnsignedBytevEXT");
}

void glMultiDrawArraysIndirect(GLenum /*mode*/, const void * /*indirect*/, GLsizei /*drawcount*/, GLsizei /*stride*/)
{
  web_gl_unsupported("glMultiDrawArraysIndirect");
}

void glMultiDrawElementsIndirect(GLenum /*mode*/, GLenum /*type*/, const void * /*indirect*/, GLsizei /*drawcount*/, GLsizei /*stride*/)
{
  web_gl_unsupported("glMultiDrawElementsIndirect");
}

void glQueryCounter(GLuint /*id*/, GLenum /*target*/)
{
  web_gl_unsupported("glQueryCounter");
}

void glTexBuffer(GLenum /*target*/, GLenum /*internalformat*/, GLuint /*buffer*/)
{
  web_gl_unsupported("glTexBuffer");
}

void glTextureBuffer(GLuint /*texture*/, GLenum /*internalformat*/, GLuint /*buffer*/)
{
  web_gl_unsupported("glTextureBuffer");
}

void glTextureParameteri(GLuint /*texture*/, GLenum /*pname*/, GLint /*param*/)
{
  web_gl_unsupported("glTextureParameteri");
}

void glTextureParameteriv(GLuint /*texture*/, GLenum /*pname*/, const GLint * /*param*/)
{
  web_gl_unsupported("glTextureParameteriv");
}

void glTextureSubImage1D(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLsizei /*width*/, GLenum /*format*/, GLenum /*type*/, const void * /*pixels*/)
{
  web_gl_unsupported("glTextureSubImage1D");
}

void glTextureSubImage2D(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLint /*yoffset*/, GLsizei /*width*/, GLsizei /*height*/, GLenum /*format*/, GLenum /*type*/, const void * /*pixels*/)
{
  web_gl_unsupported("glTextureSubImage2D");
}

void glTextureSubImage3D(GLuint /*texture*/, GLint /*level*/, GLint /*xoffset*/, GLint /*yoffset*/, GLint /*zoffset*/, GLsizei /*width*/, GLsizei /*height*/, GLsizei /*depth*/, GLenum /*format*/, GLenum /*type*/, const void * /*pixels*/)
{
  web_gl_unsupported("glTextureSubImage3D");
}

void glTextureView(GLuint /*texture*/, GLenum /*target*/, GLuint /*origtexture*/, GLenum /*internalformat*/, GLuint /*minlevel*/, GLuint /*numlevels*/, GLuint /*minlayer*/, GLuint /*numlayers*/)
{
  web_gl_unsupported("glTextureView");
}

GLboolean glUnmapNamedBuffer(GLuint /*buffer*/)
{
  web_gl_unsupported("glUnmapNamedBuffer");
  return GLboolean(0);
}

void glVertexAttribBinding(GLuint /*attribindex*/, GLuint /*bindingindex*/)
{
  web_gl_unsupported("glVertexAttribBinding");
}

void glVertexAttribFormat(GLuint /*attribindex*/, GLint /*size*/, GLenum /*type*/, GLboolean /*normalized*/, GLuint /*relativeoffset*/)
{
  web_gl_unsupported("glVertexAttribFormat");
}

void glViewportArrayv(GLuint /*first*/, GLsizei /*count*/, const GLfloat * /*v*/)
{
  web_gl_unsupported("glViewportArrayv");
}

/** \} */

}  /* extern "C" */
