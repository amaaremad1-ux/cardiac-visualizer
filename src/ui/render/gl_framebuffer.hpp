#pragma once
// gl_framebuffer.hpp - RAII offscreen FBO with one color texture + depth.
#include <cstdint>

namespace cardiac::gl {

// A single-attachment color FBO used for the scene pass and each bloom mip.
// When hdr is true the color texture is RGBA16F (values > 1 survive for the
// bright-pass), otherwise RGBA8. A depth renderbuffer is attached only when
// with_depth is true (the scene pass needs it; bloom mips do not).
class Framebuffer {
public:
  Framebuffer() = default;
  ~Framebuffer();
  Framebuffer(const Framebuffer&) = delete;
  Framebuffer& operator=(const Framebuffer&) = delete;

  bool create(int width, int height, bool hdr, bool with_depth);
  void resize(int width, int height);
  void destroy();

  void bind() const;             // binds this FBO as draw+read target
  static void unbind();          // binds the default framebuffer (0)
  void clear(float r, float g, float b, float a, bool clear_depth) const;

  unsigned int fbo() const { return fbo_; }
  unsigned int color_texture() const { return color_tex_; }
  int width() const { return width_; }
  int height() const { return height_; }
  bool valid() const { return fbo_ != 0u && complete_; }

private:
  unsigned int fbo_ = 0u;
  unsigned int color_tex_ = 0u;
  unsigned int depth_rb_ = 0u;
  int width_ = 0;
  int height_ = 0;
  bool hdr_ = false;
  bool with_depth_ = false;
  bool complete_ = false;
};

} // namespace cardiac::gl
