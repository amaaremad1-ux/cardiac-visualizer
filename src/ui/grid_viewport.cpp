// grid_viewport.cpp - Multi-subject 3D trajectory grid viewport implementation.
//
// Draws a bounded pool of GL-backed cells that page through a 500+ subject
// cohort. See include/cardiac/grid_viewport.hpp for the three-tier model
// (cohort -> selection -> materialized cells) and the thread contract: this
// file never calls SQLite, it only publishes the subject ids it needs.
#include "cardiac/grid_viewport.hpp"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace cardiac::ui {

namespace {

// Hard caps that keep the grid inside its GPU budget. Each materialized slot
// owns five FBOs plus a shader pipeline, so the page size is the real cost
// driver - not the cohort size.
constexpr int kMinGridDim = 1;
constexpr int kMaxGridDim = 4;

// Playback rebuild quantization. Continuous playback re-windows the trajectory,
// but the GPU upload only happens when the window has slid this far. At 0.4 s
// that is at most ~2.5 rebuilds/sec/cell instead of 60.
constexpr double kWindowQuantizeS = 0.4;

// Grid cells are small; a lighter particle/bloom budget per cell is what lets
// many cells stay above 30 FPS. The single-subject hero view keeps its own
// heavier settings.
constexpr int kGridParticleCount = 500;
constexpr float kGridBloom = 0.65f;
constexpr float kGridGlow = 0.9f;

constexpr char kLiveSubjectId[] = "__live_smartwatch__";

double point_elapsed_s(const std::vector<TrajectoryPoint>& pts, std::size_t i, UtcTime t0) {
  return std::chrono::duration<double>(pts[i].timestamp_utc - t0).count();
}

// First index whose elapsed time is >= t. Points are stored in time order.
std::size_t lower_bound_time(const std::vector<TrajectoryPoint>& pts, UtcTime t0, double t) {
  std::size_t lo = 0;
  std::size_t hi = pts.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (point_elapsed_s(pts, mid, t0) < t) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

int clamp_dim(int v) { return std::clamp(v, kMinGridDim, kMaxGridDim); }

}  // namespace

MultiSubjectGrid::MultiSubjectGrid() = default;

// Slots own GL resources; destroying them here releases the FBOs. The owner must
// destroy the grid while the GL context is still current (dashboard_app does
// this before ImGui/GLFW shutdown).
MultiSubjectGrid::~MultiSubjectGrid() = default;

// ------------------------------------------------------------------ cohort --

void MultiSubjectGrid::set_cohort(std::vector<SubjectRef> subjects) {
  cohort_ = std::move(subjects);
  range_active_ = false;
  range_start_ = 0;
  range_count_ = 0;
  rebuild_selection();
}

const SubjectRef* MultiSubjectGrid::cohort_at(int index) const {
  if (index < 0 || index >= static_cast<int>(cohort_.size())) return nullptr;
  return &cohort_[static_cast<std::size_t>(index)];
}

const SubjectRef* MultiSubjectGrid::selection_at(int index) const {
  if (index < 0 || index >= static_cast<int>(selection_.size())) return nullptr;
  return &selection_[static_cast<std::size_t>(index)];
}

void MultiSubjectGrid::set_subject_list(const std::vector<std::string>& subject_ids) {
  cohort_.clear();
  cohort_.reserve(subject_ids.size());
  for (const auto& id : subject_ids) cohort_.push_back(SubjectRef{id, id});
  range_active_ = false;
  range_start_ = 0;
  range_count_ = 0;
  rebuild_selection();
}

void MultiSubjectGrid::set_subject_filter(
    const std::function<bool(const std::string&)>& filter) {
  filter_ = filter;
  rebuild_selection();
}

void MultiSubjectGrid::clear_filter() {
  filter_ = nullptr;
  range_active_ = false;
  range_start_ = 0;
  range_count_ = 0;
  rebuild_selection();
}

void MultiSubjectGrid::set_subject_range(const std::string& start_id, std::uint64_t count) {
  int start = 0;
  for (std::size_t i = 0; i < cohort_.size(); ++i) {
    if (cohort_[i].id == start_id) {
      start = static_cast<int>(i);
      break;
    }
  }
  range_active_ = true;
  range_start_ = start;
  range_count_ = count;
  rebuild_selection();
}

void MultiSubjectGrid::rebuild_selection() {
  selection_.clear();
  std::size_t begin = 0;
  std::size_t end = cohort_.size();
  if (range_active_) {
    begin = static_cast<std::size_t>(std::max(0, range_start_));
    if (begin > cohort_.size()) begin = cohort_.size();
    std::size_t want = begin + static_cast<std::size_t>(range_count_);
    end = std::min(cohort_.size(), want);
  }
  selection_.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    if (filter_ && !filter_(cohort_[i].id)) continue;
    selection_.push_back(cohort_[i]);
  }
  page_ = 0;
}

