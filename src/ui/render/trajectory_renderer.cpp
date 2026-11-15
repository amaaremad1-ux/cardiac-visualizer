// trajectory_renderer.cpp - advanced glowing-filament 3D renderer.
// Isolated GL translation unit: includes cgl.hpp (which remaps gl* -> cg_gl*).
#include "trajectory_renderer.hpp"

#include "cgl.hpp"
#include "linmath.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <utility>

namespace cardiac::ui {
namespace {

// ---------------------------------------------------------------------------
// GLSL 330 core sources
// ---------------------------------------------------------------------------

// Screen-space thickened ribbon: expands each polyline point into a camera-
// facing quad of constant pixel thickness, with an energy pulse and depth fog.
const char* kFilamentVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNext;
layout(location=2) in float aSide;
layout(location=3) in float aT;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec2 uResolution;
uniform float uThickness;
out float vT;
out float vSide;
out float vFog;
void main() {
  vec4 clip = uProj * uView * vec4(aPos, 1.0);
  vec4 clipNext = uProj * uView * vec4(aNext, 1.0);
  float w = max(clip.w, 1e-4);
  float wn = max(clipNext.w, 1e-4);
  vec2 curPx = (clip.xy / w * 0.5 + 0.5) * uResolution;
  vec2 nxtPx = (clipNext.xy / wn * 0.5 + 0.5) * uResolution;
  vec2 dirPx = nxtPx - curPx;
  float len = length(dirPx);
  dirPx = len > 1e-4 ? dirPx / len : vec2(1.0, 0.0);
  vec2 nrmPx = vec2(-dirPx.y, dirPx.x);
  vec2 offPx = nrmPx * (aSide * uThickness * 0.5);
  clip.xy += (offPx / uResolution * 2.0) * w;
  gl_Position = clip;
  vT = aT;
  vSide = aSide;
  vFog = clamp((w - 1.0) / 4.0, 0.0, 1.0);
}
)GLSL";

const char* kFilamentFS = R"GLSL(
#version 330 core
in float vT;
in float vSide;
in float vFog;
uniform vec3 uColorA;
uniform vec3 uColorB;
uniform vec3 uBreakColor;
uniform float uGlow;
uniform float uTime;
out vec4 fragColor;
void main() {
  float body = 1.0 - abs(vSide);
  float core = pow(clamp(body, 0.0, 1.0), 2.0);
  float edge = smoothstep(0.0, 0.4, body);
  vec3 col = mix(uColorA, uColorB, vT);
  float pulse = 0.65 + 0.35 * sin(vT * 46.0 - uTime * 3.2);
  float intensity = (0.22 + core * 1.5) * (0.7 + 0.5 * pulse) * uGlow;
  float alpha = edge * (0.5 + core * 0.5) * (1.0 - vFog * 0.8);
  fragColor = vec4(col * intensity, alpha);
}
)GLSL";

// Additive volumetric aura: soft round sprites at every trajectory point.
const char* kCloudVS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in float aT;
layout(location=2) in float aBrk;
uniform mat4 uView;
uniform mat4 uProj;
uniform float uPointScale;
uniform float uSize;
out float vT;
out float vBrk;
void main() {
  vec4 clip = uProj * uView * vec4(aPos, 1.0);
  gl_Position = clip;
  float w = max(clip.w, 1e-3);
  gl_PointSize = clamp(uSize * uPointScale / w, 1.0, 64.0);
  vT = aT;
  vBrk = aBrk;
}
)GLSL";

const char* kCloudFS = R"GLSL(
#version 330 core
in float vT;
in float vBrk;
uniform vec3 uColorA;
uniform vec3 uColorB;
uniform vec3 uBreakColor;
uniform float uGlow;
out vec4 fragColor;
void main() {
  vec2 d = gl_PointCoord - vec2(0.5);
  float r = length(d) * 2.0;
  if (r > 1.0) discard;
  float fall = pow(1.0 - r, 2.2);
  vec3 col = mix(uColorA, uColorB, vT);
  col = mix(col, uBreakColor, vBrk);
  float a = fall * (0.35 + 0.65 * vBrk) * uGlow;
  fragColor = vec4(col * a * 1.4, a);
}
)GLSL";

