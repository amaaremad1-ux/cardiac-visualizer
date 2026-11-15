// grid_tests.cpp - headless tests for the MultiSubjectGrid cohort/paging/window
// model that backs the dashboard's multi-subject viewport grid.
//
// SCOPE
//   These tests exercise everything that decides WHICH subjects are drawn and
//   WHAT slice of each subject's trajectory reaches the GPU: the three-tier
//   cohort -> selection -> materialized-slot model, paging math, the async batch
//   request contract, time-window slicing, per-cell decimation with segment-break
//   preservation, playback looping, and the reserved live-smartwatch slot.
//
//   They deliberately do NOT create a GL context or an ImGui context. Slots own a
//   TrajectoryRenderer, but its constructor is trivial and its destructor is a
//   no-op until init() succeeds, so a 500+ subject grid can be driven entirely on
//   the CPU. That keeps this suite runnable on headless CI, where cardiac_gl_smoke
//   already covers the GPU path (and skips cleanly without a 3.3 context).
//
// WHY IT MATTERS
//   The feature requirement is "index and query 500+ subjects without lag". The
//   property that makes that true is that cost follows the PAGE size (<=16 slots),
//   never the cohort size. Every assertion below pins that invariant, so a future
//   change that materializes the whole cohort fails here instead of in the UI.
#include "cardiac/grid_viewport.hpp"
#include "cardiac/database_manager.hpp"
#include "cardiac/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond, msg)                                            \
  do {                                                              \
    ++checks;                                                       \
    if (!(cond)) {                                                  \
      ++failures;                                                   \
      std::fprintf(stderr, "FAIL %s [%d]\n", (msg), __LINE__);       \
    }                                                               \
  } while (0)

#define CHECK_EQ(a, b, msg)                                         \
  do {                                                              \
    ++checks;                                                       \
    const auto va_ = (a);                                           \
    const auto vb_ = (b);                                           \
    if (!(va_ == vb_)) {                                            \
      ++failures;                                                   \
      std::fprintf(stderr, "FAIL %s [%d]: got %lld, want %lld\n",    \
                   (msg), __LINE__,                                 \
                   static_cast<long long>(va_),                     \
                   static_cast<long long>(vb_));                    \
    }                                                               \
  } while (0)

// String ids are compared separately: CHECK_EQ prints through long long, which
// cannot carry a std::string.
#define CHECK_STR(a, b, msg)                                        \
  do {                                                              \
    ++checks;                                                       \
    const std::string sa_ = (a);                                    \
    const std::string sb_ = (b);                                    \
    if (sa_ != sb_) {                                               \
      ++failures;                                                   \
      std::fprintf(stderr, "FAIL %s [%d]: got '%s', want '%s'\n",    \
                   (msg), __LINE__, sa_.c_str(), sb_.c_str());      \
    }                                                               \
  } while (0)

// Floats need a tolerance: exact equality on a value that has been through a
// double round-trip is a flaky assertion, not a strict one.
#define CHECK_NEAR(a, b, tol, msg)                                  \
  do {                                                              \
    ++checks;                                                       \
    const double da_ = static_cast<double>(a);                      \
    const double db_ = static_cast<double>(b);                      \
    if (!(std::fabs(da_ - db_) <= (tol))) {                         \
      ++failures;                                                   \
      std::fprintf(stderr, "FAIL %s [%d]: got %.6f, want %.6f +-%.6f\n", \
                   (msg), __LINE__, da_, db_, static_cast<double>(tol)); \
    }                                                               \
  } while (0)

using cardiac::StoredSession;
using cardiac::TrajectoryPoint;
using cardiac::UtcTime;
using cardiac::ui::GridCell;
using cardiac::ui::MultiSubjectGrid;
using cardiac::ui::SubjectRef;

UtcTime at_seconds(double s) {
  return UtcTime{std::chrono::microseconds(static_cast<long long>(std::llround(s * 1e6)))};
}