// ------------------------------------------------------------------ paging --

int MultiSubjectGrid::page_capacity() const {
  return std::max(1, grid_cols_ * grid_rows_);
}

int MultiSubjectGrid::page_count() const {
  // Slot 0 is reserved for the live smartwatch stream when live is enabled, so
  // the cohort pages through one fewer slot in that mode.
  const int live_slots = live_enabled_ ? 1 : 0;
  const int per_page = std::max(1, page_capacity() - live_slots);
  if (selection_.empty()) return 1;
  const int n = static_cast<int>(selection_.size());
  return std::max(1, (n + per_page - 1) / per_page);
}

int MultiSubjectGrid::first_selection_index() const {
  const int live_slots = live_enabled_ ? 1 : 0;
  const int per_page = std::max(1, page_capacity() - live_slots);
  return page_ * per_page;
}

void MultiSubjectGrid::set_page(int page) {
  page_ = std::clamp(page, 0, std::max(0, page_count() - 1));
}

void MultiSubjectGrid::next_page() { set_page(page_ + 1); }
void MultiSubjectGrid::prev_page() { set_page(page_ - 1); }

void MultiSubjectGrid::set_grid_size(int cols, int rows) {
  const int c = clamp_dim(cols);
  const int r = clamp_dim(rows);
  if (c == grid_cols_ && r == grid_rows_) return;
  grid_cols_ = c;
  grid_rows_ = r;
  set_page(page_);  // re-clamp: capacity changed
}

// ------------------------------------------------------------------- slots --

void MultiSubjectGrid::rebind_slot(GridCell& cell, const SubjectRef& ref) {
  cell.subject_id = ref.id;
  cell.label = ref.label.empty() ? ref.id : ref.label;
  cell.mode = SessionMode::SyntheticTest;
  cell.live_enabled = false;
  cell.playback_time = 0.0f;
  cell.time_duration = time_duration_;
  cell.full_points.clear();
  cell.full_points.shrink_to_fit();
  cell.span_s = 0.0;
  cell.data_loaded = false;
  cell.data_requested = false;
  cell.viz = VisualizationSnapshot{};
  cell.viz.mode = cell.mode;
  cell.window_start_s = -1.0;
  cell.window_duration_s = -1.0f;
  cell.window_stamp = 0;
  // Keep the compiled GL pipeline: rebinding must never re-create shaders.
  cell.renderer.camera().reset();
  cell.camera.reset();
}

void MultiSubjectGrid::sync_slots() {
  const int total = page_capacity();
  const int live_slots = live_enabled_ ? 1 : 0;
  const int cohort_slots = std::max(0, total - live_slots);

  // Grow/shrink the slot pool. Slots are heap-owned so the non-movable
  // TrajectoryRenderer inside each one stays put (and keeps its GL handles).
  while (static_cast<int>(cells_.size()) < total) {
    auto c = std::make_unique<GridCell>();
    c->cell_index = static_cast<int>(cells_.size());
    cells_.push_back(std::move(c));
  }
  while (static_cast<int>(cells_.size()) > total) cells_.pop_back();

  const int first = first_selection_index();
  materialized_ = 0;
  for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
    GridCell& c = *cells_[static_cast<std::size_t>(i)];
    c.cell_index = i;
    c.time_duration = time_duration_;

    if (i < live_slots) {
      // Slot 0 is the live smartwatch stream; the dashboard feeds it directly.
      if (c.subject_id != kLiveSubjectId) {
        c.subject_id = kLiveSubjectId;
        c.label = "LIVE - smartwatch";
        c.mode = SessionMode::Live;
        c.live_enabled = true;
        c.playback_time = 0.0f;
        c.full_points.clear();
        c.span_s = 0.0;
        c.data_loaded = false;
        c.data_requested = true;  // never ask SQLite for the live subject
        c.viz = VisualizationSnapshot{};
        c.viz.mode = SessionMode::Live;
        c.renderer.camera().reset();
      }
      c.visible = true;
      ++materialized_;
      continue;
    }

    const int sel = first + (i - live_slots);
    if (sel < 0 || sel >= static_cast<int>(selection_.size()) || cohort_slots <= 0) {
      // Empty tail slot on the last page: keep it drawn but blank.
      if (!c.subject_id.empty()) {
        c.subject_id.clear();
        c.label.clear();
        c.full_points.clear();
        c.data_loaded = false;
        c.data_requested = true;
        c.viz = VisualizationSnapshot{};
      }
      c.visible = false;
      continue;
    }

    const SubjectRef& ref = selection_[static_cast<std::size_t>(sel)];
    if (c.subject_id != ref.id) rebind_slot(c, ref);
    c.visible = true;
    ++materialized_;
  }
}

