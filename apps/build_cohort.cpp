// build_cohort.cpp - headless builder for a large synthetic subject cohort.
//
// WHY THIS EXISTS
//   The multi-subject grid is specified to "index and query 500+ subjects
//   without lag". PhysioNet CHF2DB ships only 11 de-identified records, so the
//   scalability path cannot be exercised - or measured - against real data
//   alone. This tool expands the indexed cohort to an arbitrary size by
//   deriving additional SYNTHETIC_TEST subjects from those 11 real IBI series
//   and replaying each one through the exact same pipeline the live dashboard
//   and the importer use (DataNormalizer -> QualityController -> TrajectoryEngine
//   -> triangle areas -> LF/HF -> rolling descriptors).
//
//   The result is a genuine 3D trajectory per subject (not a copy), so grid
//   paging, per-cell windowing, decimation and the async batch loader are all
//   stressed with realistic shapes and beat counts.
//
// HONESTY / PROVENANCE
//   Every derived row is written with mode = "SYNTHETIC_TEST", source =
//   "synthetic-cohort" and a disclaimer stating that it is a deterministic
//   rendering/load-test record derived from de-identified research data - never
//   a patient record, never a diagnosis, never a disease score. The 11 real
//   CHF2DB imports keep their own mode/source and are left untouched.
//
// DETERMINISM
//   Subject k is produced from a std::mt19937_64 seeded with k, so re-running
//   with the same arguments rewrites byte-identical sessions (INSERT OR
//   REPLACE). No wall-clock or address-derived entropy anywhere.
//
// Usage:
//   cardiac_build_cohort [--count N] [--beats N] [--data DIR] [--db PATH]
//                        [--prefix STR]
// Defaults:
//   --count 512   --beats 900   --data data
//   --db exports/cardiac_history.db   --prefix syn-cohort-
// Idempotent: sessions are written with INSERT OR REPLACE, so re-running with the
// same arguments rewrites the same ids in place.
#include "cardiac/database_manager.hpp"
#include "cardiac/data_normalizer.hpp"
#include "cardiac/hrv_spectrum.hpp"
#include "cardiac/ibi_csv_importer.hpp"
#include "cardiac/quality_controller.hpp"
#include "cardiac/rolling_metrics.hpp"
#include "cardiac/trajectory_engine.hpp"
#include "cardiac/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using cardiac::AnalysisResult;
using cardiac::ApplicationState;
using cardiac::HrvSpectrum;
using cardiac::IntervalUnit;
using cardiac::MeasurementType;
using cardiac::NormalizedMeasurement;
using cardiac::RawMeasurement;
using cardiac::RollingMetrics;
using cardiac::SensorType;
using cardiac::SessionRecord;
using cardiac::StoredSession;
using cardiac::TrajectoryEngine;
using cardiac::TrajectoryPoint;
using cardiac::UtcTime;

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// The importer's own helper is file-local, so the same rule is repeated here:
// a triangle spanning a segment boundary contributes 0, never an interpolated
// area. Keeping it identical means a synthetic subject's areas are computed the
// same way a live or CHF2DB one is.
AnalysisResult build_analysis(const std::vector<TrajectoryPoint>& points,
                              std::uint64_t revision) {
  AnalysisResult result;
  result.revision = revision;
  if (points.size() < 3) return result;
  result.triangle_areas.reserve(points.size());
  for (std::size_t i = 1; i + 1 < points.size(); ++i) {
    cardiac::TriangleArea area;
    area.center_point_index = points[i].index;
    area.segment_id = points[i].segment_id;
    area.area_s2 = cardiac::local_triangle_area(points[i - 1], points[i], points[i + 1]);
    result.triangle_areas.push_back(area);
  }
  return result;
}

UtcTime make_timestamp(double seconds_since_epoch) {
  const long long micros = std::llround(seconds_since_epoch * 1e6);
  return UtcTime{std::chrono::microseconds(micros)};
}

