#pragma once
#include "cardiac/rolling_metrics.hpp"
#include "cardiac/types.hpp"
#include "imgui.h"
#include <cstdint>
#include <vector>
namespace cardiac::ui {
struct Camera { float yaw{0.7f}; float pitch{0.45f}; float scale{1.0f}; };
Vec3 rotate_pt(const Vec3& p, Vec3 center, const Camera& cam);
void draw_traj(ImDrawList* dl, ImVec2 origin, float size,
               const std::vector<TrajectoryPoint>& pts, const Camera& cam,
               std::uint64_t& hovered);
void draw_areas(ImDrawList* dl, ImVec2 origin, ImVec2 size, const AnalysisResult* a);
// Step 3: signal-quality lane (green=valid, red=rejected, white tick=break)
// and area-vs-elapsed-time plot (pen lifts at segment breaks).
void draw_quality_lane(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                       const std::vector<QualitySample>& q);
void draw_area_time(ImDrawList* dl, ImVec2 origin, ImVec2 size,
                    const std::vector<AreaSample>& a);
}  // namespace cardiac::ui