// GPU particle stream flowing backward from the newest beat along the trail,
// sampling the trajectory position texture; red near segment breaks (gaps).
const char* kParticleVS = R"GLSL(
#version 330 core
layout(location=0) in vec4 aSeed;   // x=phase y=speed z=radialAng w=radialMag
uniform mat4 uView;
uniform mat4 uProj;
uniform sampler2D uPosTex;
uniform float uTime;
uniform float uCount;
uniform float uHead;
uniform float uTrail;
uniform float uPointScale;
out float vLife;
out float vBrk;
out float vT;
void main() {
  float age = fract(uTime * (0.08 + aSeed.y * 0.22) + aSeed.x);
  float idx = floor(clamp(uHead - age * uTrail, 0.0, max(uCount - 1.0, 0.0)));
  vec4 p = texture(uPosTex, vec2((idx + 0.5) / max(uCount, 1.0), 0.5));
  float ang = aSeed.z * 6.2831853 + uTime * 0.7;
  float rad = age * age * (0.02 + 0.09 * aSeed.w);
  vec3 pos = p.rgb + vec3(cos(ang) * rad, sin(ang * 1.7) * rad * 0.7, sin(ang) * rad);
  vec4 clip = uProj * uView * vec4(pos, 1.0);
  gl_Position = clip;
  float w = max(clip.w, 1e-3);
  gl_PointSize = clamp((2.0 + 5.0 * (1.0 - age)) * uPointScale / w, 1.0, 40.0);
  vLife = 1.0 - age;
  vBrk = p.a;
  vT = idx / max(uCount - 1.0, 1.0);
}
)GLSL";

const char* kParticleFS = R"GLSL(
#version 330 core
in float vLife;
in float vBrk;
in float vT;
uniform vec3 uColorA;
uniform vec3 uColorB;
uniform vec3 uBreakColor;
uniform float uGlow;
out vec4 fragColor;
void main() {
  vec2 d = gl_PointCoord - vec2(0.5);
  float r = length(d) * 2.0;
  if (r > 1.0) discard;
  float fall = pow(1.0 - r, 2.0);
  vec3 col = mix(uColorA, uColorB, vT);
  col = mix(col, uBreakColor, clamp(vBrk, 0.0, 1.0));
  float a = fall * vLife * uGlow;
  fragColor = vec4(col * a * 1.8, a);
}
)GLSL";

// Fullscreen passes share this trivial vertex shader.
const char* kQuadVS = R"GLSL(
#version 330 core
layout(location=0) in vec2 aPos;
out vec2 vUV;
void main() {
  vUV = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* kBrightFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform float uThreshold;
out vec4 fragColor;
void main() {
  vec3 c = texture(uTex, vUV).rgb;
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float k = max(l - uThreshold, 0.0) / max(l, 1e-4);
  fragColor = vec4(c * k, 1.0);
}
)GLSL";

const char* kBlurFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uDir;   // texel step * direction
out vec4 fragColor;
void main() {
  vec3 sum = texture(uTex, vUV).rgb * 0.227027;
  sum += texture(uTex, vUV + uDir * 1.3846153846).rgb * 0.3162162162;
  sum += texture(uTex, vUV - uDir * 1.3846153846).rgb * 0.3162162162;
  sum += texture(uTex, vUV + uDir * 3.2307692308).rgb * 0.0702702703;
  sum += texture(uTex, vUV - uDir * 3.2307692308).rgb * 0.0702702703;
  fragColor = vec4(sum, 1.0);
}
)GLSL";