int MultiSubjectGrid::add_subject(const std::string& subject_id, const std::string& label,
                                  const VisualizationSnapshot& viz, SessionMode mode) {
  if (subject_id.empty()) return -1;
  // Register in the cohort so the selector/pager can see it, then page to it and
  // hand the snapshot straight to the slot (no SQLite round-trip needed).
  bool known = false;
  for (const auto& s : cohort_) {
    if (s.id == subject_id) {
      known = true;
      break;
    }
  }
  if (!known) cohort_.push_back(SubjectRef{subject_id, label.empty() ? subject_id : label});
  rebuild_selection();

  int sel = -1;
  for (std::size_t i = 0; i < selection_.size(); ++i) {
    if (selection_[i].id == subject_id) {
      sel = static_cast<int>(i);
      break;
    }
  }
  if (sel < 0) return -1;

  const int live_slots = live_enabled_ ? 1 : 0;
  const int per_page = std::max(1, page_capacity() - live_slots);
  set_page(sel / per_page);
  sync_slots();

  const int slot = live_slots + (sel - first_selection_index());
  if (slot < 0 || slot >= static_cast<int>(cells_.size())) return -1;
  GridCell& c = *cells_[static_cast<std::size_t>(slot)];
  rebind_slot(c, SubjectRef{subject_id, label.empty() ? subject_id : label});
  c.mode = mode;
  c.live_enabled = (mode == SessionMode::Live);
  c.full_points = viz.points;
  if (!c.full_points.empty()) {
    c.span_s = point_elapsed_s(c.full_points, c.full_points.size() - 1,
                               c.full_points.front().timestamp_utc);
    if (c.span_s < 0.0) c.span_s = 0.0;
  }
  c.data_loaded = true;
  c.data_requested = true;
  rebuild_window(c, true);
  return slot;
}

void MultiSubjectGrid::remove_subject(int cell_index) {
  if (cell_index < 0 || cell_index >= static_cast<int>(cells_.size())) return;
  const std::string id = cells_[static_cast<std::size_t>(cell_index)]->subject_id;
  if (!id.empty()) remove_subject(id);
}

void MultiSubjectGrid::remove_subject(const std::string& subject_id) {
  if (subject_id.empty() || subject_id == kLiveSubjectId) return;
  cohort_.erase(std::remove_if(cohort_.begin(), cohort_.end(),
                               [&](const SubjectRef& s) { return s.id == subject_id; }),
                cohort_.end());
  rebuild_selection();
}

GridCell* MultiSubjectGrid::cell_at(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) return nullptr;
  return cells_[static_cast<std::size_t>(index)].get();
}

const GridCell* MultiSubjectGrid::cell_at(int index) const {
  if (index < 0 || index >= static_cast<int>(cells_.size())) return nullptr;
  return cells_[static_cast<std::size_t>(index)].get();
}

GridCell* MultiSubjectGrid::find_cell(const std::string& subject_id) {
  for (auto& up : cells_) {
    if (up && up->subject_id == subject_id) return up.get();
  }
  return nullptr;
}

// ------------------------------------------------------- async data plumbing --

void MultiSubjectGrid::request_missing_data() {
  pending_requests_ = 0;
  std::vector<std::string> need;
  for (auto& up : cells_) {
    GridCell& c = *up;
    if (!c.visible || c.data_loaded) continue;
    if (c.subject_id.empty() || c.subject_id == kLiveSubjectId) continue;
    if (!c.data_requested) {
      c.data_requested = true;
      need.push_back(c.subject_id);
    } else {
      ++pending_requests_;
    }
  }
  if (!need.empty() && batch_request_) {
    pending_requests_ += static_cast<int>(need.size());
    batch_request_(need);
  }
}

