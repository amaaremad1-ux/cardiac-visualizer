// grid_viewport.hpp - Multi-subject 3D trajectory grid viewport for the Cardiac
// Dynamics Visualizer. Manages a dynamic ImGui grid of per-subject 3D renderer
// cells, each displaying a live glowing filament + particle animation drawn from
// that subject's IBI trajectory data (live or recorded).
//
// The grid supports:
//   - Subject selector/range: choose specific subject IDs or a range/batch of
//     indexed subjects from the SQLite database (500+ cohort).
//   - Time period/duration control: set the rendering time window in seconds for
//     each visualizer cell's animation playback.
//   - Live smartwatch switch: toggle live smart-watch streaming per active cell.
//   - Viewport clipping: each cell renders into its own glViewport region so
//     all trajectories share the same GL context and shader pipeline without
//     exceeding ~30 FPS on a modern GPU.
//
// Thread model:
//   - The UI thread owns the grid layout, subject selection, and time-window
//     parameters. Updates are enqueued to the async database worker where needed.
//   - Each cell's TrajectoryRenderer runs its own update+render cycle on the
//     UI thread (no worker threads needed for rendering).
//
// THREE-TIER MODEL (this is what makes a 500+ subject cohort viable):
//   cohort_    -> every indexed subject. Ids + labels only, so 500+ entries cost
//                 a few tens of KB and never touch the GPU or SQLite.
//   selection_ -> the subset the user filtered/ranged down to. Still metadata only.
//   cells_     -> the GL-backed slots actually drawn, bounded by grid_cols_ *
//                 grid_rows_. Only these own a TrajectoryRenderer and resident
//                 trajectory points.
// Paging rebinds the SAME slot objects to different subjects, so shaders and FBOs
// are compiled once and never re-created while the user scrolls the cohort. The
// grid never calls SQLite itself: it publishes the subject ids it needs and the
// owner (dashboard_frame) fetches them with
// DatabaseManager::request_sessions_batch_async, keeping the UI thread free.

#pragma once

#include "cardiac/database_manager.hpp"
#include "cardiac/types.hpp"
#include "cardiac/trajectory_engine.hpp"
#include "trajectory_renderer.hpp"
#include "camera3d.hpp"
#include "gl_framebuffer.hpp"
#include "gl_shader.hpp"

#include "imgui.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cardiac::ui {

// One indexed subject in the cohort. Deliberately tiny: a 500+ entry index must
// stay cheap enough to hold, filter, and page through every frame.
struct SubjectRef {
  std::string id;
  std::string label;
};

// One grid cell: owns the trajectory renderer, camera, and per-subject data.
struct GridCell {
  std::string subject_id;
  std::string label;
  VisualizationSnapshot viz;
  SessionMode mode = SessionMode::SyntheticTest;
  float playback_time = 0.0f;   // current animation position in seconds
  float time_duration = 60.0f;  // rendering window / playback duration
  bool live_enabled = false;   // true = live smartwatch stream active

  TrajectoryRenderer renderer;
  gl::Camera3D camera;
  int cell_index = -1;
  bool visible = true;

  // --- resident trajectory for the bound subject ---
  // Cached (not re-read from SQLite) so moving the time-duration slider or
  // scrubbing playback re-windows instantly. Only ever populated for
  // materialized slots, i.e. at most grid_cols_ * grid_rows_ subjects.
  std::vector<TrajectoryPoint> full_points;
  double span_s = 0.0;          // total recorded duration in seconds
  bool data_loaded = false;
  bool data_requested = false;

  // --- GL lifecycle ---
  bool gl_ready = false;          // renderer.init() succeeded for this slot
  bool gl_attempted = false;      // do not retry a failed init every frame

  // --- window upload bookkeeping ---
  // The GPU buffer rebuild is quantized so continuous playback does not
  // re-upload thousands of points per cell per frame.
  double window_start_s = -1.0;
  float window_duration_s = -1.0f;
  std::uint64_t window_stamp = 0;

  float last_frame_ms = 0.0f;

  // --- live feed gate ---
  // Written by the frame loop, never by the grid. Copying a SessionController
  // snapshot is heavy relative to one integer read, so the dashboard only
  // re-feeds the live smartwatch slot when this stamp changes (or the cell was
  // cleared by a rebind). Keeps an idle BLE stream at ~zero cost per frame.
  std::size_t live_feed_stamp = 0;
};

// Dynamic, scrollable ImGui grid of 3D trajectory viewports for multiple
// subjects. Each cell renders a trajectory from the subject's IBI data.
class MultiSubjectGrid {
public:
  MultiSubjectGrid();
  ~MultiSubjectGrid();

  // Subject management
  int add_subject(const std::string& subject_id, const std::string& label,
                  const VisualizationSnapshot& viz,
                  SessionMode mode = SessionMode::SyntheticTest);
  void remove_subject(int cell_index);
  void remove_subject(const std::string& subject_id);
  int subject_count() const { return (int)cells_.size(); }

  // Subject selection: specific IDs or a range/batch (up to 500+).
  void set_subject_list(const std::vector<std::string>& subject_ids);
  void set_subject_range(const std::string& start_id, uint64_t count);
  void set_subject_filter(const std::function<bool(const std::string&)>& filter);