// Tone-map scene + bloom, add a soft volumetric halo around the newest beat and
// a gentle vignette. Output is display-referred RGBA8 for ImGui.
const char* kCompositeFS = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomStrength;
uniform float uVolumetric;
uniform vec2 uHeadUV;
uniform float uExposure;
out vec4 fragColor;
vec3 aces(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
void main() {
  vec3 scene = texture(uScene, vUV).rgb;
  vec3 bloom = texture(uBloom, vUV).rgb;
  vec3 col = scene + bloom * uBloomStrength;
  // fake volumetric light shaft radiating from the head of the trail
  vec2 dv = vUV - uHeadUV;
  float halo = exp(-dot(dv, dv) * 14.0) * uVolumetric;
  col += vec3(0.25, 0.65, 0.85) * halo * 0.5;
  col = aces(col * uExposure);
  float vig = smoothstep(1.25, 0.35, length(dv * vec2(1.0, 1.0)));
  col *= mix(0.72, 1.0, vig);
  col = pow(col, vec3(1.0 / 2.2));
  fragColor = vec4(col, 1.0);
}
)GLSL";

float* map3(std::vector<float>& v) { return v.empty() ? nullptr : v.data(); }

} // namespace

// ---------------------------------------------------------------------------

TrajectoryRenderer::TrajectoryRenderer() = default;

TrajectoryRenderer::~TrajectoryRenderer() {
  if (!ready_) return;
  if (line_vbo_) glDeleteBuffers(1, &line_vbo_);
  if (line_vao_) glDeleteVertexArrays(1, &line_vao_);
  if (cloud_vbo_) glDeleteBuffers(1, &cloud_vbo_);
  if (cloud_vao_) glDeleteVertexArrays(1, &cloud_vao_);
  if (quad_vbo_) glDeleteBuffers(1, &quad_vbo_);
  if (quad_vao_) glDeleteVertexArrays(1, &quad_vao_);
  if (part_seed_vbo_) glDeleteBuffers(1, &part_seed_vbo_);
  if (part_vao_) glDeleteVertexArrays(1, &part_vao_);
  if (pos_tex_) glDeleteTextures(1, &pos_tex_);
}

bool TrajectoryRenderer::init() {
  error_.clear();
  if (!cardiac::gl::load()) {
    error_ = cardiac::gl::error();
    return false;
  }
  int maj = cardiac::gl::major_version();
  int min = cardiac::gl::minor_version();
  if (maj < 3 || (maj == 3 && min < 3)) {
    // Continue anyway; some drivers under-report. Shaders will surface errors.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "GL %d.%d reported (<3.3); attempting anyway", maj, min);
    error_ = buf;
  }
  if (!build_shaders()) {
    ready_ = false;
    return false;
  }

  glGenVertexArrays(1, &line_vao_);
  glGenBuffers(1, &line_vbo_);
  glGenVertexArrays(1, &cloud_vao_);
  glGenBuffers(1, &cloud_vbo_);
  glGenVertexArrays(1, &part_vao_);
  glGenBuffers(1, &part_seed_vbo_);
  glGenVertexArrays(1, &quad_vao_);
  glGenBuffers(1, &quad_vbo_);
  glGenTextures(1, &pos_tex_);

  // Fullscreen quad (triangle strip).
  static const float quad[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
  glBindVertexArray(quad_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(quad)), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
  glBindVertexArray(0);

  // Position texture params (data uploaded in upload_pos_texture).
  glBindTexture(GL_TEXTURE_2D, pos_tex_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  build_particle_seeds();
  resize(width_, height_);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "GL error after init: 0x%04X", err);
    error_ = buf;
    return false;
  }
  ready_ = true;
  error_.clear();
  return true;
}

bool TrajectoryRenderer::build_shaders() {
  if (!filament_prog_.build(kFilamentVS, kFilamentFS)) { error_ = "filament: " + filament_prog_.log(); return false; }
  if (!cloud_prog_.build(kCloudVS, kCloudFS))         { error_ = "cloud: " + cloud_prog_.log(); return false; }
  if (!particle_prog_.build(kParticleVS, kParticleFS)){ error_ = "particle: " + particle_prog_.log(); return false; }
  if (!bright_prog_.build(kQuadVS, kBrightFS))        { error_ = "bright: " + bright_prog_.log(); return false; }
  if (!blur_prog_.build(kQuadVS, kBlurFS))            { error_ = "blur: " + blur_prog_.log(); return false; }
  if (!composite_prog_.build(kQuadVS, kCompositeFS))  { error_ = "composite: " + composite_prog_.log(); return false; }
  return true;
}

