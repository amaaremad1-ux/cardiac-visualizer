// gl_framebuffer.cpp - Framebuffer implementation.
#include "gl_framebuffer.hpp"

#include "cgl.hpp"

namespace cardiac::gl {

Framebuffer::~Framebuffer() { destroy(); }

void Framebuffer::destroy() {
  if (depth_rb_ != 0u) {
    glDeleteRenderbuffers(1, &depth_rb_);
    depth_rb_ = 0u;
  }
  if (color_tex_ != 0u) {
    glDeleteTextures(1, &color_tex_);
    color_tex_ = 0u;
  }
  if (fbo_ != 0u) {
    glDeleteFramebuffers(1, &fbo_);
    fbo_ = 0u;
  }
  complete_ = false;
}

bool Framebuffer::create(int w, int h, bool hdr, bool with_depth) {
  destroy();
  width_ = w > 0 ? w : 1;
  height_ = h > 0 ? h : 1;
  hdr_ = hdr;
  with_depth_ = with_depth;

  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

  glGenTextures(1, &color_tex_);
  glBindTexture(GL_TEXTURE_2D, color_tex_);
  GLenum internal = hdr ? GL_RGBA16F : GL_RGBA8;
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), width_, height_, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);

  if (with_depth_) {
    glGenRenderbuffers(1, &depth_rb_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb_);
  }

  complete_ = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0u);
  glBindTexture(GL_TEXTURE_2D, 0u);
  return complete_;
}

void Framebuffer::resize(int w, int h) {
  if (w == width_ && h == height_) return;
  create(w, h, hdr_, with_depth_);
}

void Framebuffer::bind() const { glBindFramebuffer(GL_FRAMEBUFFER, fbo_); }

void Framebuffer::unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0u); }

void Framebuffer::clear(float r, float g, float b, float a, bool clear_depth) const {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, width_, height_);
  glClearColor(r, g, b, a);
  GLbitfield mask = GL_COLOR_BUFFER_BIT;
  if (clear_depth && with_depth_) {
    mask |= GL_DEPTH_BUFFER_BIT;
    glClearDepth(1.0);
  }
  glClear(mask);
}

} // namespace cardiac::gl
