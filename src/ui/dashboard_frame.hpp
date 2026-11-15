#pragma once
#include "cardiac/dashboard.hpp"
#include "cardiac/database_manager.hpp"
#include "cardiac/grid_viewport.hpp"
#include "cardiac/hrv_spectrum.hpp"
#include "cardiac/rolling_metrics.hpp"
#include "cardiac/types.hpp"
#include "dashboard_draw.hpp"
#include "trajectory_renderer.hpp"
#include <GLFW/glfw3.h>
#include <memory>
#include <string>
#include <vector>
namespace cardiac::ui {

// Persistent replay state. When a past or research session is loaded from the
// DatabaseManager it is shown in place of the live stream until cleared, so the
// advanced 3D renderer + all analytics panels replay stored data unchanged.
struct ReplayView {
  bool active = false;
  std::string label;
  VisualizationSnapshot viz;
  SessionSnapshot full;
  // Startup auto-load: the app seeds startup_session_id (from DashboardOptions);
  // the frame requests it once the DB is ready and marks startup_done so it is
  // only asked for a single time.
  std::string startup_session_id;
  bool startup_done = false;
  // Cached derived analytics for the stored session. A replayed record never
  // changes, so these are computed once at load (rolling metrics recomputed only
  // when a window slider moves) instead of every frame -- this is what keeps the
  // ~20k-beat CHF2DB research replays interactive (>30 FPS).
  bool analytics_valid = false;
  int rolling_area_win = -1;
  int rolling_qual_win = -1;
  RollingMetrics rolling;
  HrvSpectrum hrv;
  std::vector<QualitySample> quality_timeline;
  std::vector<AreaSample> area_timeline;
};

// One ImGui frame: banner, controls, advanced 3D trajectory (GL renderer with a
// DrawList fallback), rolling stats, frequency-domain LF/HF, multi-subject grid,
// session history viewer, and events. Never renders a diagnosis or disease score.
//
// `grid` is owned by run_dashboard() (as a unique_ptr, destroyed before the GL
// context goes away). The frame feeds it three things every tick: the async DB
// batch mailbox (recorded subjects), the live session snapshot (smartwatch slot),
// and dt so playback advances.
//
// `force_grid_open` pins the grid section open regardless of the user's collapse
// state. Only the self-test uses it: the header is collapsed by default so the
// hero view is not taxed by 6-16 extra GL viewports nobody is looking at, which
// also means a normal run would never exercise render_grid().
void dashboard_frame(GLFWwindow* w,
                     const std::shared_ptr<SessionController>& session,
                     TrajectoryRenderer& renderer,
                     DatabaseManager& db,
                     Camera& cam,
                     std::uint64_t& hovered,
                     std::string& msg,
                     float dt_seconds,
                     ReplayView& replay,
                     MultiSubjectGrid& grid,
                     bool force_grid_open = false);
}  // namespace cardiac::ui