void TrajectoryRenderer::resize(int width, int height) {
  width_ = std::max(1, width);
  height_ = std::max(1, height);
  int hw = std::max(1, width_ / 2);
  int hh = std::max(1, height_ / 2);
  scene_fbo_.create(width_, height_, /*hdr=*/true, /*with_depth=*/false);
  bright_fbo_.create(hw, hh, /*hdr=*/true, /*with_depth=*/false);
  ping_fbo_.create(hw, hh, /*hdr=*/true, /*with_depth=*/false);
  pong_fbo_.create(hw, hh, /*hdr=*/true, /*with_depth=*/false);
  present_fbo_.create(width_, height_, /*hdr=*/false, /*with_depth=*/false);
}

void TrajectoryRenderer::set_particle_count(int n) {
  n = std::clamp(n, 0, 20000);
  if (n == particle_count_) return;
  particle_count_ = n;
  if (ready_) build_particle_seeds();
}

void TrajectoryRenderer::build_particle_seeds() {
  std::mt19937 rng(1337u); // deterministic: same demo run looks the same
  std::uniform_real_distribution<float> u(0.f, 1.f);
  std::vector<float> seeds(static_cast<size_t>(particle_count_) * 4u);
  for (size_t i = 0; i < seeds.size(); ++i) seeds[i] = u(rng);
  glBindVertexArray(part_vao_);
  glBindBuffer(GL_ARRAY_BUFFER, part_seed_vbo_);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(seeds.size() * sizeof(float)),
               map3(seeds), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
  glBindVertexArray(0);
}