std::vector<SubjectRef> make_cohort(int n) {
  std::vector<SubjectRef> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    char id[32];
    std::snprintf(id, sizeof(id), "syn-cohort-%04d", i + 1);
    out.push_back(SubjectRef{std::string(id), std::string(id) + "  (synthetic-cohort)"});
  }
  return out;
}

// A synthetic subject: `beats` intervals of `ibi` seconds, split into `segments`
// equal chunks so segment breaks (quality gaps) are present and testable.
StoredSession make_session(const std::string& id, int beats, double ibi, int segments) {
  StoredSession s;
  s.record.session_id = id;
  s.record.mode = "SYNTHETIC_TEST";
  s.record.source = "grid-test";
  const int per_seg = std::max(1, beats / std::max(1, segments));
  s.points.reserve(static_cast<std::size_t>(std::max(0, beats)));
  double t = 0.0;
  for (int i = 0; i < beats; ++i) {
    TrajectoryPoint p;
    p.index = static_cast<std::uint64_t>(i);
    p.segment_id = static_cast<std::uint64_t>(i / per_seg);
    p.timestamp_utc = at_seconds(t);
    // A delay-embedding-ish spiral: distinct per subject when `ibi` differs.
    const double a = 0.35 * static_cast<double>(i);
    p.position.x = std::cos(a) * ibi;
    p.position.y = std::sin(a) * ibi;
    p.position.z = ibi * 0.5 + 0.01 * static_cast<double>(i % 17);
    s.points.push_back(p);
    t += ibi;
  }
  s.record.trajectory_point_count = s.points.size();
  s.record.measurement_count = s.points.size();
  return s;
}

// Captures what the grid asks the (async) database for, so the request contract
// can be asserted without touching SQLite.
struct BatchSpy {
  std::vector<std::vector<std::string>> calls;
  std::size_t total_ids = 0;
  void operator()(const std::vector<std::string>& ids) {
    calls.push_back(ids);
    total_ids += ids.size();
  }
};

// ------------------------------------------------------------------ tests --

// A 500+ cohort is indexed as metadata only, and the selection mirrors it until
// a filter or range narrows it down.
void test_cohort_index_at_scale() {
  MultiSubjectGrid grid;
  const int n = 600;  // > the 500+ requirement
  grid.set_cohort(make_cohort(n));

  CHECK_EQ(grid.cohort_size(), n, "cohort holds every indexed subject");
  CHECK_EQ(grid.selection_size(), n, "unfiltered selection == cohort");
  CHECK(grid.cohort_at(0) != nullptr, "cohort_at(0) valid");
  CHECK(grid.cohort_at(n - 1) != nullptr, "cohort_at(n-1) valid");
  CHECK(grid.cohort_at(n) == nullptr, "cohort_at out of range is null");
  CHECK_STR(grid.cohort_at(0)->id, "syn-cohort-0001", "cohort ordering preserved");
  CHECK_STR(grid.cohort_at(n - 1)->id, "syn-cohort-0600", "cohort tail id");
}

// The IBI Subject Selector path: a predicate filter narrows the selection, and
// clearing it restores the whole cohort.
void test_subject_filter() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));

  // Every id ending in "00" - i.e. syn-cohort-0100, 0200, ... 0600 => 6 matches.
  grid.set_subject_filter([](const std::string& id) {
    return id.size() >= 2 && id.compare(id.size() - 2, 2, "00") == 0;
  });
  CHECK_EQ(grid.selection_size(), 6, "filter narrows the selection");
  CHECK_STR(grid.selection_at(0)->id, "syn-cohort-0100", "first match");
  CHECK_STR(grid.selection_at(5)->id, "syn-cohort-0600", "last match");

  grid.clear_filter();
  CHECK_EQ(grid.selection_size(), 600, "clear_filter restores the whole cohort");
}

