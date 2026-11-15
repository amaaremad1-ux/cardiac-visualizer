#include "dashboard_draw.hpp"
#include <algorithm>
#include <cmath>
namespace cardiac::ui {
Vec3 rotate_pt(const Vec3& p, Vec3 c, const Camera& k) {
  double x = p.x - c.x, y = p.y - c.y, z = p.z - c.z;
  double cy = std::cos(k.yaw), sy = std::sin(k.yaw);
  double cp = std::cos(k.pitch), sp = std::sin(k.pitch);
  double x1 = x * cy - z * sy, z1 = x * sy + z * cy;
  double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
  return Vec3{x1, y2, z2};
}
void draw_traj(ImDrawList* dl, ImVec2 o, float size,
               const std::vector<TrajectoryPoint>& pts, const Camera& cam,
               std::uint64_t& hov) {
  if (pts.size() < 2) return;
  Vec3 c{0, 0, 0};
  for (auto& p : pts) { c.x += p.position.x; c.y += p.position.y; c.z += p.position.z; }
  double n = double(pts.size());
  c.x /= n; c.y /= n; c.z /= n;
  double r = 0.05;
  for (auto& p : pts) {
    r = std::max(r, std::abs(p.position.x - c.x));
    r = std::max(r, std::abs(p.position.y - c.y));
    r = std::max(r, std::abs(p.position.z - c.z));
  }
  float half = size * 0.5f * cam.scale;
  dl->AddRect(ImVec2(o.x - half - 8, o.y - half - 8), ImVec2(o.x + half + 8, o.y + half + 8),
              IM_COL32(90, 90, 110, 255));
  auto proj = [&](const Vec3& p) {
    Vec3 q = rotate_pt(p, c, cam);
    double d = 2.2 * r + q.z;
    double f = (2.0 * r) / std::max(d, 1e-6);
    return ImVec2(o.x + float(q.x / r) * half * float(f),
                  o.y - float(q.y / r) * half * float(f));
  };
  std::uint64_t seg = pts.front().segment_id;
  ImVec2 prev = proj(pts.front().position);
  for (std::size_t i = 1; i < pts.size(); ++i) {
    if (pts[i].segment_id != seg) {
      seg = pts[i].segment_id;
      prev = proj(pts[i].position);
      continue;
    }
    ImVec2 cur = proj(pts[i].position);
    float t = float(i) / float(pts.size());
    dl->AddLine(prev, cur, IM_COL32(int(80 + 120 * t), 200, int(255 - 120 * t), 255), 2.0f);
    prev = cur;
  }
  dl->AddCircleFilled(proj(pts.back().position), 5.0f, IM_COL32(255, 90, 90, 255));
  ImVec2 m = ImGui::GetMousePos();
  float best = 12.0f;
  hov = pts.back().index;
  for (auto& p : pts) {
    ImVec2 q = proj(p.position);
    float d = std::hypot(q.x - m.x, q.y - m.y);
    if (d < best) { best = d; hov = p.index; }
  }
}
void draw_areas(ImDrawList* dl, ImVec2 o, ImVec2 s, const AnalysisResult* a) {
  dl->AddRect(o, ImVec2(o.x + s.x, o.y + s.y), IM_COL32(90, 90, 110, 255));
  if (!a || a->triangle_areas.empty()) return;
  double peak = 1e-9;
  for (auto& t : a->triangle_areas) peak = std::max(peak, t.area_s2);
  std::size_t n = std::min<std::size_t>(a->triangle_areas.size(), 256);
  std::size_t s0 = a->triangle_areas.size() - n;
  ImVec2 prev{-1, -1};
  for (std::size_t i = 0; i < n; ++i) {
    double v = a->triangle_areas[s0 + i].area_s2 / peak;
    ImVec2 cur(o.x + s.x * float(i) / float(n), o.y + s.y * (1.0f - float(v)));
    if (prev.x >= 0) dl->AddLine(prev, cur, IM_COL32(120, 220, 140, 255), 1.5f);
    prev = cur;
  }
}
void draw_quality_lane(ImDrawList* dl, ImVec2 o, ImVec2 s,
                       const std::vector<QualitySample>& q) {
  dl->AddRect(o, ImVec2(o.x + s.x, o.y + s.y), IM_COL32(90, 90, 110, 255));
  if (q.empty()) return;
  const float bw = s.x / float(q.size());
  std::uint64_t seg = q.front().segment_id;
  for (std::size_t i = 0; i < q.size(); ++i) {
    const float x0 = o.x + bw * float(i);
    const float x1 = x0 + std::max(bw - 0.5f, 1.0f);
    const ImU32 col =
        q[i].is_valid ? IM_COL32(90, 200, 120, 255) : IM_COL32(230, 90, 70, 255);
    dl->AddRectFilled(ImVec2(x0, o.y + 2), ImVec2(x1, o.y + s.y - 2), col);
    if (q[i].segment_id != seg) {
      seg = q[i].segment_id;
      dl->AddLine(ImVec2(x0, o.y), ImVec2(x0, o.y + s.y),
                  IM_COL32(255, 255, 255, 255), 1.5f);
    }
  }
}
void draw_area_time(ImDrawList* dl, ImVec2 o, ImVec2 s,
                    const std::vector<AreaSample>& a) {
  dl->AddRect(o, ImVec2(o.x + s.x, o.y + s.y), IM_COL32(90, 90, 110, 255));
  if (a.size() < 2) return;
  double peak = 1e-9, tmax = 1e-9;
  for (const auto& t : a) {
    peak = std::max(peak, t.area_s2);
    tmax = std::max(tmax, t.elapsed_time_s);
  }
  ImVec2 prev{-1, -1};
  std::uint64_t seg = a.front().segment_id;
  for (const auto& t : a) {
    const float x = o.x + s.x * float(t.elapsed_time_s / tmax);
    const float y = o.y + s.y * (1.0f - float(t.area_s2 / peak));
    if (t.segment_id != seg) {
      seg = t.segment_id;  // break: lift pen, never interpolate
      prev = ImVec2{-1, -1};
    }
    if (prev.x >= 0) dl->AddLine(prev, ImVec2(x, y), IM_COL32(120, 220, 140, 255), 1.5f);
    prev = ImVec2(x, y);
  }
}
}  // namespace cardiac::ui