void TrajectoryRenderer::set_snapshot(const cardiac::VisualizationSnapshot& snap) {
  if (snap.revision == revision_ && snap.points.size() == last_built_size_) return;
  revision_ = snap.revision;
  cpu_pos_.clear();
  cpu_t_.clear();
  cpu_break_.clear();
  segments_.clear();
  const auto& pts = snap.points;
  if (pts.empty()) {
    inv_radius_ = 1.f;
    center_ = cardiac::Vec3{0.0, 0.0, 0.0};
    last_built_size_ = pts.size();
    return;
  }
  // Bounding box -> center + radius for a stable unit-sphere fit.
  double minx = 1e30, miny = 1e30, minz = 1e30;
  double maxx = -1e30, maxy = -1e30, maxz = -1e30;
  for (const auto& p : pts) {
    minx = std::min(minx, p.position.x); maxx = std::max(maxx, p.position.x);
    miny = std::min(miny, p.position.y); maxy = std::max(maxy, p.position.y);
    minz = std::min(minz, p.position.z); maxz = std::max(maxz, p.position.z);
  }
  center_ = cardiac::Vec3{(minx + maxx) * 0.5, (miny + maxy) * 0.5, (minz + maxz) * 0.5};
  double radius = 1e-6;
  for (const auto& p : pts) {
    double dx = p.position.x - center_.x, dy = p.position.y - center_.y, dz = p.position.z - center_.z;
    radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  inv_radius_ = static_cast<float>(1.0 / radius);

  cpu_pos_.reserve(pts.size());
  cpu_t_.reserve(pts.size());
  cpu_break_.reserve(pts.size());
  double n = static_cast<double>(pts.size());
  std::uint64_t prev_seg = pts.front().segment_id;
  for (size_t i = 0; i < pts.size(); ++i) {
    const auto& p = pts[i];
    cpu_pos_.push_back(cardiac::Vec3{
        (p.position.x - center_.x) * inv_radius_,
        (p.position.y - center_.y) * inv_radius_,
        (p.position.z - center_.z) * inv_radius_});
    cpu_t_.push_back(n > 1.0 ? static_cast<float>(static_cast<double>(i) / (n - 1.0)) : 1.f);
    bool brk = (i > 0 && p.segment_id != prev_seg);
    cpu_break_.push_back(brk ? 1.f : 0.f);
    if (i == 0 || p.segment_id != prev_seg) {
      segments_.push_back(Segment{static_cast<std::uint32_t>(i), 0u});
    }
    if (!segments_.empty()) segments_.back().count++;
    prev_seg = p.segment_id;
  }
  last_built_size_ = pts.size();
  rebuild_buffers();
}

void TrajectoryRenderer::rebuild_buffers() {
  if (!ready_) return;
  const size_t n = cpu_pos_.size();

  // --- Filament ribbon: 2 verts per point, per segment strips. Interleaved:
  // pos(3) next(3) side(1) t(1)  = 8 floats.
  {
    std::vector<float> data;
    data.reserve(n * 2u * 8u);
    for (const auto& seg : segments_) {
      if (seg.count < 2) continue;
      for (std::uint32_t k = 0; k < seg.count; ++k) {
        size_t i = seg.offset + k;
        size_t j = (k + 1 < seg.count) ? i + 1 : i;      // next (clamp at end)
        size_t prev = (k > 0) ? i - 1 : i;
        const Vec3& a = cpu_pos_[i];
        // tangent target: next point, or extrapolate from prev at the tail
        float nx = static_cast<float>(cpu_pos_[j].x), ny = static_cast<float>(cpu_pos_[j].y), nz = static_cast<float>(cpu_pos_[j].z);
        if (j == i) { nx = static_cast<float>(a.x + (a.x - cpu_pos_[prev].x)); ny = static_cast<float>(a.y + (a.y - cpu_pos_[prev].y)); nz = static_cast<float>(a.z + (a.z - cpu_pos_[prev].z)); }
        float t = cpu_t_[i];
        for (int s = -1; s <= 1; s += 2) {
          data.push_back(static_cast<float>(a.x));
          data.push_back(static_cast<float>(a.y));
          data.push_back(static_cast<float>(a.z));
          data.push_back(nx); data.push_back(ny); data.push_back(nz);
          data.push_back(static_cast<float>(s));
          data.push_back(t);
        }
      }
    }
    glBindVertexArray(line_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 map3(data), GL_DYNAMIC_DRAW);
    const GLsizei stride = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(7 * sizeof(float)));
    glBindVertexArray(0);
  }

  // --- Cloud: pos(3) t(1) brk(1) = 5 floats, one per point.
  {
    std::vector<float> data;
    data.reserve(n * 5u);
    for (size_t i = 0; i < n; ++i) {
      data.push_back(static_cast<float>(cpu_pos_[i].x));
      data.push_back(static_cast<float>(cpu_pos_[i].y));
      data.push_back(static_cast<float>(cpu_pos_[i].z));
      data.push_back(cpu_t_[i]);
      data.push_back(cpu_break_[i]);
    }
    glBindVertexArray(cloud_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, cloud_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 map3(data), GL_DYNAMIC_DRAW);
    const GLsizei stride = 5 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(4 * sizeof(float)));
    glBindVertexArray(0);
  }

  upload_pos_texture();
}

void TrajectoryRenderer::upload_pos_texture() {
  const size_t n = cpu_pos_.size();
  if (n == 0) return;
  std::vector<float> px(n * 4u);
  for (size_t i = 0; i < n; ++i) {
    px[i * 4u + 0u] = static_cast<float>(cpu_pos_[i].x);
    px[i * 4u + 1u] = static_cast<float>(cpu_pos_[i].y);
    px[i * 4u + 2u] = static_cast<float>(cpu_pos_[i].z);
    px[i * 4u + 3u] = cpu_break_[i];
  }
  glBindTexture(GL_TEXTURE_2D, pos_tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, static_cast<GLsizei>(n), 1, 0, GL_RGBA, GL_FLOAT,
               px.data());
  glBindTexture(GL_TEXTURE_2D, 0);
}