// Batch/range selection: pick a contiguous slice of the indexed cohort.
void test_range_selection() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));

  grid.set_subject_range("syn-cohort-0250", 40);
  CHECK_EQ(grid.selection_size(), 40, "range yields exactly count subjects");
  CHECK_STR(grid.selection_at(0)->id, "syn-cohort-0250", "range starts at the id");
  CHECK_STR(grid.selection_at(39)->id, "syn-cohort-0289", "range end is inclusive");

  // A range past the end of the cohort clamps instead of overflowing.
  grid.set_subject_range("syn-cohort-0590", 100);
  CHECK_EQ(grid.selection_size(), 11, "range clamps at the cohort tail");

  // An unknown start id falls back to the beginning rather than selecting nothing.
  grid.set_subject_range("no-such-subject", 5);
  CHECK_EQ(grid.selection_size(), 5, "unknown range start still yields a usable page");

  grid.clear_filter();
  CHECK_EQ(grid.selection_size(), 600, "clear_filter resets the range too");
}

// The core scalability invariant: only grid_cols*grid_rows slots are ever
// materialized, no matter how large the cohort is. This is what keeps a 500+
// subject index inside a real-time GPU budget.
void test_bounded_materialization() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));
  grid.set_grid_size(4, 4);
  CHECK_EQ(grid.page_capacity(), 16, "capacity == cols*rows");

  BatchSpy spy;
  grid.set_batch_request_handler([&spy](const std::vector<std::string>& ids) { spy(ids); });

  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(grid.subject_count(), 16, "only one page of slots exists");
  CHECK_EQ(grid.materialized_count(), 16, "all 16 slots are materialized");
  CHECK_EQ(spy.total_ids, 16u, "exactly the visible page is requested from SQLite");
  CHECK_EQ(spy.calls.size(), 1u, "one batched request, not one query per subject");

  // A second tick must not re-ask for the same subjects (that would hammer the
  // DB worker every frame while the user waits for data).
  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(spy.calls.size(), 1u, "no duplicate batch request for the same page");
  CHECK_EQ(grid.pending_request_count(), 16, "in-flight requests are tracked");
}

// Paging walks the selection in capacity-sized steps and clamps at both ends.
void test_paging_math() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));
  grid.set_grid_size(3, 2);  // capacity 6
  CHECK_EQ(grid.page_capacity(), 6, "capacity 3x2");
  CHECK_EQ(grid.page_count(), 100, "600 subjects / 6 per page == 100 pages");
  CHECK_EQ(grid.first_selection_index(), 0, "page 0 starts at selection 0");

  grid.next_page();
  CHECK_EQ(grid.page(), 1, "next_page advances");
  CHECK_EQ(grid.first_selection_index(), 6, "page 1 starts at selection 6");

  grid.set_page(99);
  CHECK_EQ(grid.page(), 99, "set_page to the last page");
  grid.next_page();
  CHECK_EQ(grid.page(), 99, "next_page clamps at the last page");
  grid.set_page(5000);
  CHECK_EQ(grid.page(), 99, "set_page clamps an absurd page");
  grid.set_page(-5);
  CHECK_EQ(grid.page(), 0, "set_page clamps a negative page");
  grid.prev_page();
  CHECK_EQ(grid.page(), 0, "prev_page clamps at page 0");

  // Paging to a new page must ask for exactly that page's subjects.
  BatchSpy spy;
  grid.set_batch_request_handler([&spy](const std::vector<std::string>& ids) { spy(ids); });
  grid.set_page(50);
  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(spy.calls.size(), 1u, "one request after a page change");
  CHECK_EQ(spy.calls[0].size(), 6u, "the request covers the new page only");
  CHECK_STR(spy.calls[0].front(), "syn-cohort-0301", "page 50 starts at subject 301");
  CHECK_STR(spy.calls[0].back(), "syn-cohort-0306", "page 50 ends at subject 306");
}

