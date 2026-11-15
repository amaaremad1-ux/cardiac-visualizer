// grid_ui.cpp - ImGui controls and integration for the MultiSubjectGrid.
//
// Draws the parameter panel described in the feature spec:
//   * IBI Subject Selector  - filter + pick single subjects or batches out of the
//                             indexed cohort (500+), or select an id range.
//   * Time Duration (s)     - the active visualization window per cell.
//   * Live Smartwatch Stream- reserves grid slot 0 for real-time BLE heart-rate
//                             trajectories drawn alongside recorded subjects.
// The subject list is virtualized with ImGuiListClipper and the grid pages a
// bounded pool of GL cells, so a 500+ cohort costs no frame time to browse.
#include "cardiac/grid_ui.hpp"

#include "cardiac/database_manager.hpp"
#include "cardiac/grid_viewport.hpp"
#include "cardiac/types.hpp"
#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cardiac::ui {

namespace {

const ImVec4 kRed{0.90f, 0.32f, 0.30f, 1.0f};
const ImVec4 kGreen{0.35f, 0.80f, 0.45f, 1.0f};
const ImVec4 kBlue{0.45f, 0.65f, 1.00f, 1.0f};

// ImGuiTextFilter::InputBuf is char[256]; there is no exported size constant.
constexpr std::size_t kFilterBufSize = 256;

// Stable identity for the cohort currently pushed into the grid, so set_cohort()
// (which resets selection + paging) is only called when the DB list really
// changed - never per frame.
std::uint64_t history_fingerprint(const std::vector<SessionRecord>& history) {
  // FNV-1a over the ids and the count: cheap, and good enough to detect the
  // importer adding rows while the dashboard is open.
  std::uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
  };
  const std::size_t count = history.size();
  mix(&count, sizeof(count));
  for (const auto& r : history) mix(r.session_id.data(), r.session_id.size());
  return h;
}

// Pushes the database session list into the grid's cohort index when it changes.
// Returns true if the cohort was (re)built this frame.
bool sync_cohort(MultiSubjectGrid& grid, const std::vector<SessionRecord>& history) {
  static std::uint64_t last_fp = 0;
  const std::uint64_t fp = history_fingerprint(history);
  if (fp == last_fp) return false;
  last_fp = fp;

  std::vector<SubjectRef> cohort;
  cohort.reserve(history.size());
  for (const auto& r : history) {
    if (r.session_id.empty()) continue;
    // Label carries the provenance so the grid cells are self-describing.
    std::string label = r.session_id;
    if (!r.source.empty() && r.source != r.session_id) label += "  (" + r.source + ")";
    cohort.push_back(SubjectRef{r.session_id, label});
  }
  grid.set_cohort(std::move(cohort));
  return true;
}

}  // namespace

void populate_subject_list(ImGuiTextFilter* filter,
                           const std::vector<SessionRecord>& history,
                           std::vector<std::string>& out_ids) {
  out_ids.clear();
  out_ids.reserve(history.size());
  for (const auto& rec : history) {
    if (rec.session_id.empty()) continue;
    if (filter == nullptr || filter->PassFilter(rec.session_id.c_str()))
      out_ids.push_back(rec.session_id);
  }
}