void TrajectoryRenderer::render_scene() {
  scene_fbo_.bind();
  glViewport(0, 0, scene_fbo_.width(), scene_fbo_.height());
  glClearColor(0.01f, 0.015f, 0.03f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (cpu_pos_.empty()) return;

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive glow
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_PROGRAM_POINT_SIZE);

  using namespace cardiac::gl;
  Mat4 view = camera_.view();
  float aspect = static_cast<float>(scene_fbo_.width()) / static_cast<float>(scene_fbo_.height());
  Mat4 proj = camera_.proj(aspect);
  Vec2f res{static_cast<float>(scene_fbo_.width()), static_cast<float>(scene_fbo_.height())};
  float point_scale = static_cast<float>(scene_fbo_.height()) * 0.5f;

  const float cA[3] = {0.10f, 0.95f, 0.45f}; // valid: green
  const float cB[3] = {0.20f, 0.55f, 1.00f}; // valid: blue (toward head)
  const float cR[3] = {1.00f, 0.22f, 0.18f}; // gap/break: red

  // --- cloud aura (under the filament)
  cloud_prog_.use();
  cloud_prog_.set_mat4("uView", view.m);
  cloud_prog_.set_mat4("uProj", proj.m);
  cloud_prog_.set_float("uPointScale", point_scale);
  cloud_prog_.set_float("uSize", 6.0f);
  cloud_prog_.set_vec3("uColorA", cA[0], cA[1], cA[2]);
  cloud_prog_.set_vec3("uColorB", cB[0], cB[1], cB[2]);
  cloud_prog_.set_vec3("uBreakColor", cR[0], cR[1], cR[2]);
  cloud_prog_.set_float("uGlow", glow_ * 0.5f);
  glBindVertexArray(cloud_vao_);
  glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(cpu_pos_.size()));

  // --- filament ribbon, one strip per contiguous segment (never bridge gaps)
  filament_prog_.use();
  filament_prog_.set_mat4("uView", view.m);
  filament_prog_.set_mat4("uProj", proj.m);
  filament_prog_.set_vec2("uResolution", res.x, res.y);
  filament_prog_.set_float("uThickness", 3.2f);
  filament_prog_.set_float("uTime", time_);
  filament_prog_.set_vec3("uColorA", cA[0], cA[1], cA[2]);
  filament_prog_.set_vec3("uColorB", cB[0], cB[1], cB[2]);
  filament_prog_.set_vec3("uBreakColor", cR[0], cR[1], cR[2]);
  filament_prog_.set_float("uGlow", glow_);
  glBindVertexArray(line_vao_);
  GLsizei base = 0;
  for (const auto& seg : segments_) {
    GLsizei count = static_cast<GLsizei>(seg.count) * 2;
    if (seg.count >= 2) glDrawArrays(GL_TRIANGLE_STRIP, base, count);
    base += count;
  }

  // --- particle stream from the head backward along the trail
  if (particle_count_ > 0 && cpu_pos_.size() >= 2) {
    particle_prog_.use();
    particle_prog_.set_mat4("uView", view.m);
    particle_prog_.set_mat4("uProj", proj.m);
    particle_prog_.set_float("uTime", time_);
    particle_prog_.set_float("uPointScale", point_scale);
    particle_prog_.set_int("uPosTex", 0);
    float cnt = static_cast<float>(cpu_pos_.size());
    particle_prog_.set_float("uCount", cnt);
    particle_prog_.set_float("uHead", cnt - 1.f);
    float trail = std::min(cnt, std::max(24.f, cnt * flow_speed_));
    particle_prog_.set_float("uTrail", trail);
    particle_prog_.set_vec3("uColorA", cA[0], cA[1], cA[2]);
    particle_prog_.set_vec3("uColorB", cB[0], cB[1], cB[2]);
    particle_prog_.set_vec3("uBreakColor", cR[0], cR[1], cR[2]);
    particle_prog_.set_float("uGlow", glow_ * 0.8f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pos_tex_);
    glBindVertexArray(part_vao_);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(particle_count_));
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glBindVertexArray(0);
  glUseProgram(0);
  glDisable(GL_PROGRAM_POINT_SIZE);
}

