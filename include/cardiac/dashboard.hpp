#pragma once

#include "cardiac/session_controller.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace cardiac::ui {

// Dear ImGui + GLFW + OpenGL3 desktop dashboard (Module D renderer).
// Runs visualization_snapshot() copies on the render thread (~30 FPS); heavy
// triangle areas stay on AsyncAnalyzer. Shows LIVE vs SYNTHETIC_TEST banner,
// state, gap-split 3D delay trajectory, area stats -- never diagnoses.
// Returns process exit code; non-zero when windowing/GL is unavailable.
struct DashboardOptions {
    std::string title{"Cardiac Dynamics Visualizer"};
    int width{1280};
    int height{800};
    // 0 = synthetic demo stream (labeled); 1 = live BLE HRS session.
    bool live_mode{false};
    std::string device_name_hint;
    std::size_t synthetic_beats{240};
    // When non-empty (and not live), the dashboard asks the DatabaseManager for
    // this stored session as soon as the DB is ready and replays it in the 3D
    // view. Default auto-loads the imported CHF2DB research record so
    // `dev.bat` opens straight into real data. Set empty to start on the
    // synthetic demo stream instead.
    std::string startup_session_id{"chf2db-chf207"};
    // >0 = run exactly this many frames, then print frame + multi-subject-grid
    // diagnostics and exit 0. This is the verification path for the grid: it
    // creates the real window and GL context, materializes a full 4x4 page of
    // cells, pages through the indexed cohort so slots rebind and the async
    // batch loader runs, and reports measured cost per cell. 0 = normal
    // interactive run.
    int selftest_frames{0};
};

int run_dashboard(DashboardOptions options,
                  std::shared_ptr<SessionController> external_session = nullptr);

} // namespace cardiac::ui