void draw_grid_control_panel(GLFWwindow* w, MultiSubjectGrid& grid, DatabaseManager& db,
                             const std::vector<SessionRecord>& history,
                             const std::string& title, int width, int height,
                             const std::string& msg) {
  (void)w;

  // ---- Header -------------------------------------------------------------
  ImGui::TextUnformatted(title.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("| %d cell slots | cohort %d | selected %d", grid.page_capacity(),
                      grid.cohort_size(), grid.selection_size());
  if (!msg.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kBlue, "%s", msg.c_str());
  }

  const bool cohort_rebuilt = sync_cohort(grid, history);
  if (cohort_rebuilt && !db.ready()) {
    ImGui::SameLine();
    ImGui::TextColored(kRed, "(db opening)");
  }

  if (!ImGui::CollapsingHeader("Multi-Subject Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
    // Controls hidden: still draw the grid so the viewports stay live.
    ImGui::Separator();
    grid.render_grid(nullptr);
    return;
  }

  // ---- IBI Subject Selector ----------------------------------------------
  static ImGuiTextFilter subject_filter;
  static char filter_copy[kFilterBufSize] = "";
  static int batch_size = 6;
  static char range_start[128] = "";
  static int range_count = 12;

  ImGui::TextUnformatted("IBI Subject Selector");
  ImGui::SetNextItemWidth(260.0f);
  subject_filter.Draw("Filter##grid_subject");
  ImGui::SameLine();
  ImGui::TextDisabled("%d / %d match", grid.selection_size(), grid.cohort_size());

  // Re-apply the filter only when its text actually changed (set_subject_filter
  // rebuilds the whole selection, so doing it every frame would be waste).
  if (std::strncmp(filter_copy, subject_filter.InputBuf, kFilterBufSize - 1) != 0) {
    std::strncpy(filter_copy, subject_filter.InputBuf, kFilterBufSize - 1);
    filter_copy[kFilterBufSize - 1] = '\0';
    if (filter_copy[0] == '\0') {
      grid.clear_filter();
    } else {
      ImGuiTextFilter* f = &subject_filter;  // static: outlives the grid
      grid.set_subject_filter([f](const std::string& id) {
        return f->PassFilter(id.c_str());
      });
    }
  }

  // Candidate list (virtualized - safe at 500+ entries).
  std::vector<std::string> matches;
  populate_subject_list(&subject_filter, history, matches);

  if (ImGui::BeginChild("grid_subject_list", ImVec2(-1.0f, 130.0f),
                        ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened)) {
    if (matches.empty()) {
      ImGui::TextDisabled(grid.cohort_size() == 0
                              ? "No subjects indexed yet. Use 'Import + store' in the "
                                "Session History Viewer, or run the importer."
                              : "No subject matches this filter.");
    } else {
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(matches.size()));
      while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
          const std::string& id = matches[static_cast<std::size_t>(row)];
          ImGui::PushID(row);
          if (ImGui::SmallButton("+")) {
            // Add this one subject to the grid (pages to it automatically).
            VisualizationSnapshot empty{};
            grid.add_subject(id, id, empty, SessionMode::SyntheticTest);
          }
          ImGui::PopID();
          ImGui::SameLine();
          const bool in_grid = grid.find_cell(id) != nullptr;
          if (in_grid) ImGui::TextColored(kGreen, "%s", id.c_str());
          else ImGui::TextUnformatted(id.c_str());
        }
      }
    }
  }
  ImGui::EndChild();

  // Batch selection: fill the visible page(s) from the current selection.
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Batch:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  ImGui::InputInt("##batch_size", &batch_size, 1, 4);
  batch_size = std::clamp(batch_size, 1, 64);
  ImGui::SameLine();
  if (ImGui::Button("Add batch")) {
    // Add up to batch_size subjects starting at the current page.
    const int start = grid.first_selection_index();
    int added = 0;
    for (int i = 0; i < batch_size && added < batch_size; ++i) {
      const int sel = start + i;
      const SubjectRef* ref = grid.selection_at(sel);
      if (ref == nullptr) break;
      VisualizationSnapshot empty{};
      grid.add_subject(ref->id, ref->label, empty, SessionMode::SyntheticTest);
      ++added;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Fill page")) {
    // Jump so the current page is fully populated from the selection.
    const int start = grid.first_selection_index();
    for (int i = 0; i < grid.page_capacity(); ++i) {
      const SubjectRef* ref = grid.selection_at(start + i);
      if (ref == nullptr) break;
      VisualizationSnapshot empty{};
      grid.add_subject(ref->id, ref->label, empty, SessionMode::SyntheticTest);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear view")) {
    // Empty the selection without dropping anything from the indexed cohort:
    // a zero-length range selects nothing, and 'All indexed' restores it.
    grid.set_subject_range(std::string(), 0u);
  }

  // Range selection over the indexed cohort (e.g. subjects 100..199).
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Range:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0f);
  ImGui::InputTextWithHint("##range_start", "start subject id", range_start, sizeof(range_start));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  ImGui::InputInt("##range_count", &range_count, 4, 16);
  range_count = std::clamp(range_count, 1, 1024);
  ImGui::SameLine();
  if (ImGui::Button("Apply range")) {
    grid.set_subject_range(std::string(range_start),
                           static_cast<std::uint64_t>(std::max(1, range_count)));
  }
  ImGui::SameLine();
  if (ImGui::Button("All indexed")) grid.clear_filter();

  // ---- Time Duration (seconds) -------------------------------------------
  ImGui::Separator();
  ImGui::TextUnformatted("Time Duration (seconds)");
  static float duration = grid.time_duration();
  duration = std::clamp(duration, 1.0f, 600.0f);
  ImGui::SetNextItemWidth(320.0f);
  if (ImGui::SliderFloat("##duration_slider", &duration, 1.0f, 600.0f, "%.1f s",
                         ImGuiSliderFlags_AlwaysClamp)) {
    grid.set_time_duration(duration);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  if (ImGui::InputFloat("s##duration", &duration, 1.0f, 10.0f, "%.1f")) {
    duration = std::clamp(duration, 1.0f, 600.0f);
    grid.set_time_duration(duration);
  }
  ImGui::SameLine();
  static float speed = 1.0f;
  ImGui::SetNextItemWidth(110.0f);
  if (ImGui::SliderFloat("##speed", &speed, 0.0f, 8.0f, "x%.2f")) {
    grid.set_playback_speed(speed);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("window %.1f s, playback x%.2f", static_cast<double>(grid.time_duration()),
                      static_cast<double>(grid.playback_speed()));
  if (ImGui::SmallButton("0x (freeze)")) {
    speed = 0.0f;
    grid.set_playback_speed(0.0f);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("1x")) {
    speed = 1.0f;
    grid.set_playback_speed(1.0f);
  }

  // ---- Live Smartwatch Stream --------------------------------------------
  ImGui::Separator();
  ImGui::TextUnformatted("Live Smartwatch Stream (BLE Heart Rate)");
  static bool live = false;
  if (ImGui::Checkbox("Enable live BLE stream in slot 1", &live)) {
    grid.set_live_enabled(live);
  }
  ImGui::SameLine();
  const int live_slot = grid.live_slot_index();
  if (live_slot >= 0) {
    const GridCell* lc = grid.cell_at(live_slot);
    if (lc != nullptr && lc->data_loaded && !lc->full_points.empty()) {
      ImGui::TextColored(kGreen, "streaming: %zu beats, %.1f s covered",
                         lc->full_points.size(), lc->span_s);
    } else {
      ImGui::TextColored(kRed, "idle - waiting for BLE heart-rate notifications");
    }
  } else {
    ImGui::TextDisabled("off - grid shows recorded subjects only");
  }
  ImGui::TextDisabled("Live draws real-time RR intervals from the watch alongside the "
                      "database records. No hardware = stays idle, never synthesized.");

  // ---- Layout + paging ----------------------------------------------------
  ImGui::Separator();
  // Seed from the grid's real state, and only push a size back when the user
  // actually moves a slider. Calling set_grid_size() unconditionally every frame
  // would overwrite any layout set programmatically (the self-test's 4x4 page)
  // and re-clamp paging 60 times a second for no reason.
  static int cols = grid.grid_cols();
  static int rows = grid.grid_rows();
  ImGui::SetNextItemWidth(110.0f);
  bool layout_changed = ImGui::SliderInt("Cols##grid", &cols, 1, 4);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  const bool rows_changed = ImGui::SliderInt("Rows##grid", &rows, 1, 4);
  layout_changed = layout_changed || rows_changed;
  if (layout_changed) grid.set_grid_size(cols, rows);
  ImGui::SameLine();
  static int point_budget = grid.max_points_per_cell();
  ImGui::SetNextItemWidth(130.0f);
  if (ImGui::SliderInt("Pts/cell", &point_budget, 256, 20000)) {
    grid.set_max_points_per_cell(point_budget);
  }

  ImGui::AlignTextToFramePadding();
  if (ImGui::Button("< Page")) grid.prev_page();
  ImGui::SameLine();
  ImGui::Text("page %d / %d", grid.page() + 1, grid.page_count());
  ImGui::SameLine();
  if (ImGui::Button("Page >")) grid.next_page();
  ImGui::SameLine();
  if (ImGui::Button("Reset views")) grid.fit_all_cells();

  // ---- Diagnostics --------------------------------------------------------
  const float grid_ms = grid.last_grid_ms();
  const float fps = grid_ms > 0.01f ? 1000.0f / grid_ms : 0.0f;
  ImGui::TextDisabled(
      "cells: %d materialized, %d GL-ready | grid %.1f ms (%.0f FPS draw), worst cell "
      "%.1f ms | db pending jobs %llu, awaiting %d subject(s)",
      grid.subject_count(), grid.gl_ready_count(), static_cast<double>(grid_ms),
      static_cast<double>(fps), static_cast<double>(grid.worst_cell_ms()),
      static_cast<unsigned long long>(db.pending_jobs()), grid.pending_request_count());
  if (!db.ready() && !db.last_error().empty())
    ImGui::TextColored(kRed, "DB error: %s", db.last_error().c_str());

  // ---- The viewports themselves ------------------------------------------
  ImGui::Separator();
  ImGui::TextUnformatted("3D Trajectories (drag: orbit | right-drag: pan | wheel: zoom | "
                         "double-click: reset)");
  if (grid.selection_size() == 0 && grid.subject_count() == 0) {
    ImGui::TextDisabled("No subjects selected. Pick from the list above or load the cohort "
                        "with 'Import + store' in the Session History Viewer.");
  }
  // Reserve the remaining window height for the grid so cells size to it.
  const float avail_h = ImGui::GetContentRegionAvail().y;
  const float want_h = (height > 0) ? static_cast<float>(height) * 0.5f : avail_h;
  ImGui::BeginChild("grid_viewports", ImVec2(static_cast<float>(width > 0 ? width : 0),
                                              std::max(160.0f, std::min(avail_h, want_h))),
                    ImGuiChildFlags_None);
  grid.render_grid(nullptr);
  ImGui::EndChild();
}

}  // namespace cardiac::ui