void MultiSubjectGrid::attach_sessions(const std::vector<StoredSession>& sessions) {
  for (const auto& s : sessions) {
    GridCell* c = find_cell(s.record.session_id);
    if (!c) continue;  // page moved on before the worker answered; ignore.
    c->full_points = s.points;
    if (!c->full_points.empty()) {
      c->span_s = point_elapsed_s(c->full_points, c->full_points.size() - 1,
                                  c->full_points.front().timestamp_utc);
      if (c->span_s < 0.0) c->span_s = 0.0;
    } else {
      c->span_s = 0.0;
    }
    c->mode = (s.record.mode == "LIVE") ? SessionMode::Live : SessionMode::SyntheticTest;
    c->data_loaded = true;
    c->data_requested = true;
    rebuild_window(*c, true);
  }
}

void MultiSubjectGrid::set_active_subject_data(const std::string& subject_id,
                                               const VisualizationSnapshot& viz) {
  GridCell* c = find_cell(subject_id);
  if (!c) return;
  c->full_points = viz.points;
  if (!c->full_points.empty()) {
    c->span_s = point_elapsed_s(c->full_points, c->full_points.size() - 1,
                                c->full_points.front().timestamp_utc);
    if (c->span_s < 0.0) c->span_s = 0.0;
  }
  c->mode = viz.mode;
  c->data_loaded = true;
  c->data_requested = true;
  rebuild_window(*c, true);
}

void MultiSubjectGrid::update_cell_from_session(GridCell& cell, const SessionSnapshot& snap,
                                                const VisualizationSnapshot& viz) {
  cell.mode = snap.mode;
  cell.live_enabled = (snap.mode == SessionMode::Live);
  cell.full_points = viz.points;
  if (!cell.full_points.empty()) {
    cell.span_s = point_elapsed_s(cell.full_points, cell.full_points.size() - 1,
                                  cell.full_points.front().timestamp_utc);
    if (cell.span_s < 0.0) cell.span_s = 0.0;
  } else {
    cell.span_s = 0.0;
  }
  cell.viz.state = viz.state;
  cell.data_loaded = !cell.full_points.empty();
  cell.data_requested = true;
  // A live cell always trails the newest beat, so pin playback to the tail and
  // force a re-window every time the stream grows.
  const double tail = std::max(0.0, cell.span_s - static_cast<double>(cell.time_duration));
  cell.playback_time = static_cast<float>(tail);
  rebuild_window(cell, true);
}

// -------------------------------------------------------------- time window --

void MultiSubjectGrid::rebuild_window(GridCell& cell, bool force) {
  const float dur = std::max(0.1f, cell.time_duration);

  if (!force && cell.window_stamp != 0 && dur == cell.window_duration_s &&
      std::fabs(static_cast<double>(cell.playback_time) - cell.window_start_s) <
          kWindowQuantizeS) {
    return;
  }

  cell.window_start_s = static_cast<double>(cell.playback_time);
  cell.window_duration_s = dur;
  cell.viz.mode = cell.mode;
  cell.viz.state = cell.full_points.empty() ? ApplicationState::COLLECTING
                                            : ApplicationState::ANALYSIS_READY;
  cell.viz.points.clear();

  if (cell.full_points.empty()) {
    cell.viz.revision = ++revision_counter_;
    cell.window_stamp = cell.viz.revision;
    return;
  }

  const UtcTime t0 = cell.full_points.front().timestamp_utc;
  const double start = std::max(0.0, static_cast<double>(cell.playback_time));
  const double end = start + static_cast<double>(dur);
  const std::size_t i0 = lower_bound_time(cell.full_points, t0, start);
  std::size_t i1 = lower_bound_time(cell.full_points, t0, end);
  if (i1 <= i0) i1 = std::min(cell.full_points.size(), i0 + 1);

  const std::size_t count = i1 - i0;
  const std::size_t budget =
      static_cast<std::size_t>(std::max(64, max_points_per_cell_));
  const std::size_t step = count > budget ? (count + budget - 1) / budget : 1;
  // Never let decimation connect across a quality break: the first point of
  // every segment is always kept so gaps stay visually split.
  const std::size_t hard_cap = budget * 2;

  cell.viz.points.reserve(std::min(count, hard_cap));
  std::uint64_t last_seg = std::numeric_limits<std::uint64_t>::max();
  std::size_t emitted = 0;
  for (std::size_t i = i0; i < i1 && emitted < hard_cap; ++i) {
    const TrajectoryPoint& p = cell.full_points[i];
    const bool seg_changed = (p.segment_id != last_seg);
    const bool on_stride = ((i - i0) % step) == 0;
    const bool is_last = (i + 1 == i1);
    if (!seg_changed && !on_stride && !is_last) continue;
    cell.viz.points.push_back(p);
    last_seg = p.segment_id;
    ++emitted;
  }

  cell.viz.revision = ++revision_counter_;
  cell.window_stamp = cell.viz.revision;
}

