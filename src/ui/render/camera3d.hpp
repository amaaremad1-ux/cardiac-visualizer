#pragma once
// camera3d.hpp - damped orbit/pan/zoom camera for the 3D trajectory viewport.
// Header-only. All angles in radians; matrices column-major (see linmath.hpp).
#include "linmath.hpp"

#include <cmath>

namespace cardiac::gl {

class Camera3D {
public:
  // Current (smoothed) and goal (input-driven) state. update() eases current
  // toward goal so mouse motion feels fluid without per-frame jitter.
  float yaw = 0.7f, pitch = 0.45f, distance = 3.0f;
  float goal_yaw = 0.7f, goal_pitch = 0.45f, goal_distance = 3.0f;
  Vec3f target{0.f, 0.f, 0.f}, goal_target{0.f, 0.f, 0.f};
  float fov_deg = 45.f;
  float z_near = 0.02f, z_far = 100.f;

  void reset(float dist = 3.0f) {
    goal_yaw = 0.7f; goal_pitch = 0.45f; goal_distance = dist;
    goal_target = Vec3f{0.f, 0.f, 0.f};
  }

  // Frame a bounding sphere so the whole trajectory fits the viewport.
  void fit(Vec3f center, float radius) {
    goal_target = center;
    float half_fov = fov_deg * 0.5f * 3.14159265f / 180.f;
    float d = radius / std::sin(half_fov > 1e-4f ? half_fov : 1e-4f);
    goal_distance = d * 1.35f + 0.05f;
  }

  void orbit(float dx, float dy) {
    goal_yaw += dx * 0.006f;
    goal_pitch += dy * 0.006f;
    const float lim = 1.5533f; // ~89 deg, avoid gimbal flip
    if (goal_pitch > lim) goal_pitch = lim;
    if (goal_pitch < -lim) goal_pitch = -lim;
  }

  void pan(float dx, float dy) {
    // Move the target along the camera's right/up vectors, scaled by distance.
    Vec3f e = eye_from(yaw, pitch, goal_distance, target);
    Vec3f fwd = normalize(target - e);
    Vec3f right = normalize(cross(fwd, Vec3f{0.f, 1.f, 0.f}));
    Vec3f up = cross(right, fwd);
    float s = goal_distance * 0.0016f;
    goal_target = goal_target + right * (-dx * s) + up * (dy * s);
  }

  void zoom(float delta) {
    goal_distance *= std::exp(-delta * 0.0015f);
    if (goal_distance < 0.05f) goal_distance = 0.05f;
    if (goal_distance > 100.f) goal_distance = 100.f;
  }

  // Exponential smoothing; frame-rate independent via dt.
  void update(float dt) {
    float k = 1.f - std::exp(-dt * 12.f);
    yaw += (goal_yaw - yaw) * k;
    pitch += (goal_pitch - pitch) * k;
    distance += (goal_distance - distance) * k;
    target = lerp(target, goal_target, k);
  }

  static Vec3f eye_from(float y, float p, float dist, Vec3f tgt) {
    float cp = std::cos(p);
    return tgt + Vec3f{cp * std::sin(y), std::sin(p), cp * std::cos(y)} * dist;
  }

  Vec3f eye() const { return eye_from(yaw, pitch, distance, target); }

  Mat4 view() const { return look_at(eye(), target, Vec3f{0.f, 1.f, 0.f}); }

  Mat4 proj(float aspect) const {
    float f = fov_deg * 3.14159265f / 180.f;
    return perspective(f, aspect > 1e-4f ? aspect : 1e-4f, z_near, z_far);
  }
};

} // namespace cardiac::gl