// Changing the grid dimensions re-clamps the page and re-sizes the slot pool
// without recreating more slots than the new capacity.
void test_grid_resize() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));
  grid.set_grid_size(2, 2);
  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(grid.subject_count(), 4, "2x2 materializes 4 slots");

  grid.set_grid_size(4, 4);
  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(grid.subject_count(), 16, "growing to 4x4 materializes 16 slots");
  CHECK_EQ(grid.page_capacity(), 16, "capacity follows the resize");

  // Dimensions clamp independently to [1,4], so an absurd request degrades to a
  // valid grid instead of allocating an unbounded number of GL slots.
  grid.set_grid_size(99, 0);
  CHECK_EQ(grid.page_capacity(), 4, "99x0 clamps per-dimension to 4x1");
  grid.set_grid_size(-3, -7);
  CHECK_EQ(grid.page_capacity(), 1, "negative dims clamp to 1x1");
  grid.set_grid_size(1, 1);
  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(grid.subject_count(), 1, "shrinking to 1x1 drops the extra slots");
}

// attach_sessions() routes each row to the slot bound to that subject id and
// ignores rows whose page has already scrolled away.
void test_attach_sessions_routing() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));
  grid.set_grid_size(3, 2);
  grid.set_batch_request_handler([](const std::vector<std::string>&) {});
  grid.update_cells(1.0f / 60.0f);

  std::vector<StoredSession> rows;
  rows.push_back(make_session("syn-cohort-0001", 200, 0.80, 1));
  rows.push_back(make_session("syn-cohort-0004", 200, 0.60, 2));
  rows.push_back(make_session("syn-cohort-0999", 200, 0.70, 1));  // not on this page
  grid.attach_sessions(rows);

  GridCell* c0 = grid.find_cell("syn-cohort-0001");
  CHECK(c0 != nullptr, "slot 0 bound to the first page subject");
  if (c0 != nullptr) {
    CHECK(c0->data_loaded, "attached data marked loaded");
    CHECK_EQ(c0->full_points.size(), 200u, "all beats are resident for re-windowing");
    // 199 intervals * 0.8 s
    CHECK(std::fabs(c0->span_s - 159.2) < 0.01, "span derived from the timestamps");
    CHECK(!c0->viz.points.empty(), "the window was built immediately on attach");
  }

  GridCell* c3 = grid.find_cell("syn-cohort-0004");
  CHECK(c3 != nullptr, "slot 3 bound to its subject");
  if (c3 != nullptr) CHECK_EQ(c3->full_points.size(), 200u, "slot 3 data resident");

  CHECK(grid.find_cell("syn-cohort-0999") == nullptr,
        "a stale row for an off-page subject is ignored, not materialized");

  // Subjects still waiting on the worker are reported as pending, not loaded.
  GridCell* c1 = grid.find_cell("syn-cohort-0002");
  CHECK(c1 != nullptr, "slot 1 exists");
  if (c1 != nullptr) CHECK(!c1->data_loaded, "unanswered subjects stay unloaded");
}