void TrajectoryRenderer::render_bloom() {
  glDisable(GL_BLEND);
  int hw = bright_fbo_.width(), hh = bright_fbo_.height();

  // Bright-pass: scene -> bright_fbo (half res).
  bright_fbo_.bind();
  glViewport(0, 0, hw, hh);
  bright_prog_.use();
  bright_prog_.set_int("uTex", 0);
  bright_prog_.set_float("uThreshold", 0.55f);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_fbo_.color_texture());
  glBindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // Separable blur ping-pong (2 iterations) at half res.
  blur_prog_.use();
  blur_prog_.set_int("uTex", 0);
  cardiac::gl::Framebuffer* src = &bright_fbo_;
  cardiac::gl::Framebuffer* dst = &ping_fbo_;
  for (int i = 0; i < 2; ++i) {
    // horizontal
    dst->bind();
    glViewport(0, 0, hw, hh);
    blur_prog_.set_vec2("uDir", 1.f / static_cast<float>(hw), 0.f);
    glBindTexture(GL_TEXTURE_2D, src->color_texture());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::swap(src, dst);
    // vertical
    dst->bind();
    glViewport(0, 0, hw, hh);
    blur_prog_.set_vec2("uDir", 0.f, 1.f / static_cast<float>(hh));
    glBindTexture(GL_TEXTURE_2D, src->color_texture());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::swap(src, dst);
  }
  bloom_src_texture_ = src->color_texture();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

void TrajectoryRenderer::render_composite() {
  present_fbo_.bind();
  glViewport(0, 0, present_fbo_.width(), present_fbo_.height());
  glDisable(GL_BLEND);

  // Project the head point to screen UV for the volumetric halo.
  using namespace cardiac::gl;
  Mat4 view = camera_.view();
  float aspect = static_cast<float>(width_) / static_cast<float>(height_);
  Mat4 proj = camera_.proj(aspect);
  Vec2f head_uv{0.5f, 0.5f};
  if (!cpu_pos_.empty()) {
    const Vec3& h = cpu_pos_.back();
    Vec3f hp{static_cast<float>(h.x), static_cast<float>(h.y), static_cast<float>(h.z)};
    Mat4 vp = mul(proj, view);
    float x = vp.m[0] * hp.x + vp.m[4] * hp.y + vp.m[8] * hp.z + vp.m[12];
    float y = vp.m[1] * hp.x + vp.m[5] * hp.y + vp.m[9] * hp.z + vp.m[13];
    float w = vp.m[3] * hp.x + vp.m[7] * hp.y + vp.m[11] * hp.z + vp.m[15];
    if (std::fabs(w) > 1e-5f) head_uv = Vec2f{x / w * 0.5f + 0.5f, y / w * 0.5f + 0.5f};
  }

  composite_prog_.use();
  composite_prog_.set_int("uScene", 0);
  composite_prog_.set_int("uBloom", 1);
  composite_prog_.set_float("uBloomStrength", bloom_);
  composite_prog_.set_float("uVolumetric", volumetric_);
  composite_prog_.set_float("uExposure", 1.15f);
  composite_prog_.set_vec2("uHeadUV", head_uv.x, head_uv.y);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_fbo_.color_texture());
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, bloom_src_texture_);
  glBindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);
  glUseProgram(0);
}

unsigned int TrajectoryRenderer::render(float dt_seconds) {
  if (!ready_) return 0u;
  auto t0 = std::chrono::steady_clock::now();
  if (dt_seconds > 0.f && dt_seconds < 0.25f) time_ += dt_seconds;
  else time_ += 1.f / 60.f;
  camera_.update(dt_seconds > 0.f ? dt_seconds : 1.f / 60.f);

  render_scene();
  render_bloom();
  render_composite();
  cardiac::gl::Framebuffer::unbind();

  auto t1 = std::chrono::steady_clock::now();
  last_frame_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();
  return present_fbo_.color_texture();
}

unsigned int TrajectoryRenderer::output_texture() const {
  return present_fbo_.color_texture();
}

} // namespace cardiac::ui