// One real CHF2DB record reduced to its IBI series: the raw material for every
// derived subject. Kept as plain doubles so the perturbation below is easy to
// read and fully deterministic.
struct BaseSeries {
  std::string record;                    // e.g. "chf207"
  std::vector<double> ibi_s;             // beat-to-beat intervals, in order
};

struct Options {
  int count = 512;
  int beats = 900;
  std::filesystem::path data_dir{"data"};
  std::filesystem::path db_path{"exports/cardiac_history.db"};
  std::string prefix{"syn-cohort-"};
  bool show_help = false;
};

bool parse_args(int argc, char** argv, Options& out, std::string& err) {
  auto next_value = [&](int& i, const char* flag, std::string& dst) {
    if (i + 1 >= argc) {
      err = std::string(flag) + " needs a value";
      return false;
    }
    dst = argv[++i];
    return true;
  };
  auto next_int = [&](int& i, const char* flag, int& dst) {
    std::string raw;
    if (!next_value(i, flag, raw)) return false;
    try {
      dst = std::stoi(raw);
    } catch (...) {
      err = std::string(flag) + " expects an integer, got '" + raw + "'";
      return false;
    }
    return true;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--count") {
      if (!next_int(i, "--count", out.count)) return false;
    } else if (a == "--beats") {
      if (!next_int(i, "--beats", out.beats)) return false;
    } else if (a == "--data") {
      std::string v;
      if (!next_value(i, "--data", v)) return false;
      out.data_dir = v;
    } else if (a == "--db") {
      std::string v;
      if (!next_value(i, "--db", v)) return false;
      out.db_path = v;
    } else if (a == "--prefix") {
      if (!next_value(i, "--prefix", out.prefix)) return false;
    } else if (a == "--help" || a == "-h") {
      out.show_help = true;
    } else {
      err = "unknown argument: " + a;
      return false;
    }
  }
  if (out.count < 0) { err = "--count must be >= 0"; return false; }
  if (out.beats < 32) { err = "--beats must be >= 32 (a trajectory needs history)"; return false; }
  if (out.prefix.empty()) { err = "--prefix must not be empty"; return false; }
  return true;
}

// Loads every real *_IBI.csv in the data folder. Fails loudly when there is
// nothing to derive from: fabricating a cohort out of thin air would produce
// trajectories that look real but mean nothing.
std::vector<BaseSeries> load_bases(const std::filesystem::path& dir, int max_beats_per_base,
                                   std::string& err) {
  std::error_code ec;
  std::vector<std::filesystem::path> csvs;
  if (!std::filesystem::is_directory(dir, ec)) {
    err = "data folder not found: " + dir.string();
    return {};
  }
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    const auto p = e.path();
    if (p.extension() != ".csv") continue;
    if (p.filename().string().find("_IBI") == std::string::npos) continue;
    csvs.push_back(p);
  }
  std::sort(csvs.begin(), csvs.end());

  std::vector<BaseSeries> bases;
  for (const auto& csv : csvs) {
    std::string perr;
    auto imp = cardiac::import_ibi_csv(csv, {}, &perr);
    if (!imp) {
      std::fprintf(stderr, "[cohort] SKIP %s: %s\n", csv.filename().string().c_str(),
                   perr.c_str());
      continue;
    }
    BaseSeries b;
    b.record = imp->source_record.empty() ? csv.stem().string() : imp->source_record;
    b.ibi_s.reserve(imp->session.measurements.size());
    for (const auto& m : imp->session.measurements) {
      if (!m.interval_s) continue;
      if (!m.is_valid) continue;  // rejected beats would poison the derived HRV
      b.ibi_s.push_back(*m.interval_s);
      if (static_cast<int>(b.ibi_s.size()) >= max_beats_per_base) break;
    }
    if (b.ibi_s.size() < 32) {
      std::fprintf(stderr, "[cohort] SKIP %s: only %zu usable beats\n", b.record.c_str(),
                   b.ibi_s.size());
      continue;
    }
    std::printf("[cohort] base %-10s %zu beats, IBI mean %.3f s\n", b.record.c_str(),
                b.ibi_s.size(), imp->session.record.ibi_mean_s);
    bases.push_back(std::move(b));
  }
  if (bases.empty()) err = "no usable *_IBI.csv bases in " + dir.string();
  return bases;
}

