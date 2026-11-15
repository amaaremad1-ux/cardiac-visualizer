#include "cardiac/dashboard.hpp"
#include "cardiac/database_manager.hpp"
#include "cardiac/grid_viewport.hpp"
#include "dashboard_draw.hpp"
#include "dashboard_frame.hpp"
#include "dashboard_session.hpp"
#include "trajectory_renderer.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
namespace cardiac::ui {
int run_dashboard(DashboardOptions options, std::shared_ptr<SessionController> ext) {
  if (!glfwInit()) {
    std::fprintf(stderr, "GLFW init failed\n");
    return 2;
  }
  // The advanced renderer needs a real OpenGL 3.3 core context (shaders + FBOs).
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  GLFWwindow* w =
      glfwCreateWindow(options.width, options.height, options.title.c_str(), nullptr, nullptr);
  if (!w) {
    glfwTerminate();
    return 2;
  }
  glfwMakeContextCurrent(w);
  glfwSwapInterval(1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(w, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");

  auto session = ext;
  std::shared_ptr<DeviceAdapter> ad;
  if (!session) build_dashboard_session(options, session, ad);

  // Advanced 3D renderer (GL). init() may fail without a 3.3 context/GPU; the
  // frame then falls back to the legacy DrawList trajectory - never a crash.
  auto renderer = std::make_unique<TrajectoryRenderer>();
  if (!renderer->init()) {
    std::fprintf(stderr, "TrajectoryRenderer init failed: %s\n", renderer->error().c_str());
  }

  // Asynchronous SQLite history store (worker thread; UI only polls mailboxes).
  DatabaseManager db;
  db.open_async("exports/cardiac_history.db");
  db.request_session_list_async();

  // Multi-subject grid. Owned here (not in the frame) so it can be destroyed
  // while the GL context is still current: every materialized slot holds five
  // FBOs plus a compiled shader pipeline. The grid never touches SQLite itself;
  // it publishes the subject ids it needs and the DB worker answers in bulk, so
  // paging a 500+ cohort never blocks the render thread.
  auto grid = std::make_unique<MultiSubjectGrid>();
  grid->set_batch_request_handler(
      [&db](const std::vector<std::string>& ids) { db.request_sessions_batch_async(ids); });
  // Self-test drives the worst case on purpose: the largest supported page (4x4 =
  // 16 GL cells) and continuous page turns, so slots rebind and the async batch
  // loader runs against the real 500+ cohort index.
  const bool selftest = options.selftest_frames > 0;
  if (selftest) grid->set_grid_size(4, 4);

  Camera cam;
  std::uint64_t hov = 0;
  std::string msg;
  ReplayView replay;
  // Auto-load a stored CHF2DB session on startup (never overrides a live run).
  if (!options.live_mode) replay.startup_session_id = options.startup_session_id;

  // Opt-in telemetry (CARDIAC_DASHBOARD_TELEMETRY=1): proves the render thread
  // keeps ticking at ~30 FPS while the async DB opens, lists history, and loads
  // the startup session -- i.e. no UI-thread freeze. Silent by default.
  const bool telemetry = selftest || [] {
    const char* e = std::getenv("CARDIAC_DASHBOARD_TELEMETRY");
    return e != nullptr && *e != '\0' && std::string(e) != "0";
  }();
  if (telemetry) {
    std::fprintf(stdout,
                 "[dashboard] startup: live=%d startup_session=\"%s\" "
                 "db=exports/cardiac_history.db\n",
                 options.live_mode ? 1 : 0, replay.startup_session_id.c_str());
    std::fflush(stdout);
  }
  long long frame = 0;
  bool selftest_failed = false;
  float worst_ms = 0.0f;
  float sum_ms = 0.0f;
  float sum_frame_ms = 0.0f;  // dashboard_frame(): ImGui build + render() issue
  float sum_swap_ms = 0.0f;   // ImGui::Render + SwapBuffers (vsync/GPU wait)
  auto prev = std::chrono::steady_clock::now();
  const auto run_start = prev;
  while (!glfwWindowShouldClose(w)) {
    auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - prev).count();
    prev = now;
    const float dt_ms = dt * 1000.0f;
    if (dt_ms > worst_ms) worst_ms = dt_ms;
    sum_ms += dt_ms;
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    auto tA = std::chrono::steady_clock::now();
    dashboard_frame(w, session, *renderer, db, cam, hov, msg, dt, replay, *grid, selftest);
    auto tB = std::chrono::steady_clock::now();
    ImGui::Render();
    int W = 0, H = 0;
    glfwGetFramebufferSize(w, &W, &H);
    glViewport(0, 0, W, H);
    glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(w);
    auto tC = std::chrono::steady_clock::now();
    sum_frame_ms += std::chrono::duration<float, std::milli>(tB - tA).count();
    sum_swap_ms += std::chrono::duration<float, std::milli>(tC - tB).count();
    // vsync (glfwSwapInterval(1)) paces the loop to the display refresh. The old
    // fixed 33 ms sleep overshot to ~47 ms because Windows' default timer
    // resolution is ~15.6 ms, capping us at ~21 FPS. Keep only a small floor so a
    // vsync-less context can't busy-spin the CPU/GPU.
    auto el = std::chrono::steady_clock::now() - now;
    if (el < std::chrono::milliseconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(2) - el);
    ++frame;
    if (telemetry && (frame % 30) == 0) {
      const float avg_ms = sum_ms / 30.0f;
      std::fprintf(stdout,
                   "[dashboard] frame %lld | avg %.1f ms (~%.1f FPS) | worst %.1f ms "
                   "| frame_cpu %.1f ms | swap %.1f ms\n",
                   frame, static_cast<double>(avg_ms),
                   avg_ms > 0.0f ? static_cast<double>(1000.0f / avg_ms) : 0.0,
                   static_cast<double>(worst_ms),
                   static_cast<double>(sum_frame_ms / 30.0f),
                   static_cast<double>(sum_swap_ms / 30.0f));
      std::fflush(stdout);
      worst_ms = 0.0f;
      sum_ms = 0.0f;
      sum_frame_ms = 0.0f;
      sum_swap_ms = 0.0f;
    }
    if (!selftest) continue;
    // Turn the page regularly so slot rebinding, the async SQLite batch loader and
    // the per-cell GPU re-window are all exercised - not just one static page.
    if ((frame % 45) == 0) grid->next_page();
    if (frame < options.selftest_frames) continue;

    const double wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - run_start)
            .count();
    const double fps = wall_ms > 0.0 ? 1000.0 * static_cast<double>(frame) / wall_ms : 0.0;
    const int gl_ready = grid->gl_ready_count();
    const float grid_ms = grid->last_grid_ms();
    // Points actually handed to the GPU this frame. This is the evidence that the
    // cells drew trajectories rather than empty frames: a cell with no resident
    // data contributes nothing here.
    std::size_t drawn_points = 0;
    int drawn_cells = 0;
    for (int i = 0; i < grid->subject_count(); ++i) {
      const GridCell* c = grid->cell_at(i);
      if (c == nullptr || !c->visible) continue;
      if (c->viz.points.empty()) continue;
      drawn_points += c->viz.points.size();
      ++drawn_cells;
    }
    std::fprintf(stdout,
                 "[selftest] ---------------- multi-subject grid ----------------\n"
                 "[selftest] frames rendered      : %lld in %.0f ms (%.1f FPS)\n"
                 "[selftest] cohort indexed       : %d subject(s)\n"
                 "[selftest] selection / pages    : %d subject(s) over %d page(s)\n"
                 "[selftest] grid layout          : %dx%d = %d cell slots per page\n"
                 "[selftest] cells GL-ready       : %d / %d materialized\n"
                 "[selftest] trajectories drawn   : %d cell(s), %zu point(s) on the GPU\n"
                 "[selftest] grid draw cost       : %.2f ms/frame CPU issue (%.1f FPS "
                 "headroom), worst cell %.2f ms\n"
                 "[selftest] db jobs still pending: %llu | subjects awaiting data: %d\n",
                 frame, wall_ms, fps, grid->cohort_size(), grid->selection_size(),
                 grid->page_count(), grid->grid_cols(), grid->grid_rows(),
                 grid->page_capacity(), gl_ready, grid->materialized_count(), drawn_cells,
                 drawn_points, static_cast<double>(grid_ms),
                 grid_ms > 0.01f ? static_cast<double>(1000.0f / grid_ms) : 0.0,
                 static_cast<double>(grid->worst_cell_ms()),
                 static_cast<unsigned long long>(db.pending_jobs()),
                 grid->pending_request_count());
    // The verdict: the grid must have actually put cells on the GPU and kept the
    // frame loop above 30 FPS while doing it. Anything less is a failure, not a
    // warning - this is the check that a green build does not provide.
    const bool ok = gl_ready > 0 && drawn_cells > 0 && drawn_points > 0 && fps >= 30.0;
    std::fprintf(stdout, "[selftest] verdict            : %s\n",
                 ok ? "PASS - grid rendered live at 30+ FPS"
                    : "FAIL - grid did not render, or dropped below 30 FPS");
    std::fflush(stdout);
    selftest_failed = !ok;
    break;
  }
  session->stop();
  session->disconnect();
  // Release the grid's GL slots first: the batch handler captures `db` by
  // reference, and the per-cell renderers need a current context to free their
  // FBOs/shaders. Both happen before the context is torn down below.
  grid.reset();
  db.close();
  // Destroy GL resources while the context is still current, before shutdown.
  renderer.reset();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(w);
  glfwTerminate();
  return selftest_failed ? 1 : 0;
}
}  // namespace cardiac::ui