// ---------------------------------------------------------------- playback --

void MultiSubjectGrid::advance_playback(float dt) {
  const double step = static_cast<double>(dt) * static_cast<double>(playback_speed_);
  for (auto& up : cells_) {
    GridCell& c = *up;
    if (!c.visible) continue;
    const double dur = static_cast<double>(std::max(0.1f, c.time_duration));
    if (c.mode == SessionMode::Live) {
      // Live always shows the most recent `duration` seconds.
      c.playback_time = static_cast<float>(std::max(0.0, c.span_s - dur));
      continue;
    }
    const double loop_span = std::max(0.0, c.span_s - dur);
    if (loop_span <= 0.0) {
      c.playback_time = 0.0f;
      continue;
    }
    double t = static_cast<double>(c.playback_time) + step;
    if (t > loop_span) t = std::fmod(t, loop_span);
    if (t < 0.0) t = 0.0;
    c.playback_time = static_cast<float>(t);
  }
}

void MultiSubjectGrid::update_cells(float dt) {
  last_dt_ = (dt > 0.0f && dt < 0.25f) ? dt : (1.0f / 60.0f);
  sync_slots();
  for (auto& up : cells_) up->time_duration = time_duration_;
  request_missing_data();
  advance_playback(last_dt_);
}

// ---------------------------------------------------------------------- GL --

bool MultiSubjectGrid::ensure_gl(GridCell& cell) {
  if (cell.gl_ready) return true;
  if (cell.gl_attempted) return false;
  cell.gl_attempted = true;
  if (!cell.renderer.init()) return false;
  cell.renderer.set_particle_count(kGridParticleCount);
  cell.renderer.set_bloom(kGridBloom);
  cell.renderer.set_glow(kGridGlow);
  cell.gl_ready = true;
  return true;
}