// The Time Duration control must reach every materialized cell and bound the
// slice of the trajectory handed to the GPU.
void test_time_duration_window() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));
  grid.set_grid_size(2, 1);
  grid.set_time_duration(30.0f);
  grid.set_playback_speed(0.0f);  // freeze so the window is deterministic
  grid.set_batch_request_handler([](const std::vector<std::string>&) {});
  grid.update_cells(1.0f / 60.0f);

  // 30 s at 0.5 s/beat == 60 beats inside the window out of 400 recorded.
  std::vector<StoredSession> rows;
  rows.push_back(make_session("syn-cohort-0001", 400, 0.50, 1));
  rows.push_back(make_session("syn-cohort-0002", 400, 0.50, 1));
  grid.attach_sessions(rows);

  for (int i = 0; i < grid.subject_count(); ++i) {
    const GridCell* c = grid.cell_at(i);
    CHECK(c != nullptr, "cell exists");
    if (c == nullptr) continue;
    CHECK_NEAR(c->time_duration, 30.0, 1e-5, "the global duration reached the cell");
    CHECK_EQ(c->full_points.size(), 400u, "the full record stays resident");
    // Window slice: 60 beats, +1 because lower_bound on the end edge is inclusive
    // of the boundary beat in the common case; accept either 60 or 61.
    CHECK(c->viz.points.size() >= 59u && c->viz.points.size() <= 62u,
          "only the requested time window is uploaded, not the whole record");
  }

  // Widening the window widens the slice. Note the contract: update_cells()
  // advances playback and rebinds slots, but the actual re-windowing happens in
  // render_grid() (once per frame, quantized) or immediately on attach. Headless
  // we re-attach, which forces a rebuild at the new duration.
  grid.set_time_duration(120.0f);
  grid.update_cells(1.0f / 60.0f);
  grid.attach_sessions(rows);
  const GridCell* c0 = grid.cell_at(0);
  CHECK(c0 != nullptr, "cell 0 exists after resize");
  if (c0 != nullptr) {
    CHECK_NEAR(c0->time_duration, 120.0, 1e-5, "the new duration reached the cell");
    CHECK(c0->viz.points.size() > 200u && c0->viz.points.size() <= 245u,
          "a 120 s window yields ~240 beats");
  }

  // A duration longer than the recording clamps to what exists rather than
  // inventing beats past the end.
  grid.set_time_duration(600.0f);
  grid.update_cells(1.0f / 60.0f);
  grid.attach_sessions(rows);
  const GridCell* c1 = grid.cell_at(1);
  CHECK(c1 != nullptr, "cell 1 exists");
  if (c1 != nullptr) {
    CHECK_EQ(c1->viz.points.size(), 400u, "an over-long window shows the whole record");
  }
}

// Decimation must respect the per-cell point budget AND keep the first point of
// every segment, so a quality gap never gets visually bridged by a decimated
// polyline. This is the same invariant the hero renderer relies on.
void test_decimation_preserves_segment_breaks() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(4));
  grid.set_grid_size(1, 1);
  grid.set_time_duration(600.0f);  // whole record
  grid.set_max_points_per_cell(256);
  grid.set_playback_speed(0.0f);
  grid.set_batch_request_handler([](const std::vector<std::string>&) {});
  grid.update_cells(1.0f / 60.0f);

  // 4000 beats in 8 segments, way over the 256-point budget.
  std::vector<StoredSession> rows;
  rows.push_back(make_session("syn-cohort-0001", 4000, 0.10, 8));
  grid.attach_sessions(rows);
  grid.update_cells(1.0f / 60.0f);

  const GridCell* c = grid.cell_at(0);
  CHECK(c != nullptr, "cell exists");
  if (c == nullptr) return;
  CHECK_EQ(c->full_points.size(), 4000u, "the full record is resident");
  // Hard cap is budget*2 because segment starts and the final beat are always kept.
  CHECK(c->viz.points.size() <= 512u, "the window respects the point budget");
  CHECK(c->viz.points.size() >= 200u, "decimation kept a useful polyline");

  std::vector<std::uint64_t> emitted_segments;
  for (const auto& p : c->viz.points) {
    if (emitted_segments.empty() || emitted_segments.back() != p.segment_id)
      emitted_segments.push_back(p.segment_id);
  }
  CHECK_EQ(emitted_segments.size(), 8u, "every segment boundary survived decimation");
  CHECK_EQ(c->viz.points.back().index, 3999u, "the newest beat is always drawn");
}

