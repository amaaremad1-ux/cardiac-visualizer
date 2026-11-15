#pragma once
// linmath.hpp - minimal float vec3/mat4 math for the renderer camera.
// Self-contained (no GLM). Column-major matrices, matching GLSL convention:
// m[col*4 + row]. Header-only; included by render TUs only.
#include <cmath>

namespace cardiac::gl {

struct Vec2f {
  float x{0.f}, y{0.f};
  Vec2f() = default;
  Vec2f(float xx, float yy) : x(xx), y(yy) {}
};

struct Vec3f {
  float x{0.f}, y{0.f}, z{0.f};
  Vec3f() = default;
  Vec3f(float xx, float yy, float zz) : x(xx), y(yy), z(zz) {}
};

inline Vec3f operator+(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3f operator-(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3f operator*(Vec3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3f operator*(float s, Vec3f a) { return a * s; }
inline Vec3f operator/(Vec3f a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline float dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross(Vec3f a, Vec3f b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3f a) { return std::sqrt(dot(a, a)); }
inline Vec3f normalize(Vec3f a) {
  float l = length(a);
  return l > 1e-8f ? a / l : Vec3f{0.f, 0.f, 0.f};
}
inline Vec3f lerp(Vec3f a, Vec3f b, float t) { return a + (b - a) * t; }

struct Mat4 {
  float m[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major identity
  static Mat4 identity() { return Mat4{}; }
};

inline Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  }
  return r;
}

inline Mat4 perspective(float fovy_rad, float aspect, float z_near, float z_far) {
  Mat4 r;
  for (int i = 0; i < 16; ++i) r.m[i] = 0.f;
  float f = 1.f / std::tan(fovy_rad * 0.5f);
  r.m[0] = f / (aspect > 1e-6f ? aspect : 1e-6f);
  r.m[5] = f;
  r.m[10] = (z_far + z_near) / (z_near - z_far);
  r.m[11] = -1.f;
  r.m[14] = (2.f * z_far * z_near) / (z_near - z_far);
  return r;
}

inline Mat4 look_at(Vec3f eye, Vec3f center, Vec3f up) {
  Vec3f f = normalize(center - eye);
  Vec3f s = normalize(cross(f, up));
  Vec3f u = cross(s, f);
  Mat4 r; // rotation then translation, column-major
  r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;   r.m[12] = -dot(s, eye);
  r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;   r.m[13] = -dot(u, eye);
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
  r.m[3] = 0.f; r.m[7] = 0.f; r.m[11] = 0.f;  r.m[15] = 1.f;
  return r;
}

inline Mat4 translate(Vec3f t) {
  Mat4 r;
  r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
  return r;
}

inline Mat4 scale_uniform(float s) {
  Mat4 r;
  r.m[0] = s; r.m[5] = s; r.m[10] = s;
  return r;
}

} // namespace cardiac::gl