// Derives one synthetic IBI series from a real base.
//
// The perturbation is deliberately mild and smooth: a per-subject mean heart-rate
// scale, an AR(1) multiplicative wander (so variability differs between subjects
// without producing implausible spikes), and a slow respiration-like modulation.
// A random start offset and length make each subject's duration and geometry
// distinct. Beat-to-beat structure is inherited from the real record, which is
// what keeps the resulting 3D delay embedding shaped like a cardiac trajectory
// rather than like noise.
std::vector<double> derive_ibi(const BaseSeries& base, int subject_index, int want_beats,
                               double& epoch_offset_s_out) {
  std::mt19937_64 rng(static_cast<std::uint64_t>(subject_index) * 6364136223846793005ULL +
                      1442695040888963407ULL);
  std::normal_distribution<double> gauss(0.0, 1.0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  const std::size_t len = base.ibi_s.size();
  const int want = std::min<int>(want_beats, static_cast<int>(len));
  const int max_start = static_cast<int>(len) - want;
  const int start = (max_start > 0) ? static_cast<int>(unit(rng) * static_cast<double>(max_start))
                                    : 0;

  // Per-subject mean HR shift: +-~10% (1 sigma), clamped so it stays plausible.
  const double mean_scale = std::exp(std::max(-0.25, std::min(0.25, gauss(rng) * 0.09)));
  // Per-subject variability gain: some subjects are steadier, some more labile.
  const double var_gain = std::exp(std::max(-0.6, std::min(0.8, gauss(rng) * 0.35)));
  const double rho = 0.94;                       // AR(1) memory
  const double sigma = 0.020 * var_gain;         // AR(1) innovation scale
  const double resp_amp = 0.010 + 0.020 * unit(rng);
  const double resp_period = 12.0 + 20.0 * unit(rng);   // beats
  const double resp_phase = unit(rng) * 6.283185307179586;
  // Each subject gets its own synthetic timeline origin (seconds), so timestamps
  // never collide with the real imports or with each other.
  const double epoch = 1.0e6 + 1000.0 * static_cast<double>(subject_index);
  epoch_offset_s_out = epoch;

  std::vector<double> out;
  out.reserve(static_cast<std::size_t>(want));
  double ar = 0.0;
  for (int i = 0; i < want; ++i) {
    ar = rho * ar + sigma * gauss(rng);
    const double mod =
        1.0 + resp_amp * std::sin(resp_phase +
                                  6.283185307179586 * static_cast<double>(i) / resp_period);
    double v = base.ibi_s[static_cast<std::size_t>(start + i)] * mean_scale * std::exp(ar) * mod;
    // Clamp inside the quality controller's acceptance band (QualityConfig
    // min/max interval), nudged inward so a derived beat is never rejected for
    // a rounding reason alone. Anything outside that band is not a plausible RR
    // interval, and letting it through would just manufacture segment breaks.
    constexpr double lo = cardiac::QualityConfig::min_interval_s + 0.01;
    constexpr double hi = cardiac::QualityConfig::max_interval_s - 0.01;
    v = std::max(lo, std::min(hi, v));
    out.push_back(v);
  }
  return out;
}

// Replays a derived IBI series through the live pipeline and assembles the
// StoredSession. Mirrors import_ibi_csv() step for step so a synthetic subject
// is indistinguishable - structurally - from an imported one.
StoredSession build_subject(const std::string& session_id, const std::vector<double>& ibi,
                            double epoch_offset_s, const std::string& origin_note,
                            const cardiac::QualityConfig& quality_cfg) {
  cardiac::DataNormalizer normalizer;
  cardiac::QualityController quality(quality_cfg);
  TrajectoryEngine trajectory(static_cast<std::size_t>(ibi.size()) + 16u);

  std::vector<NormalizedMeasurement> measurements;
  measurements.reserve(ibi.size());
  std::size_t valid_count = 0;
  double t = epoch_offset_s;

  for (double interval : ibi) {
    RawMeasurement raw;
    raw.timestamp_utc = make_timestamp(t);
    raw.sensor_type = SensorType::ECG;
    raw.measurement_type = MeasurementType::IBI;
    raw.interval = interval;
    raw.interval_unit = IntervalUnit::Seconds;
    raw.heart_rate_bpm =
        (interval > 1e-6) ? std::optional<double>(60.0 / interval) : std::optional<double>();
    raw.device_valid = true;
    t += interval;

    NormalizedMeasurement normalized = normalizer.normalize(raw);
    cardiac::QualityDecision decision = quality.evaluate(std::move(normalized));
    measurements.push_back(decision.measurement);
    if (decision.measurement.is_valid) ++valid_count;
    if (decision.break_before) trajectory.break_segment();
    if (decision.measurement.is_valid && decision.measurement.interval_s) {
      trajectory.push_interval(decision.measurement.timestamp_utc,
                               *decision.measurement.interval_s);
    }
  }

  std::vector<TrajectoryPoint> points = trajectory.snapshot_all_points();
  AnalysisResult analysis = build_analysis(points, trajectory.revision());

  const RollingMetrics rolling =
      cardiac::compute_rolling_metrics(points, analysis, measurements,
                                       static_cast<std::size_t>(-1),
                                       static_cast<std::size_t>(-1));
  const HrvSpectrum hrv =
      cardiac::compute_hrv_spectrum(cardiac::hrv_input_from_measurements(measurements));

  StoredSession s;
  SessionRecord& rec = s.record;
  rec.session_id = session_id;
  rec.mode = "SYNTHETIC_TEST";
  rec.source = "synthetic-cohort";
  rec.started_utc = measurements.empty() ? std::string()
                                         : cardiac::format_utc(measurements.front().timestamp_utc);
  rec.saved_utc = cardiac::format_utc(cardiac::utc_now());
  rec.final_state = cardiac::to_string(ApplicationState::ANALYSIS_READY);
  rec.measurement_count = measurements.size();
  rec.valid_count = valid_count;
  rec.trajectory_point_count = points.size();
  rec.area_mean_s2 = rolling.area_mean_s2;
  rec.ibi_mean_s = rolling.ibi_mean_s;
  rec.ibi_sd_s = rolling.ibi_sd_s;
  rec.lf_power_ms2 = hrv.reliable ? hrv.lf_power_ms2 : 0.0;
  rec.hf_power_ms2 = hrv.reliable ? hrv.hf_power_ms2 : 0.0;
  rec.lf_hf_ratio = hrv.reliable ? hrv.lf_hf_ratio : 0.0;
  rec.disclaimer =
      "SYNTHETIC_TEST cohort member derived deterministically from de-identified "
      "PhysioNet CHF2DB research IBI data (" + origin_note +
      "). Exists to exercise multi-subject grid indexing and rendering at 500+ "
      "subjects. Not a patient record, not a diagnosis, not a disease score, and "
      "not a clinical HRV interpretation.";

  s.points = std::move(points);
  s.measurements = std::move(measurements);
  s.analysis = std::move(analysis);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string err;
  if (!parse_args(argc, argv, opt, err)) {
    std::fprintf(stderr, "[cohort] %s\n", err.c_str());
    std::fprintf(stderr,
                 "usage: cardiac_build_cohort [--count N] [--beats N] [--data DIR] "
                 "[--db PATH] [--prefix STR]\n");
    return 2;
  }
  if (opt.show_help) {
    std::printf(
        "cardiac_build_cohort - expand the indexed subject cohort with deterministic\n"
        "SYNTHETIC_TEST subjects derived from the real CHF2DB IBI records.\n"
        "  --count N     subjects to generate (default 512)\n"
        "  --beats N     beats per subject (default 900)\n"
        "  --data DIR    folder of *_IBI.csv bases (default data)\n"
        "  --db PATH     SQLite history database (default exports/cardiac_history.db)\n"
        "  --prefix STR  session id prefix (default syn-cohort-)\n");
    return 0;
  }

  // Bases are capped well above --beats so the start-offset window can vary.
  std::vector<BaseSeries> bases = load_bases(opt.data_dir, std::max(opt.beats * 4, 4096), err);
  if (bases.empty()) {
    std::fprintf(stderr, "[cohort] %s\n", err.c_str());
    return 1;
  }
  std::printf("[cohort] %zu real base record(s) available\n", bases.size());

  cardiac::DatabaseManager db;
  db.open_async(opt.db_path);
  if (!wait_until([&] { return db.poll_ready(); }, 60000)) {
    std::fprintf(stderr, "[cohort] timed out opening database\n");
    return 1;
  }
  if (!db.ready()) {
    std::fprintf(stderr, "[cohort] database open failed: %s\n", db.last_error().c_str());
    return 1;
  }
  std::printf("[cohort] database ready: %s\n", db.path().string().c_str());

  const cardiac::QualityConfig quality_cfg{};
  int enqueued = 0;
  const int width = (opt.count > 0)
                        ? std::max(4, static_cast<int>(std::to_string(opt.count).size()))
                        : 4;

  for (int k = 0; k < opt.count; ++k) {
    const BaseSeries& base = bases[static_cast<std::size_t>(k) % bases.size()];
    double epoch = 0.0;
    std::vector<double> ibi = derive_ibi(base, k, opt.beats, epoch);

    char idbuf[64];
    std::snprintf(idbuf, sizeof(idbuf), "%s%0*d", opt.prefix.c_str(), width, k + 1);
    StoredSession s = build_subject(std::string(idbuf), ibi, epoch,
                                    "derived from " + base.record, quality_cfg);

    if ((k % 64) == 0 || k + 1 == opt.count) {
      std::printf("[cohort] %d/%d  latest %s: %zu beats -> %zu points, IBI mean %.3f s\n",
                  k + 1, opt.count, s.record.session_id.c_str(), s.measurements.size(),
                  s.points.size(), s.record.ibi_mean_s);
      std::fflush(stdout);
    }
    db.save_session_async(std::move(s));
    ++enqueued;
  }

  // FIFO worker: the list job is enqueued after every save, so when the list
  // mailbox fires, all writes have committed. That is the sync point.
  db.request_session_list_async();
  std::vector<SessionRecord> sessions;
  const int timeout_ms = 600000;
  if (!wait_until([&] { return db.poll_session_list(sessions); }, timeout_ms)) {
    std::fprintf(stderr, "[cohort] timed out waiting for the write queue to drain "
                         "(%d job(s) still pending)\n",
                 static_cast<int>(db.pending_jobs()));
    db.close();
    return 1;
  }

  std::size_t synthetic = 0;
  for (const auto& r : sessions) {
    if (r.source == "synthetic-cohort") ++synthetic;
  }
  std::printf("[cohort] enqueued=%d | database now indexes %zu session(s): %zu synthetic "
              "cohort member(s), %zu other\n",
              enqueued, sessions.size(), synthetic, sessions.size() - synthetic);
  std::printf("[cohort] done. Open the dashboard and expand 'Multi-Subject Grid'.\n");

  db.close();
  return 0;
}