// Playback advances with dt * speed and loops inside [0, span - duration] so a
// short window keeps cycling through a long recording.
void test_playback_advances_and_loops() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(2));
  grid.set_grid_size(1, 1);
  grid.set_time_duration(30.0f);
  grid.set_playback_speed(1.0f);
  grid.set_batch_request_handler([](const std::vector<std::string>&) {});
  grid.update_cells(1.0f / 60.0f);

  std::vector<StoredSession> rows;
  rows.push_back(make_session("syn-cohort-0001", 200, 0.50, 1));  // span 99.5 s
  grid.attach_sessions(rows);

  GridCell* c = grid.cell_at(0);
  CHECK(c != nullptr, "cell exists");
  if (c == nullptr) return;
  const double loop_span = c->span_s - 30.0;  // 69.5
  CHECK(loop_span > 0.0, "the record is longer than the window, so it can loop");

  grid.update_cells(0.2f);  // +0.2 s per tick at speed 1
  CHECK_NEAR(c->playback_time, 0.2, 1e-3, "playback advanced by dt * speed");

  // A dt outside (0, 0.25) is treated as a stall and replaced by a 1/60 s tick,
  // so resuming from a breakpoint or a long GC pause can never make the grid
  // jump to the end of a recording.
  const float before_stall = c->playback_time;
  grid.update_cells(5.0f);
  CHECK_NEAR(c->playback_time, static_cast<double>(before_stall) + 1.0 / 60.0, 1e-3,
             "an implausible dt falls back to a 1/60 s tick");

  // Drive well past the end of the recording: playback must wrap, never run off
  // the tail and never go negative.
  grid.set_playback_speed(8.0f);
  bool wrapped = false;
  float prev = c->playback_time;
  bool in_range = true;
  for (int i = 0; i < 200; ++i) {
    grid.update_cells(0.2f);  // +1.6 s per tick => 320 s over a 69.5 s loop
    if (c->playback_time < prev) wrapped = true;
    prev = c->playback_time;
    if (c->playback_time < 0.0f ||
        static_cast<double>(c->playback_time) > loop_span + 1e-3) {
      in_range = false;
    }
  }
  CHECK(wrapped, "playback loops back to the start of a long recording");
  CHECK(in_range, "playback stays inside [0, span - duration] at all times");

  // Freezing stops the clock but keeps the window drawn.
  grid.set_playback_speed(0.0f);
  const float frozen = c->playback_time;
  for (int i = 0; i < 10; ++i) grid.update_cells(0.2f);
  CHECK_NEAR(c->playback_time, frozen, 1e-5, "speed 0 freezes playback");
  CHECK(!c->viz.points.empty(), "a frozen cell still renders its window");
}