  // Parameter controls
  void set_time_duration(float seconds) { time_duration_ = seconds; }
  float time_duration() const { return time_duration_; }
  void set_live_enabled(bool enabled) { live_enabled_ = enabled; }
  bool live_enabled() const { return live_enabled_; }
  void set_grid_size(int cols, int rows);
  // Current layout. The control panel seeds its widgets from these so drawing the
  // panel cannot silently overwrite a size that was set programmatically.
  int grid_cols() const { return grid_cols_; }
  int grid_rows() const { return grid_rows_; }
  ImVec2 grid_origin() const { return grid_origin_; }
  float cell_spacing() const { return cell_spacing_; }

  // Grid rendering: called from the frame loop. Handles camera update, renderer
  // update/render, and draws each cell's texture into its view-port region.
  void update_cells(float dt);
  void render_grid(ImDrawList* grid_dl);

  // Per-cell: set live smartwatch data; called from the data adapter.
  void set_active_subject_data(const std::string& subject_id,
                               const VisualizationSnapshot& viz);

  // Cell access
  GridCell* cell_at(int index);
  const GridCell* cell_at(int index) const;
  GridCell* find_cell(const std::string& subject_id);

  // Host device data: called from the application loop when a live session is
  // present. Populates the cell's trajectory engine.
  void update_cell_from_session(GridCell& cell, const SessionSnapshot& snap,
                                const VisualizationSnapshot& viz);

  // Scrolling/fitting
  void fit_all_cells();
  void scroll_to_center(ImVec2 view_center);

  // ---------------------------------------------------------------- cohort --
  // Replaces the indexed subject list (500+ entries; metadata only). Resets the
  // selection and paging to the start of the new cohort.
  void set_cohort(std::vector<SubjectRef> subjects);
  int cohort_size() const { return (int)cohort_.size(); }
  const SubjectRef* cohort_at(int index) const;

  // Subjects passing the current filter/range. Paging walks this, not cohort_.
  int selection_size() const { return (int)selection_.size(); }
  const SubjectRef* selection_at(int index) const;
  void clear_filter();

  // ---------------------------------------------------------------- paging --
  int page_capacity() const;                 // slots drawn per page
  int page_count() const;
  int page() const { return page_; }
  void set_page(int page);
  void next_page();
  void prev_page();
  int first_selection_index() const;         // selection index of slot 0

  // ------------------------------------------------------- async data feed --
  // The grid publishes the ids it needs; the owner runs the SQLite batch query
  // on the DatabaseManager worker thread and hands the rows back.
  using BatchRequestFn = std::function<void(const std::vector<std::string>&)>;
  void set_batch_request_handler(BatchRequestFn handler) {
    batch_request_ = std::move(handler);
  }
  void attach_sessions(const std::vector<StoredSession>& sessions);
  int pending_request_count() const { return pending_requests_; }

  // ------------------------------------------------------------ diagnostics --
  float last_grid_ms() const { return last_grid_ms_; }
  float worst_cell_ms() const { return worst_cell_ms_; }
  int materialized_count() const { return materialized_; }
  int gl_ready_count() const { return gl_ready_count_; }
  void set_playback_speed(float s) { playback_speed_ = s; }
  float playback_speed() const { return playback_speed_; }
  // Point budget per cell. Grid cells are small, so decimating keeps 500+
  // subject paging smooth without a visible difference at cell resolution.
  void set_max_points_per_cell(int n) { max_points_per_cell_ = n; }
  int max_points_per_cell() const { return max_points_per_cell_; }
  // Slot index reserved for the live smartwatch stream (-1 when disabled).
  int live_slot_index() const { return live_enabled_ ? 0 : -1; }

private:
  void rebuild_selection();
  void sync_slots();
  void rebind_slot(GridCell& cell, const SubjectRef& ref);
  void request_missing_data();
  void advance_playback(float dt);
  void rebuild_window(GridCell& cell, bool force);
  bool ensure_gl(GridCell& cell);

  std::vector<std::unique_ptr<GridCell>> cells_;

  // Cohort / selection index (metadata only, safe at 500+ entries).
  std::vector<SubjectRef> cohort_;
  std::vector<SubjectRef> selection_;
  std::function<bool(const std::string&)> filter_;
  bool range_active_ = false;
  int range_start_ = 0;
  std::uint64_t range_count_ = 0;

  // Grid layout
  int grid_cols_ = 3;
  int grid_rows_ = 2;
  int page_ = 0;
  ImVec2 grid_origin_{0.f, 0.f};
  float cell_spacing_ = 12.0f;

  // Global parameter
  float time_duration_ = 60.0f;
  bool live_enabled_ = false;
  float playback_speed_ = 1.0f;
  int max_points_per_cell_ = 6000;

  // Async data plumbing
  BatchRequestFn batch_request_;
  int pending_requests_ = 0;

  // Diagnostics
  float last_dt_ = 1.f / 60.f;
  float last_grid_ms_ = 0.f;
  float worst_cell_ms_ = 0.f;
  int materialized_ = 0;
  int gl_ready_count_ = 0;
  std::uint64_t revision_counter_ = 0;

  // Camera shared across cells when present (synthetic/live session)
  gl::Camera3D shared_camera_;
};

}  // namespace cardiac::ui