void MultiSubjectGrid::render_grid(ImDrawList* grid_dl) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  const auto t_start = std::chrono::steady_clock::now();

  ImDrawList* dl = (grid_dl != nullptr) ? grid_dl : ImGui::GetWindowDrawList();
  sync_slots();
  grid_origin_ = ImGui::GetCursorScreenPos();

  gl_ready_count_ = 0;
  worst_cell_ms_ = 0.0f;

  if (cells_.empty()) {
    ImGui::TextDisabled("No grid slots.");
    last_grid_ms_ = 0.0f;
    return;
  }

  // Cell size derives from the region actually available, so the grid reflows
  // when the window is resized instead of overflowing it.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float spacing = cell_spacing_;
  const float cols_f = static_cast<float>(grid_cols_);
  float cw = (avail.x - spacing * (cols_f - 1.0f)) / cols_f;
  if (!(cw > 40.0f)) cw = 40.0f;
  float ch = cw * 0.70f;
  // Keep the whole page on screen when there is room to spare.
  const float rows_f = static_cast<float>(grid_rows_);
  const float max_h = (avail.y - 24.0f - spacing * (rows_f - 1.0f)) / rows_f;
  if (max_h > 40.0f && ch > max_h) ch = max_h;
  const ImVec2 cell_size(cw, ch);

  const ImVec2 fbs = ImGui::GetIO().DisplayFramebufferScale;
  const ImGuiIO& io = ImGui::GetIO();

  for (int row = 0; row < grid_rows_; ++row) {
    bool drew_any = false;
    for (int col = 0; col < grid_cols_; ++col) {
      const int idx = row * grid_cols_ + col;
      if (idx >= static_cast<int>(cells_.size())) break;
      GridCell& c = *cells_[static_cast<std::size_t>(idx)];
      ImGui::PushID(idx);

      if (!c.visible) {
        ImGui::Dummy(cell_size);
        const ImVec2 mn = ImGui::GetItemRectMin();
        dl->AddRect(mn, ImVec2(mn.x + cell_size.x, mn.y + cell_size.y),
                    IM_COL32(70, 74, 92, 255), 4.0f);
      } else if (!ensure_gl(c)) {
        // No GL 3.3 context: keep the layout and say so, never crash.
        ImGui::Dummy(cell_size);
        const ImVec2 mn = ImGui::GetItemRectMin();
        dl->AddRect(mn, ImVec2(mn.x + cell_size.x, mn.y + cell_size.y),
                    IM_COL32(150, 80, 80, 255), 4.0f);
        dl->AddText(ImVec2(mn.x + 6.0f, mn.y + 6.0f), IM_COL32(255, 190, 190, 255),
                    c.label.c_str());
        dl->AddText(ImVec2(mn.x + 6.0f, mn.y + 22.0f), IM_COL32(210, 210, 220, 255),
                    "GL 3.3 renderer unavailable");
      } else {
        ++gl_ready_count_;
        rebuild_window(c, false);
        c.renderer.set_snapshot(c.viz);

        const int rw = std::max(1, static_cast<int>(cell_size.x * fbs.x));
        const int rh = std::max(1, static_cast<int>(cell_size.y * fbs.y));
        if (rw != c.renderer.width() || rh != c.renderer.height()) c.renderer.resize(rw, rh);

        const unsigned int tex = c.renderer.render(last_dt_);
        c.last_frame_ms = c.renderer.last_frame_ms();
        if (c.last_frame_ms > worst_cell_ms_) worst_cell_ms_ = c.last_frame_ms;

        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), cell_size);

        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const bool is_live = (c.mode == SessionMode::Live);
        const ImU32 border = is_live ? IM_COL32(90, 220, 120, 255)
                                     : (c.data_loaded ? IM_COL32(120, 140, 200, 255)
                                                      : IM_COL32(120, 110, 90, 255));
        dl->AddRect(mn, mx, border, 4.0f, 0, 1.5f);

        // Label chip + status line drawn over the rendered texture.
        const std::string head = c.label;
        dl->AddText(ImVec2(mn.x + 6.0f, mn.y + 4.0f), IM_COL32(255, 255, 255, 235),
                    head.c_str());
        char stat[128];
        if (is_live) {
          std::snprintf(stat, sizeof(stat), "LIVE  %.1fs window  %zu pts",
                        static_cast<double>(c.time_duration), c.viz.points.size());
        } else if (!c.data_loaded) {
          std::snprintf(stat, sizeof(stat), "loading from SQLite...");
        } else {
          std::snprintf(stat, sizeof(stat), "t=%.1f/%.1fs  %zu pts  %.1f ms",
                        static_cast<double>(c.playback_time), c.span_s,
                        c.viz.points.size(), static_cast<double>(c.last_frame_ms));
        }
        dl->AddText(ImVec2(mn.x + 6.0f, mx.y - 18.0f), IM_COL32(215, 225, 245, 225), stat);

        // Per-cell orbit/pan/zoom, so each subject can be inspected separately.
        if (ImGui::IsItemHovered()) {
          if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            c.renderer.camera().orbit(io.MouseDelta.x, io.MouseDelta.y);
          if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
            c.renderer.camera().pan(io.MouseDelta.x, io.MouseDelta.y);
          if (io.MouseWheel != 0.0f) c.renderer.camera().zoom(io.MouseWheel * 120.0f);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            c.renderer.camera().reset();
            c.playback_time = 0.0f;
            rebuild_window(c, true);
          }
        }
      }

      ImGui::PopID();
      drew_any = true;
      if (col + 1 < grid_cols_ && idx + 1 < static_cast<int>(cells_.size()))
        ImGui::SameLine(0.0f, spacing);
    }
    if (!drew_any) break;
  }

  const auto t_end = std::chrono::steady_clock::now();
  last_grid_ms_ = std::chrono::duration<float, std::milli>(t_end - t_start).count();
}

// ------------------------------------------------------------ fit / scroll --

void MultiSubjectGrid::fit_all_cells() {
  page_ = 0;
  for (auto& up : cells_) {
    GridCell& c = *up;
    c.renderer.camera().reset();
    c.camera.reset();
    if (c.mode != SessionMode::Live) c.playback_time = 0.0f;
    rebuild_window(c, true);
  }
}

void MultiSubjectGrid::scroll_to_center(ImVec2 view_center) {
  if (ImGui::GetCurrentContext() == nullptr) return;
  const ImVec2 wp = ImGui::GetWindowPos();
  ImGui::SetScrollFromPosX(view_center.x - wp.x, 0.5f);
  ImGui::SetScrollFromPosY(view_center.y - wp.y, 0.5f);
}

}  // namespace cardiac::ui