// The Live Smartwatch Stream switch reserves slot 0: it is never requested from
// SQLite, it is fed by the owner, and the cohort pages through one fewer slot.
void test_live_slot_reservation() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(600));
  grid.set_grid_size(3, 2);  // capacity 6
  CHECK_EQ(grid.live_slot_index(), -1, "no live slot while disabled");

  BatchSpy spy;
  grid.set_batch_request_handler([&spy](const std::vector<std::string>& ids) { spy(ids); });

  grid.set_live_enabled(true);
  CHECK_EQ(grid.live_slot_index(), 0, "slot 0 is the live slot");
  CHECK_EQ(grid.page_capacity(), 6, "capacity is unchanged");
  CHECK_EQ(grid.first_selection_index(), 0, "page 0 still starts at selection 0");
  // 600 subjects through 5 cohort slots => 120 pages.
  CHECK_EQ(grid.page_count(), 120, "the live slot removes one paging slot");

  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(grid.subject_count(), 6, "all 6 slots still exist");
  CHECK_EQ(spy.total_ids, 5u, "SQLite is asked for 5 recorded subjects, never the live one");
  for (const auto& id : spy.calls[0]) {
    CHECK(id != "__live_smartwatch__", "the live subject id is never sent to the database");
  }

  const GridCell* live = grid.cell_at(0);
  CHECK(live != nullptr, "the live cell exists");
  if (live != nullptr) {
    CHECK_STR(live->subject_id, "__live_smartwatch__", "live cell identity");
    CHECK(live->live_enabled, "live cell flagged live");
    CHECK(live->mode == cardiac::SessionMode::Live, "live cell mode is Live");
    CHECK(!live->data_loaded, "live cell starts empty until the watch streams");
  }

  // Feeding the live cell from a session snapshot pins it to the newest beats.
  cardiac::SessionSnapshot snap;
  snap.session_id = "live-watch";
  snap.mode = cardiac::SessionMode::Live;
  auto live_viz_rows = make_session("live", 400, 0.50, 1);
  cardiac::VisualizationSnapshot viz;
  viz.mode = cardiac::SessionMode::Live;
  viz.points = live_viz_rows.points;
  viz.revision = 7;
  GridCell* lc = grid.cell_at(0);
  CHECK(lc != nullptr, "live cell writable");
  if (lc != nullptr) {
    grid.update_cell_from_session(*lc, snap, viz);
    CHECK(lc->data_loaded, "live cell reports data after a feed");
    CHECK_EQ(lc->full_points.size(), 400u, "live beats are resident");
    // span 199.5 s, window 60 s => playback pinned to 139.5 s
    CHECK(std::fabs(static_cast<double>(lc->playback_time) - 139.5) < 0.01,
          "live playback trails the newest beat");
    CHECK(!lc->viz.points.empty(), "live window built immediately");
    grid.update_cells(1.0f);
    CHECK(std::fabs(static_cast<double>(lc->playback_time) - 139.5) < 0.01,
          "the live slot stays pinned to the tail across ticks");
  }

  // Turning live off hands slot 0 back to the cohort.
  grid.set_live_enabled(false);
  grid.update_cells(1.0f / 60.0f);
  CHECK_EQ(grid.live_slot_index(), -1, "live slot released");
  CHECK_EQ(grid.page_count(), 100, "all 6 slots page the cohort again");
  const GridCell* c0 = grid.cell_at(0);
  CHECK(c0 != nullptr, "slot 0 exists");
  if (c0 != nullptr) {
    CHECK_STR(c0->subject_id, "syn-cohort-0001", "slot 0 rebound to the cohort");
  }
}

// The measured cost of driving a 500+ cohort must stay flat: this is the
// "without lag" requirement expressed as a number rather than a hope.
void test_large_cohort_update_cost() {
  MultiSubjectGrid grid;
  grid.set_cohort(make_cohort(1000));
  grid.set_grid_size(4, 4);
  grid.set_batch_request_handler([](const std::vector<std::string>&) {});

  // Warm up, then time 300 ticks (~5 s at 60 FPS) including page changes, which
  // is the worst case: every page turn rebinds all 16 slots.
  grid.update_cells(1.0f / 60.0f);
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kTicks = 300;
  for (int i = 0; i < kTicks; ++i) {
    if ((i % 10) == 0) grid.next_page();
    grid.update_cells(1.0f / 60.0f);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double per_tick_ms = total_ms / static_cast<double>(kTicks);

  std::printf("[grid] 1000-subject cohort: %d ticks (with page turns) in %.2f ms "
              "=> %.4f ms/tick, %d slots materialized\n",
              kTicks, total_ms, per_tick_ms, grid.materialized_count());

  CHECK_EQ(grid.materialized_count(), 16, "still only one page of slots at 1000 subjects");
  // Two orders of magnitude below a 16.6 ms frame budget. Deliberately loose so
  // it is not flaky on a loaded CI machine while still catching an O(cohort)
  // regression, which would cost milliseconds per tick rather than microseconds.
  CHECK(per_tick_ms < 2.0, "per-tick cohort cost stays negligible");
}

}  // namespace

int main() {
  std::printf("grid_tests: MultiSubjectGrid cohort / paging / window model (headless)\n");

  test_cohort_index_at_scale();
  test_subject_filter();
  test_range_selection();
  test_bounded_materialization();
  test_paging_math();
  test_grid_resize();
  test_attach_sessions_routing();
  test_time_duration_window();
  test_decimation_preserves_segment_breaks();
  test_playback_advances_and_loops();
  test_live_slot_reservation();
  test_large_cohort_update_cost();

  std::printf("grid_tests: %d check(s), %d failure(s)\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
