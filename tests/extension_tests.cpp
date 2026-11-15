// extension_tests.cpp - headless verification for the Phase-2 additions:
// frequency-domain HRV (LF/HF), the CHF2DB IBI CSV importer, and the
// asynchronous SQLite3 DatabaseManager round-trip. Self-contained: it writes a
// synthetic CHF2DB-format CSV to a temp file, so it never depends on the
// research dataset being present. No GPU / no window required.
#include "cardiac/database_manager.hpp"
#include "cardiac/hrv_spectrum.hpp"
#include "cardiac/ibi_csv_importer.hpp"
#include "cardiac/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {
int failures = 0;
int checks = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::cerr << "FAIL " << msg << " [" << __LINE__ << "]\n";        \
        }                                                                    \
    } while (0)

void wait_for(const std::function<bool()>& pred, int tries = 400) {
    for (int i = 0; i < tries && !pred(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// Writes a de-identified CHF2DB-format IBI CSV: `beats` rows at ~0.8 s with a
// gentle sinusoidal wander, plus one long gap to force a segment break.
std::filesystem::path write_synthetic_csv(const std::filesystem::path& dir, int beats) {
    std::filesystem::create_directories(dir);
    const auto path = dir / "chfTEST_IBI.csv";
    std::ofstream f(path);
    f << "record,beat_index,sample_index,time_seconds,beat_annotation_code,"
         "beat_annotation_symbol,beat_annotation_label,ibi_samples,ibi_seconds,"
         "ibi_ms,heart_rate_bpm,qc_flag\n";
    double t = 100.0;
    for (int i = 1; i <= beats; ++i) {
        // Inject a 8 s gap at the midpoint -> exceeds max_gap_s -> break_before.
        if (i == beats / 2) t += 8.0;
        const double ibi = (i == 1) ? 0.0 : (0.80 + 0.05 * std::sin(0.35 * i));
        f << "chfTEST," << i << "," << static_cast<int>(t * 250) << "," << t
          << ",1,N,NORMAL,";
        if (i == 1) {
            f << ",,,,first_beat_no_previous_interval\n";
        } else {
            f << static_cast<int>(ibi * 250) << "," << ibi << "," << ibi * 1000.0
              << "," << 60.0 / ibi << ",\n";
        }
        t += ibi;
    }
    return path;
}

void test_hrv_basics() {
    using namespace cardiac;
    // Too-short series must be rejected, never faked.
    std::vector<HrvBandInput> tiny(10);
    for (std::size_t i = 0; i < 10; ++i) { tiny[i].elapsed_time_s = static_cast<double>(i) * 0.8; tiny[i].interval_s = 0.8; }
    HrvSpectrum s = compute_hrv_spectrum(tiny);
    CHECK(!s.reliable, "hrv rejects short series");
    CHECK(!s.reason.empty(), "hrv gives a reason");

    // A long, gently-varying series should be reliable with finite, non-negative
    // band powers and a non-negative ratio.
    std::vector<HrvBandInput> series;
    const int n = 400;
    for (int i = 0; i < n; ++i) {
        HrvBandInput b;
        b.elapsed_time_s = i * 0.8;
        b.interval_s = 0.80 + 0.06 * std::sin(2.0 * 3.14159 * 0.1 * (i * 0.8));
        series.push_back(b);
    }
    HrvSpectrum h = compute_hrv_spectrum(series);
    CHECK(h.reliable, "hrv reliable on long clean series");
    CHECK(std::isfinite(h.lf_power_ms2) && h.lf_power_ms2 >= 0.0, "lf power finite/non-negative");
    CHECK(std::isfinite(h.hf_power_ms2) && h.hf_power_ms2 >= 0.0, "hf power finite/non-negative");
    CHECK(std::isfinite(h.lf_hf_ratio) && h.lf_hf_ratio >= 0.0, "lf/hf ratio finite/non-negative");
    CHECK(h.windows >= 1, "welch produced at least one window");
    CHECK(h.psd_freq_hz.size() == h.psd_power.size(), "psd freq/power aligned");
    CHECK(!h.psd_freq_hz.empty(), "psd populated when keep_psd default true");
}

void test_importer_and_pipeline() {
    using namespace cardiac;
    const auto dir = std::filesystem::temp_directory_path() / "cardiac_import_test";
    const auto csv = write_synthetic_csv(dir, 260);

    std::string err;
    auto imp = import_ibi_csv(csv, {}, &err);
    CHECK(imp.has_value(), ("importer succeeds: " + err).c_str());
    if (!imp) return;

    CHECK(imp->source_record == "chfTEST", "source record parsed from CSV");
    CHECK(imp->rows_skipped >= 1, "first-beat empty IBI skipped");
    CHECK(imp->rows_imported >= 250, "most beats imported");
    CHECK(imp->session.points.size() > 0, "trajectory points built");
    CHECK(imp->session.measurements.size() == imp->rows_imported, "measurements match imports");
    // The injected 8 s gap must split the trajectory into >1 segment.
    std::uint64_t max_seg = 0;
    for (const auto& p : imp->session.points) max_seg = std::max(max_seg, p.segment_id);
    CHECK(max_seg >= 1, "gap produced a segment break (never bridged)");
    CHECK(imp->session.analysis.has_value(), "analysis present");
    CHECK(!imp->session.analysis->triangle_areas.empty(), "triangle areas computed");

    const auto& rec = imp->session.record;
    CHECK(rec.mode == "CHF2DB_RESEARCH", "mode labeled CHF2DB_RESEARCH");
    CHECK(rec.session_id == "chf2db-chfTEST", "deterministic session id");
    CHECK(!rec.disclaimer.empty(), "disclaimer present");
    CHECK(rec.disclaimer.find("PhysioNet") != std::string::npos, "attributes PhysioNet CHF2DB");
    CHECK(rec.trajectory_point_count == imp->session.points.size(), "record point count matches");
    CHECK(rec.ibi_mean_s > 0.5 && rec.ibi_mean_s < 1.2, "ibi mean plausible");

    // Bad path -> graceful failure with a reason, never a throw.
    std::string err2;
    auto bad = import_ibi_csv(dir / "does_not_exist.csv", {}, &err2);
    CHECK(!bad.has_value(), "missing file returns nullopt");
    CHECK(!err2.empty(), "missing file sets error");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Optional integration check against the real provided PhysioNet CHF2DB export.
// Runs only when the dataset is present in the repo; otherwise skips silently so
// the suite stays portable.
void test_real_chf2db_if_present() {
    using namespace cardiac;
    const std::filesystem::path candidates[] = {
        "E:/CardiacVisualizer/data/chf207_IBI.csv",
        "../data/chf207_IBI.csv",
        "data/chf207_IBI.csv",
    };
    std::filesystem::path csv;
    for (const auto& c : candidates)
        if (std::filesystem::exists(c)) { csv = c; break; }
    if (csv.empty()) {
        std::cout << "(real CHF2DB dataset not found; live-import check skipped)\n";
        return;
    }
    std::string err;
    auto imp = import_ibi_csv(csv, {}, &err);
    CHECK(imp.has_value(), ("real chf207 imports: " + err).c_str());
    if (!imp) return;
    CHECK(imp->source_record == "chf207", "real record id parsed (chf207)");
    CHECK(imp->rows_imported > 1000, "real record has many beats");
    CHECK(imp->session.points.size() > 100, "real trajectory built");
    CHECK(imp->session.analysis && !imp->session.analysis->triangle_areas.empty(),
          "real analysis computed");
    CHECK(imp->session.record.ibi_mean_s > 0.3 && imp->session.record.ibi_mean_s < 2.0,
          "real IBI mean within physiological bounds");
    std::cout << "Real chf207: " << imp->rows_imported << " beats, "
              << imp->session.points.size() << " points, IBI mean "
              << imp->session.record.ibi_mean_s << " s, HRV reliable="
              << (imp->hrv.reliable ? "yes" : "no") << "\n";
}

#ifdef CARDIAC_HAVE_SQLITE3
void test_database_roundtrip() {
    using namespace cardiac;
    const auto dir = std::filesystem::temp_directory_path() / "cardiac_db_test";
    std::filesystem::create_directories(dir);
    const auto db_path = dir / "history.db";
    const auto csv = write_synthetic_csv(dir, 200);

    std::string err;
    auto imp = import_ibi_csv(csv, {}, &err);
    CHECK(imp.has_value(), "import for db test");
    if (!imp) return;

    DatabaseManager db;
    db.open_async(db_path);
    wait_for([&] { return db.poll_ready(); });
    CHECK(db.ready(), "db opened");
    CHECK(db.last_error().empty(), ("db open clean: " + db.last_error()).c_str());

    const std::size_t n_points = imp->session.points.size();
    const std::size_t n_meas = imp->session.measurements.size();
    const std::size_t n_areas = imp->session.analysis ? imp->session.analysis->triangle_areas.size() : 0;
    const std::string id = imp->session.record.session_id;

    db.save_session_async(imp->session);
    std::string saved_id;
    wait_for([&] { return db.poll_saved(saved_id); });
    CHECK(saved_id == id, "saved id echoed");
    CHECK(std::filesystem::exists(db_path), "db file created on disk");

    db.request_session_list_async();
    std::vector<SessionRecord> list;
    wait_for([&] { return db.poll_session_list(list); });
    bool found = false;
    for (const auto& r : list) if (r.session_id == id) found = true;
    CHECK(found, "session appears in history list");

    db.request_session_async(id);
    std::shared_ptr<StoredSession> loaded;
    wait_for([&] { return db.poll_session(loaded); });
    CHECK(loaded != nullptr, "session loaded back");
    if (loaded) {
        CHECK(loaded->record.session_id == id, "loaded id");
        CHECK(loaded->record.mode == "CHF2DB_RESEARCH", "loaded mode");
        CHECK(loaded->points.size() == n_points, "loaded points count matches");
        CHECK(loaded->measurements.size() == n_meas, "loaded measurements count matches");
        CHECK(loaded->analysis.has_value(), "loaded analysis present");
        CHECK(loaded->analysis && loaded->analysis->triangle_areas.size() == n_areas,
              "loaded areas count matches");
        if (!loaded->points.empty() && !imp->session.points.empty()) {
            const double dx = std::fabs(loaded->points.front().position.x -
                                        imp->session.points.front().position.x);
            CHECK(dx < 1e-9, "loaded geometry bit-faithful");
        }
    }
    db.close();
    CHECK(!db.ready(), "db closed");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif

} // namespace

int main() {
    test_hrv_basics();
    test_importer_and_pipeline();
    test_real_chf2db_if_present();
#ifdef CARDIAC_HAVE_SQLITE3
    test_database_roundtrip();
#else
    std::cout << "(SQLite3 disabled; database round-trip test skipped)\n";
#endif
    if (failures == 0) {
        std::cout << "ALL EXTENSION TESTS PASSED (" << checks << " checks)\n";
        return 0;
    }
    std::cerr << failures << " failures / " << checks << " checks\n";
    return 1;
}
