#include "dashboard_frame.hpp"
#include "cardiac/grid_ui.hpp"
#include "cardiac/grid_viewport.hpp"
#include "cardiac/hrv_spectrum.hpp"
#include "cardiac/ibi_csv_importer.hpp"
#include "cardiac/rolling_metrics.hpp"
#include "cardiac/session_exporter.hpp"
#include "dashboard_draw.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
namespace cardiac::ui {
namespace {

const ImVec4 kRed{0.90f, 0.32f, 0.30f, 1.0f};
const ImVec4 kGreen{0.35f, 0.80f, 0.45f, 1.0f};

// Opt-in startup telemetry (set CARDIAC_DASHBOARD_TELEMETRY=1). Silent by
// default so `dev.bat` runs stay clean; used to verify the async DB populates
// the history viewer and auto-loads the startup session without stalling UI.
bool telemetry_enabled() {
  static const bool on = [] {
    const char* e = std::getenv("CARDIAC_DASHBOARD_TELEMETRY");
    return e != nullptr && *e != '\0' && std::string(e) != "0";
  }();
  return on;
}

// Builds replay snapshots from a stored session so the advanced 3D renderer and
// every analytics panel display de-identified history/research data unchanged.
void load_replay(ReplayView& rv, const StoredSession& s) {
  rv.active = true;
  rv.label = s.record.session_id.empty() ? s.record.source : s.record.session_id;
  // Invalidate the cached analytics so the next frame recomputes them for THIS
  // record (loading a different session must never show stale stats).
  rv.analytics_valid = false;
  rv.rolling_area_win = -1;
  rv.rolling_qual_win = -1;
  rv.viz = VisualizationSnapshot{};
  rv.viz.state = ApplicationState::ANALYSIS_READY;
  rv.viz.mode = SessionMode::SyntheticTest;  // non-live; banner text overridden
  rv.viz.revision = s.analysis ? s.analysis->revision : 0u;
  rv.viz.points = s.points;
  rv.viz.analysis = s.analysis;
  rv.full = SessionSnapshot{};
  rv.full.session_id = s.record.session_id;
  rv.full.mode = SessionMode::SyntheticTest;
  rv.full.state = ApplicationState::ANALYSIS_READY;
  rv.full.measurements = s.measurements;
}

// Packs the live (or replayed) in-memory session into a StoredSession for the DB.
StoredSession pack_current(const SessionSnapshot& full, const VisualizationSnapshot& viz) {
  StoredSession s;
  const bool live = full.mode == SessionMode::Live;
  s.record.session_id = full.session_id;
  s.record.mode = live ? "LIVE" : "SYNTHETIC_TEST";
  s.record.source = live ? "live-ble-hrs" : "synthetic-demo";
  s.record.started_utc = format_utc(full.started_utc);
  s.record.saved_utc = format_utc(utc_now());
  s.record.final_state = to_string(full.state);
  std::size_t valid = 0;
  for (const auto& m : full.measurements)
    if (m.is_valid) ++valid;
  const RollingMetrics rm = compute_rolling_metrics(
      viz.points, viz.analysis, full.measurements,
      static_cast<std::size_t>(-1), static_cast<std::size_t>(-1));
  const HrvSpectrum hrv = compute_hrv_spectrum(hrv_input_from_measurements(full.measurements));
  s.record.measurement_count = full.measurements.size();
  s.record.valid_count = valid;
  s.record.trajectory_point_count = viz.points.size();
  s.record.area_mean_s2 = rm.area_mean_s2;
  s.record.ibi_mean_s = rm.ibi_mean_s;
  s.record.ibi_sd_s = rm.ibi_sd_s;
  s.record.lf_power_ms2 = hrv.reliable ? hrv.lf_power_ms2 : 0.0;
  s.record.hf_power_ms2 = hrv.reliable ? hrv.hf_power_ms2 : 0.0;
  s.record.lf_hf_ratio = hrv.reliable ? hrv.lf_hf_ratio : 0.0;
  s.record.disclaimer =
      live ? "Live geometric visualization only - no diagnosis or disease score."
           : "Synthetic demo data - geometric visualization only, not clinical.";
  s.points = viz.points;
  s.measurements = full.measurements;
  s.analysis = viz.analysis;
  return s;
}

// Small PSD polyline (power vs frequency) drawn into a reserved screen rect.
void draw_psd(ImDrawList* dl, ImVec2 o, ImVec2 s, const HrvSpectrum& h) {
  dl->AddRect(o, ImVec2(o.x + s.x, o.y + s.y), IM_COL32(90, 90, 110, 255));
  if (h.psd_freq_hz.size() < 2) return;
  float peak = 1e-6f;
  for (float p : h.psd_power) peak = std::max(peak, p);
  const float fmax = h.psd_freq_hz.back() > 1e-6f ? h.psd_freq_hz.back() : 1.0f;
  ImVec2 prev{-1.f, -1.f};
  for (std::size_t i = 0; i < h.psd_freq_hz.size() && i < h.psd_power.size(); ++i) {
    const float x = o.x + s.x * (h.psd_freq_hz[i] / fmax);
    const float y = o.y + s.y * (1.0f - (h.psd_power[i] / peak));
    if (prev.x >= 0.f) dl->AddLine(prev, ImVec2(x, y), IM_COL32(150, 180, 255, 255), 1.5f);
    prev = ImVec2(x, y);
  }
  // Shade the LF and HF bands for orientation.
  const auto band = [&](float lo, float hi, ImU32 col) {
    const float x0 = o.x + s.x * (lo / fmax), x1 = o.x + s.x * (hi / fmax);
    dl->AddRectFilled(ImVec2(x0, o.y), ImVec2(x1, o.y + s.y), col);
  };
  band(0.04f, 0.15f, IM_COL32(255, 200, 90, 26));   // LF
  band(0.15f, 0.40f, IM_COL32(120, 200, 255, 26));  // HF
}

}  // namespace

void dashboard_frame(GLFWwindow* w, const std::shared_ptr<SessionController>& session,
                     TrajectoryRenderer& renderer, DatabaseManager& db, Camera& cam,
                     std::uint64_t& hovered, std::string& msg, float dt_seconds,
                     ReplayView& replay, MultiSubjectGrid& grid, bool force_grid_open) {
  (void)w;
  // Persistent list of stored sessions for the history viewer (UI-thread only).
  static std::vector<SessionRecord> history;
  // Startup auto-load: once the async DB is ready, ask (exactly once) for the
  // configured default session so the dashboard opens straight into it.
  if (!replay.startup_done && db.ready() && !replay.startup_session_id.empty()) {
    replay.startup_done = true;
    db.request_session_async(replay.startup_session_id);
  }
  // Poll the async DB mailboxes (cheap flag checks; never blocks the UI thread).
  {
    std::vector<SessionRecord> listed;
    if (db.poll_session_list(listed)) {
      history = std::move(listed);
      static bool logged_history = false;
      if (telemetry_enabled() && !logged_history) {
        logged_history = true;
        std::fprintf(stdout,
                     "[dashboard] history ready: %zu session(s) listed by the async "
                     "worker; UI thread never blocked\n",
                     history.size());
        std::fflush(stdout);
      }
    }
    std::shared_ptr<StoredSession> loaded;
    // do_load returns an empty shell for a missing id; only replay a real one.
    if (db.poll_session(loaded) && loaded && !loaded->record.session_id.empty()) {
      load_replay(replay, *loaded);
      msg = "Loaded session " + loaded->record.session_id;
      static bool logged_load = false;
      if (telemetry_enabled() && !logged_load) {
        logged_load = true;
        std::fprintf(stdout,
                     "[dashboard] auto-loaded startup session: %s (%zu points, "
                     "%zu measurements)\n",
                     loaded->record.session_id.c_str(), loaded->points.size(),
                     loaded->measurements.size());
        std::fflush(stdout);
      }
    }
    // Multi-subject grid: recorded subjects arrive here in bulk. The grid asked
    // for these ids (see set_batch_request_handler in run_dashboard), the DB
    // worker ran the query off-thread, and we only move the rows into the slots.
    std::vector<StoredSession> batch;
    if (db.poll_sessions_batch(batch)) {
      const std::size_t n = batch.size();
      grid.attach_sessions(batch);
      static bool logged_batch = false;
      if (telemetry_enabled() && !logged_batch) {
        logged_batch = true;
        std::fprintf(stdout,
                     "[dashboard] grid batch ready: %zu subject(s) loaded by the async "
                     "worker; UI thread never blocked\n",
                     n);
        std::fflush(stdout);
      }
    }
    std::string saved_id;
    if (db.poll_saved(saved_id)) msg = "Saved session " + saved_id;
  }

  // Bind to the stored snapshots directly in replay mode: replay.viz/replay.full
  // already own the (~20k-beat CHF2DB) data, so copying them every frame is pure
  // waste. Only the live/synthetic path needs a fresh copy from the controller.
  VisualizationSnapshot viz_storage;
  SessionSnapshot full_storage;
  if (!replay.active) {
    viz_storage = session->visualization_snapshot();
    full_storage = session->snapshot();
  }
  const VisualizationSnapshot& viz = replay.active ? replay.viz : viz_storage;
  const SessionSnapshot& full = replay.active ? replay.full : full_storage;
  const bool live = !replay.active && viz.mode == SessionMode::Live;

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
  ImGui::Begin("Cardiac", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
  if (replay.active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.35f, 0.60f, 1.0f));
    ImGui::Button("REPLAY - STORED RESEARCH SESSION");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("%s", replay.label.c_str());
  } else if (live) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.45f, 0.15f, 1.0f));
    ImGui::Button("LIVE DEVICE DATA");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.05f, 1.0f));
    ImGui::Button("SYNTHETIC_TEST - DEMO ONLY");
    ImGui::PopStyleColor();
  }
  ImGui::SameLine();
  ImGui::Text("State: %s session=%s", to_string(viz.state),
              replay.active ? replay.label.c_str() : session->session_id().c_str());
  ImGui::Text("Geometric visualization only - no diagnosis or disease score.");
  if (replay.active) {
    ImGui::TextColored(kGreen, "De-identified stored data. Local SQLite is not a medical record store.");
  } else if (!live) {
    ImGui::Text("Demo stream is scripted; live stays idle without hardware.");
  } else if (viz.points.empty()) {
    ImGui::Text("Waiting for BLE Heart Rate Monitor...");
  }

  if (!replay.active) {
    if (ImGui::Button("Pause")) { try { session->pause(); } catch (...) {} }
    ImGui::SameLine();
    if (ImGui::Button("Resume")) { try { session->resume(); } catch (...) {} }
    ImGui::SameLine();
    if (ImGui::Button("Reconnect (break)")) { try { session->reconnect(); } catch (...) {} }
    ImGui::SameLine();
  } else {
    if (ImGui::Button("Return to live")) { replay.active = false; }
    ImGui::SameLine();
  }
  static char export_dir[260] = "exports";
  ImGui::InputText("Export dir", export_dir, sizeof(export_dir));
  ImGui::SameLine();
  if (ImGui::Button("Export CSV/JSON")) {
    try {
      SessionExporter ex;
      auto r = ex.export_session(full, viz.points, viz.analysis, export_dir);
      msg = "Exported " + r.csv_path.string() + " + " + r.json_path.string();
    } catch (const std::exception& e) {
      msg = std::string("Export failed: ") + e.what();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Reveal folder")) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", export_dir, nullptr, nullptr, SW_SHOWNORMAL);
#else
    msg = "Reveal supported on Windows only";
#endif
  }
  if (!replay.active) {
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { try { session->stop(); } catch (...) {} }
  }
  if (!msg.empty()) ImGui::Text("%s", msg.c_str());

  if (!renderer.ready()) {
    ImGui::SliderFloat("Yaw", &cam.yaw, -3.14f, 3.14f);
    ImGui::SameLine();
    ImGui::SliderFloat("Pitch", &cam.pitch, -1.4f, 1.4f);
    ImGui::SameLine();
    ImGui::SliderFloat("Zoom", &cam.scale, 0.4f, 2.5f);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 av = ImGui::GetContentRegionAvail();
  const float side = std::max(240.0f, std::min(av.x * 0.60f, av.y - 150.0f));
  const ImVec2 img_size(side + 16.0f, side + 16.0f);
  const ImVec2 cur0 = ImGui::GetCursorScreenPos();

  // --- Advanced 3D trajectory: GL renderer with a DrawList fallback ---
  if (renderer.ready()) {
    renderer.set_snapshot(viz);
    const ImVec2 fbs = ImGui::GetIO().DisplayFramebufferScale;
    const int rw = std::max(1, static_cast<int>(img_size.x * fbs.x));
    const int rh = std::max(1, static_cast<int>(img_size.y * fbs.y));
    if (rw != renderer.width() || rh != renderer.height()) renderer.resize(rw, rh);
    const unsigned int tex = renderer.render(dt_seconds);
    ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), img_size);
    if (ImGui::IsItemHovered()) {
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        renderer.camera().orbit(io.MouseDelta.x, io.MouseDelta.y);
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        renderer.camera().pan(io.MouseDelta.x, io.MouseDelta.y);
      if (io.MouseWheel != 0.0f) renderer.camera().zoom(io.MouseWheel * 120.0f);
    }
    hovered = viz.points.empty() ? 0u : viz.points.back().index;
    ImGui::Text("Drag: orbit | Right-drag: pan | Wheel: zoom   (%.1f ms, %zu pts, %zu segs)",
                renderer.last_frame_ms(), renderer.point_count(), renderer.segment_count());
    if (ImGui::Button("Reset view")) renderer.camera().reset();
  } else {
    ImGui::Dummy(img_size);
    const ImVec2 p0(cur0.x + 8.0f, cur0.y + 8.0f);
    draw_traj(dl, ImVec2(p0.x + side / 2.0f, p0.y + side / 2.0f), side, viz.points, cam, hovered);
    ImGui::TextColored(kRed, "GL 3.3 renderer unavailable (%s) - fallback line view.",
                       renderer.error().empty() ? "not initialized" : renderer.error().c_str());
  }

  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Text("Points: %zu rev: %llu", viz.points.size(), (unsigned long long)viz.revision);
  if (!viz.points.empty()) {
    auto& L = viz.points.back();
    ImGui::Text("Latest X=%.3f Y=%.3f Z=%.3f s", L.position.x, L.position.y, L.position.z);
    ImGui::Text("Hovered #%llu seg %llu", (unsigned long long)hovered,
                (unsigned long long)L.segment_id);
  }
  const AnalysisResult* an = viz.analysis ? &*viz.analysis : nullptr;
  if (an && !an->triangle_areas.empty()) {
    double s = 0.0, pk = 0.0;
    for (auto& a : an->triangle_areas) { s += a.area_s2; pk = std::max(pk, a.area_s2); }
    ImGui::Text("Areas n=%zu mean=%.6f peak=%.6f s^2", an->triangle_areas.size(),
                s / static_cast<double>(an->triangle_areas.size()), pk);
    ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(320, 100));
    draw_areas(dl, sp, ImVec2(320, 100), an);
  } else {
    ImGui::Text("Areas: pending (background worker)...");
  }

  static int area_win = 60, qual_win = 60;
  ImGui::SliderInt("Area window", &area_win, 5, 600);
  ImGui::SameLine();
  ImGui::SliderInt("Quality window", &qual_win, 5, 600);

  // Derived analytics. A stored replay never changes, so cache everything in the
  // ReplayView and recompute rolling metrics only when a window slider moves;
  // the live/synthetic stream recomputes each frame. Recomputing a Welch FFT +
  // percentile sort over ~20k beats every frame was the dominant per-frame cost.
  RollingMetrics rm_live;
  HrvSpectrum hrv_live;
  std::vector<QualitySample> qt_live;
  std::vector<AreaSample> at_live;
  if (replay.active) {
    const bool slider_moved =
        replay.rolling_area_win != area_win || replay.rolling_qual_win != qual_win;
    if (!replay.analytics_valid || slider_moved) {
      replay.rolling = compute_rolling_metrics(
          viz.points, viz.analysis, full.measurements,
          static_cast<std::size_t>(area_win), static_cast<std::size_t>(qual_win));
      replay.rolling_area_win = area_win;
      replay.rolling_qual_win = qual_win;
    }
    if (!replay.analytics_valid) {
      replay.quality_timeline = build_quality_timeline(full.measurements);
      replay.area_timeline = build_area_timeline(viz.points, viz.analysis);
      replay.hrv = compute_hrv_spectrum(hrv_input_from_measurements(full.measurements));
      replay.analytics_valid = true;
    }
  } else {
    rm_live = compute_rolling_metrics(
        viz.points, viz.analysis, full.measurements,
        static_cast<std::size_t>(area_win), static_cast<std::size_t>(qual_win));
    qt_live = build_quality_timeline(full.measurements);
    at_live = build_area_timeline(viz.points, viz.analysis);
    hrv_live = compute_hrv_spectrum(hrv_input_from_measurements(full.measurements));
  }
  const RollingMetrics& rm = replay.active ? replay.rolling : rm_live;
  const HrvSpectrum& hrv = replay.active ? replay.hrv : hrv_live;
  const std::vector<QualitySample>& qt = replay.active ? replay.quality_timeline : qt_live;
  const std::vector<AreaSample>& at = replay.active ? replay.area_timeline : at_live;
  ImGui::Text("Rolling areas: n=%zu mean=%.6f p50=%.6f peak=%.6f min=%.6f s^2",
              rm.window_areas, rm.area_mean_s2, rm.area_p50_s2, rm.area_peak_s2,
              rm.area_min_s2);
  ImGui::Text("IBI-X mean=%.4f sd=%.4f s | valid %.0f%% (last %zu) | gaps=%zu",
              rm.ibi_mean_s, rm.ibi_sd_s, rm.valid_ratio * 100.0,
              std::min<std::size_t>(static_cast<std::size_t>(qual_win),
                                    full.measurements.size()),
              rm.gap_count);
  const ImVec2 qp = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(560, 36));
  draw_quality_lane(dl, qp, ImVec2(560, 36), qt);
  ImGui::Text("Quality lane: green=valid red=rejected white tick=segment break");
  const ImVec2 ap = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(560, 100));
  draw_area_time(dl, ap, ImVec2(560, 100), at);
  ImGui::Text("Area timeline (s^2 vs elapsed s; breaks lift pen, never connect)");

  // --- Frequency-domain HRV (LF/HF): shown only when data quality allows ---
  ImGui::Text("Frequency domain (research descriptor - not clinical HRV):");
  if (hrv.reliable) {
    ImGui::TextColored(kGreen, "LF=%.1f ms^2  HF=%.1f ms^2  LF/HF=%.3f",
                       hrv.lf_power_ms2, hrv.hf_power_ms2, hrv.lf_hf_ratio);
    ImGui::Text("total=%.1f ms^2  windows=%zu  coverage=%.0fs  fs=%.1fHz",
                hrv.total_power_ms2, hrv.windows, hrv.coverage_s, hrv.sample_rate_hz);
    const ImVec2 hp = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(560, 90));
    draw_psd(dl, hp, ImVec2(560, 90), hrv);
    ImGui::Text("PSD (amber=LF band, blue=HF band) vs frequency Hz");
  } else {
    ImGui::TextColored(kRed, "LF/HF not shown: %s", hrv.reason.c_str());
  }

  ImGui::Text("Recent events:");
  std::size_t st = full.events.size() > 6 ? full.events.size() - 6 : 0;
  for (std::size_t i = st; i < full.events.size(); ++i)
    ImGui::BulletText("%s -> %s", to_string(full.events[i].code),
                      to_string(full.events[i].state));
  ImGui::EndGroup();

  // --- Session History Viewer (async SQLite3) ---
  if (ImGui::CollapsingHeader("Session History Viewer (SQLite3, asynchronous)")) {
    if (!db.ready()) {
      ImGui::Text("Opening database...");
      if (!db.last_error().empty())
        ImGui::TextColored(kRed, "DB error: %s", db.last_error().c_str());
    } else {
      ImGui::Text("DB: %s   pending jobs: %llu   sessions: %zu",
                  db.path().string().c_str(), (unsigned long long)db.pending_jobs(),
                  history.size());
      if (ImGui::Button("Refresh list")) db.request_session_list_async();
      ImGui::SameLine();
      if (ImGui::Button("Save current session")) {
        db.save_session_async(pack_current(full, viz));
        db.request_session_list_async();
      }
      ImGui::SameLine();
      static char import_dir[300] = "E:/CardiacVisualizer/data";
      ImGui::InputText("CHF2DB folder", import_dir, sizeof(import_dir));
      ImGui::SameLine();
      if (ImGui::Button("Import + store")) {
        int n = 0;
        std::error_code ec;
        std::filesystem::path dir(import_dir);
        if (std::filesystem::is_directory(dir, ec)) {
          for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            const auto p = e.path();
            if (p.extension() != ".csv") continue;
            if (p.filename().string().find("_IBI") == std::string::npos) continue;
            std::string err;
            auto imp = import_ibi_csv(p, {}, &err);
            if (imp) { db.save_session_async(std::move(imp->session)); ++n; }
            else msg = "Import failed: " + err;
          }
        } else {
          msg = std::string("Not a folder: ") + import_dir;
        }
        if (n > 0) msg = "Imported " + std::to_string(n) + " CHF2DB record(s)";
        db.request_session_list_async();
      }

      if (ImGui::BeginTable("history", 7,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0, 160))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Load");
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Beats");
        ImGui::TableSetupColumn("Points");
        ImGui::TableSetupColumn("IBI mean (s)");
        ImGui::TableSetupColumn("LF/HF");
        ImGui::TableHeadersRow();
        for (const auto& r : history) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::PushID(r.session_id.c_str());
          if (ImGui::SmallButton("Load")) db.request_session_async(r.session_id);
          ImGui::PopID();
          ImGui::TableNextColumn(); ImGui::Text("%s", r.session_id.c_str());
          ImGui::TableNextColumn(); ImGui::Text("%s", r.source.c_str());
          ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.measurement_count);
          ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.trajectory_point_count);
          ImGui::TableNextColumn(); ImGui::Text("%.3f", r.ibi_mean_s);
          ImGui::TableNextColumn();
          if (r.lf_hf_ratio > 0.0) ImGui::Text("%.3f", r.lf_hf_ratio);
          else ImGui::TextColored(kRed, "n/a");
        }
        ImGui::EndTable();
      }
    }
  }

  // --- Multi-Subject Grid (500+ cohort, bounded GL slots, async batch load) ---
  // Collapsed by default: expanding it materializes up to cols*rows GL cells and
  // starts paging the cohort, which is the one part of the dashboard with a real
  // per-frame GPU cost. The hero 3D view above is unaffected either way. The
  // self-test forces it open so render_grid() is genuinely exercised.
  //
  // Everything the grid needs per frame is driven from INSIDE this block. While
  // the panel is closed the grid does no work at all: it materializes no slots,
  // issues no SQLite batch requests, copies no live snapshots and uploads no GPU
  // buffers. Driving it unconditionally cost a ~125 ms hitch on the first batch
  // arrival for a view nobody was looking at.
  if (force_grid_open) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  if (ImGui::CollapsingHeader("Multi-Subject Grid (concurrent 3D trajectories)")) {
    // Materialize the slot pool, publish the ids still needed to the DB worker,
    // and move playback forward. Runs before render_grid so the live slot below
    // exists on the very first frame the panel is opened.
    grid.update_cells(dt_seconds);

    // --- Live smartwatch stream -> grid slot 0 -------------------------------
    // The grid reserves its first slot for real-time BLE heart-rate so the watch
    // is drawn alongside the recorded cohort. It reads straight from the
    // controller, not from `viz`/`full`: during a replay those bind the stored
    // session while the live stream keeps running underneath, and the grid must
    // keep showing the watch.
    if (grid.live_enabled()) {
      GridCell* lc = grid.cell_at(grid.live_slot_index());
      if (lc != nullptr) {
        // valid_count() is a cheap monotone counter behind the controller mutex.
        // Gating the snapshot copy on it keeps an idle stream at one integer read
        // per frame instead of copying thousands of points at 30 FPS. A cell that
        // was just rebound (points cleared) always re-feeds, so the slot never
        // sticks empty after toggling live off and on.
        const std::size_t stamp = session->valid_count();
        if (stamp != lc->live_feed_stamp || lc->full_points.empty()) {
          lc->live_feed_stamp = stamp;
          grid.update_cell_from_session(*lc, session->snapshot(),
                                        session->visualization_snapshot());
        }
      }
    }

    draw_grid_control_panel(w, grid, db, history, "Multi-Subject Viewport Grid", 0, 0, msg);
  }
  ImGui::End();
}
}  // namespace cardiac::ui
