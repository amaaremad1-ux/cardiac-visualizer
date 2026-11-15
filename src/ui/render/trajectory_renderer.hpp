#pragma once
// trajectory_renderer.hpp - advanced GPU renderer for the 3D IBI delay-embedding.
//
// Replaces the old ImGui DrawList line rendering with a real OpenGL 3.3 core
// pipeline: a glowing camera-facing filament (screen-space thickened ribbon),
// an additive volumetric point-cloud aura, a GPU particle stream that flows
// backward from the newest valid beat along the trail, distance fog, and an
// HDR bloom post-process (bright-pass -> separable blur -> tone-mapped
// composite with a soft volumetric halo + vignette).
//
// Data-quality visual coding (matches plot_ibi_3d.py semantics and the core
// QualityController): the filament shades green->blue along valid data; segment
// breaks (gaps / rejected intervals) never connect and are marked with red
// particle bursts. The delay-coordinate math itself is untouched - it still
// comes from TrajectoryEngine via VisualizationSnapshot.
//
// ISOLATION: this header exposes NO OpenGL types. It is safe to include from
// ImGui translation units. All GL lives in the .cpp behind the cgl loader.
#include "camera3d.hpp"
#include "gl_framebuffer.hpp"
#include "gl_shader.hpp"

#include "cardiac/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cardiac::ui {

class TrajectoryRenderer {
public:
  TrajectoryRenderer();
  ~TrajectoryRenderer();
  TrajectoryRenderer(const TrajectoryRenderer&) = delete;
  TrajectoryRenderer& operator=(const TrajectoryRenderer&) = delete;

  // Loads GL entry points and compiles all shaders against the CURRENT context.
  // Returns false on failure; error() explains why (e.g. no 3.3 core context).
  bool init();
  bool ready() const { return ready_; }
  const std::string& error() const { return error_; }

  // Viewport size in pixels (the ImGui image region * framebuffer scale).
  void resize(int width, int height);
  int width() const { return width_; }
  int height() const { return height_; }

  // Rebuilds GPU buffers only when the snapshot revision/size changes. Cheap to
  // call every frame with an unchanged snapshot.
  void set_snapshot(const cardiac::VisualizationSnapshot& snap);

  // Aesthetic/behaviour knobs (all clamped to sane ranges).
  void set_glow(float intensity) { glow_ = intensity; }
  void set_bloom(float strength) { bloom_ = strength; }
  void set_particle_count(int n);
  void set_trail_flow(float speed) { flow_speed_ = speed; }
  void set_volumetric(float v) { volumetric_ = v; }

  cardiac::gl::Camera3D& camera() { return camera_; }
  const cardiac::gl::Camera3D& camera() const { return camera_; }

  // Advances animation by dt seconds and renders the full pipeline into the
  // internal composite FBO. Returns the output color texture id (0 if not
  // ready). Bind it with ImGui::Image((ImTextureID)(intptr_t)id, size).
  unsigned int render(float dt_seconds);
  unsigned int output_texture() const;

  // Diagnostics for the on-screen HUD / smoke test.
  std::size_t point_count() const { return cpu_pos_.size(); }
  std::size_t segment_count() const { return segments_.size(); }
  std::uint64_t revision() const { return revision_; }
  float last_frame_ms() const { return last_frame_ms_; }

private:
  struct Segment { std::uint32_t offset = 0; std::uint32_t count = 0; };

  void rebuild_buffers();
  void upload_pos_texture();
  void build_particle_seeds();
  bool build_shaders();
  void render_scene();
  void render_bloom();
  void render_composite();

  bool ready_ = false;
  std::string error_;

  int width_ = 800, height_ = 600;
  float glow_ = 1.0f;
  float bloom_ = 0.9f;
  float volumetric_ = 0.6f;
  float flow_speed_ = 0.35f;
  float time_ = 0.f;
  float last_frame_ms_ = 0.f;

  // Camera-fit transform: world = (p - center) * inv_radius.
  cardiac::Vec3 center_{0.0, 0.0, 0.0};
  float inv_radius_ = 1.f;
  cardiac::gl::Camera3D camera_;

  // CPU mirrors (double) -> uploaded as normalized float.
  std::vector<cardiac::Vec3> cpu_pos_;
  std::vector<float> cpu_t_;       // 0..1 along trail
  std::vector<float> cpu_break_;   // 1 at a segment-start (gap marker)
  std::vector<Segment> segments_;
  std::uint64_t revision_ = 0;
  std::size_t last_built_size_ = 0;

  // GL handles (plain unsigned ints; GL calls live in the .cpp).
  unsigned int line_vao_ = 0, line_vbo_ = 0;
  unsigned int cloud_vao_ = 0, cloud_vbo_ = 0;
  unsigned int quad_vao_ = 0, quad_vbo_ = 0;
  unsigned int part_vao_ = 0, part_seed_vbo_ = 0;
  unsigned int pos_tex_ = 0;
  unsigned int bloom_src_texture_ = 0;
  int particle_count_ = 3000;

  cardiac::gl::ShaderProgram filament_prog_;
  cardiac::gl::ShaderProgram cloud_prog_;
  cardiac::gl::ShaderProgram particle_prog_;
  cardiac::gl::ShaderProgram bright_prog_;
  cardiac::gl::ShaderProgram blur_prog_;
  cardiac::gl::ShaderProgram composite_prog_;

  cardiac::gl::Framebuffer scene_fbo_;   // HDR RGBA16F + depth
  cardiac::gl::Framebuffer bright_fbo_;  // half-res
  cardiac::gl::Framebuffer ping_fbo_;    // half-res blur A
  cardiac::gl::Framebuffer pong_fbo_;    // half-res blur B
  cardiac::gl::Framebuffer present_fbo_; // RGBA8 final shown in ImGui
};

} // namespace cardiac::ui
