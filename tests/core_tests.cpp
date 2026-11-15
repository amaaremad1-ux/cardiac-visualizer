#include "cardiac/async_analyzer.hpp"
#include "cardiac/ble_hrs_adapter.hpp"
#include "cardiac/rolling_metrics.hpp"
#include "cardiac/ble_hrs_parser.hpp"
#include "cardiac/data_normalizer.hpp"
#include "cardiac/quality_controller.hpp"
#include "cardiac/session_controller.hpp"
#include "cardiac/session_exporter.hpp"
#include "cardiac/trajectory_engine.hpp"
#include "synthetic_test_adapter.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using cardiac::UtcTime;
int failures = 0;
int checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; std::cerr << "FAIL " << msg << " [" << __LINE__ << "]\n"; } } while (0)

UtcTime base_time() {
    std::tm tm{};
    tm.tm_year = 126; tm.tm_mon = 0; tm.tm_mday = 1;
    const auto t = std::mktime(&tm);
    return cardiac::UtcTime(std::chrono::duration_cast<cardiac::UtcTime::duration>(
        std::chrono::system_clock::from_time_t(t).time_since_epoch()));
}
UtcTime add_s(UtcTime t, double s) {
    return t + std::chrono::duration_cast<UtcTime::duration>(std::chrono::duration<double>(s));
}
cardiac::RawMeasurement beat(UtcTime t, double ibi, cardiac::SensorType s = cardiac::SensorType::PPG) {
    cardiac::RawMeasurement m;
    m.timestamp_utc = t; m.sensor_type = s;
    m.measurement_type = cardiac::MeasurementType::IBI;
    m.interval = ibi; m.interval_unit = cardiac::IntervalUnit::Seconds;
    return m;
}
bool has_flag(const cardiac::NormalizedMeasurement& m, cardiac::QualityFlag f) {
    for (auto q : m.quality_flags) if (q == f) return true;
    return false;
}
void wait_for(std::function<bool()> pred) {
    for (int i = 0; i < 200 && !pred(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void test_normalizer() {
    cardiac::DataNormalizer n;
    auto m = beat(base_time(), 0.8);
    auto out = n.normalize(m);
    CHECK(out.is_valid, "normal valid");
    CHECK(out.interval_s && std::abs(*out.interval_s - 0.8) < 1e-9, "normal interval");
    CHECK(out.heart_rate_bpm && std::abs(*out.heart_rate_bpm - 75.0) < 1e-9, "hr derived 60/ibi");
    CHECK(std::abs(out.elapsed_time_s) < 1e-9, "origin elapsed 0");

    cardiac::DataNormalizer n2;
    auto ms = beat(base_time(), 850.0); ms.interval_unit = cardiac::IntervalUnit::Milliseconds;
    auto o2 = n2.normalize(ms);
    CHECK(o2.interval_s && std::abs(*o2.interval_s - 0.85) < 1e-9, "ms conversion");

    cardiac::DataNormalizer n3;
    auto ble = beat(base_time(), 870.4); ble.interval_unit = cardiac::IntervalUnit::Ble1024thsOfSecond;
    auto o3 = n3.normalize(ble);
    CHECK(o3.interval_s && std::abs(*o3.interval_s - 0.85) < 1e-6, "ble1024 conversion");

    // HR-only must never synthesize an interval.
    cardiac::DataNormalizer n4;
    cardiac::RawMeasurement hr{};
    hr.timestamp_utc = base_time(); hr.sensor_type = cardiac::SensorType::PPG;
    hr.measurement_type = cardiac::MeasurementType::HeartRate;
    hr.heart_rate_bpm = 70.0; hr.interval = std::nullopt;
    auto o4 = n4.normalize(hr);
    CHECK(!o4.is_valid && !o4.interval_s, "hr-only rejected, no invented ibi");
    CHECK(has_flag(o4, cardiac::QualityFlag::MissingInterval), "hr-only missing flag");

    cardiac::DataNormalizer n5;
    auto bad_hr = beat(base_time(), 0.8); bad_hr.heart_rate_bpm = -5.0;
    CHECK(!n5.normalize(bad_hr).is_valid, "bad hr rejected");
    cardiac::DataNormalizer n6;
    auto unk = beat(base_time(), 0.8, cardiac::SensorType::Unknown);
    CHECK(!n6.normalize(unk).is_valid, "unknown sensor rejected");
}

void test_quality_bounds() {
    for (double v : {0.3, 0.85, 2.0}) {
        cardiac::QualityController q;
        cardiac::DataNormalizer n;
        auto d = q.evaluate(n.normalize(beat(base_time(), v)));
        CHECK(d.measurement.is_valid && !d.break_before, "bounds inclusive");
    }
    for (double v : {0.299, 2.001}) {
        cardiac::QualityController q;
        cardiac::DataNormalizer n;
        auto d = q.evaluate(n.normalize(beat(base_time(), v)));
        CHECK(!d.measurement.is_valid && d.break_before, "bounds exclusive outside");
        CHECK(has_flag(d.measurement, cardiac::QualityFlag::IntervalOutOfBounds), "oob flag");
    }
    // Duplicates / monotonic / gaps / interrupt.
    {
        cardiac::QualityController q; cardiac::DataNormalizer n;
        auto t = base_time();
        auto a = q.evaluate(n.normalize(beat(t, 0.8)));
        CHECK(a.measurement.is_valid, "first ok");
        auto dup = q.evaluate(n.normalize(beat(t, 0.8)));
        CHECK(!dup.measurement.is_valid && has_flag(dup.measurement, cardiac::QualityFlag::DuplicateTimestamp), "dup");
        auto back = q.evaluate(n.normalize(beat(add_s(t, -1.0), 0.8)));
        CHECK(!back.measurement.is_valid && has_flag(back.measurement, cardiac::QualityFlag::NonMonotonicTimestamp), "nonmono");
    }
    {
        cardiac::QualityController q; cardiac::DataNormalizer n;
        auto t = base_time();
        q.evaluate(n.normalize(beat(t, 0.8)));
        auto gap = q.evaluate(n.normalize(beat(add_s(t, 5.0), 0.8)));
        CHECK(gap.break_before && has_flag(gap.measurement, cardiac::QualityFlag::GapBefore), "absolute gap");
    }
    {
        // Lost beat below absolute limit: delta 1.0 > interval 0.5 + tol 0.25.
        cardiac::QualityController q; cardiac::DataNormalizer n;
        auto t = base_time();
        q.evaluate(n.normalize(beat(t, 0.5)));
        auto gap = q.evaluate(n.normalize(beat(add_s(t, 1.0), 0.5)));
        CHECK(gap.break_before && has_flag(gap.measurement, cardiac::QualityFlag::GapBefore), "relative gap");
    }
    {
        cardiac::QualityController q; cardiac::DataNormalizer n;
        auto t = base_time();
        q.evaluate(n.normalize(beat(t, 0.8)));
        q.interrupt();
        auto nxt = q.evaluate(n.normalize(beat(add_s(t, 0.8), 0.8)));
        CHECK(nxt.break_before && has_flag(nxt.measurement, cardiac::QualityFlag::StreamInterrupted), "interrupt");
        CHECK(nxt.measurement.is_valid, "interrupt sample still valid");
    }
    bool threw = false;
    try { cardiac::QualityController q(cardiac::QualityConfig{1.0, 0.1}); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "bad gap config rejected");
}

void test_trajectory() {
    cardiac::TrajectoryEngine eng(10);
    auto t = base_time();
    CHECK(!eng.push_interval(t, 0.8), "need 3");
    CHECK(!eng.push_interval(add_s(t, 0.8), 0.9), "need 3b");
    CHECK(eng.push_interval(add_s(t, 1.7), 1.0), "third emits");
    auto pts = eng.snapshot_all_points();
    CHECK(pts.size() == 1, "one point");
    CHECK(std::abs(pts[0].position.x - 1.0) < 1e-9 && std::abs(pts[0].position.y - 0.9) < 1e-9 &&
          std::abs(pts[0].position.z - 0.8) < 1e-9, "delay embedding order X newest");
    eng.break_segment();
    CHECK(!eng.push_interval(add_s(t, 2.5), 0.8), "pending dropped after break");
    CHECK(!eng.push_interval(add_s(t, 3.3), 0.8), "pending dropped 2");
    CHECK(eng.push_interval(add_s(t, 4.1), 0.8), "fresh triple after break");
    auto all = eng.snapshot_all_points();
    CHECK(all.back().segment_id == 1, "new segment id");

    cardiac::TrajectoryPoint p0{0, 0, t, {0, 0, 0}}, p1{1, 0, t, {1, 0, 0}}, p2{2, 0, t, {1, 1, 0}};
    CHECK(std::abs(cardiac::local_triangle_area(p0, p1, p2) - 0.5) < 1e-9, "right triangle area");
    cardiac::TrajectoryPoint q2{2, 7, t, {1, 1, 0}};
    CHECK(cardiac::local_triangle_area(p0, p1, q2) == 0.0, "cross-segment zero, no interp");

    cardiac::TrajectoryEngine cap(3);
    for (int i = 0; i < 6; ++i) cap.push_interval(add_s(t, 0.8 * i), 0.8);
    CHECK(cap.point_count() == 3, "bounded capacity rolls");
}

void test_async() {
    cardiac::AsyncAnalyzer az;
    auto t = base_time();
    std::vector<cardiac::TrajectoryPoint> pts = {
        {0, 0, t, {0, 0, 0}}, {1, 0, t, {1, 0, 0}}, {2, 0, t, {1, 1, 0}}};
    az.request(pts, 42);
    wait_for([&] { auto r = az.latest_result(); return r && r->revision == 42; });
    auto r = az.latest_result();
    CHECK(r && r->triangle_areas.size() == 1, "one area");
    CHECK(r && std::abs(r->triangle_areas[0].area_s2 - 0.5) < 1e-9, "area value");
    az.shutdown();
}

void test_session_states() {
    using namespace cardiac::testing;
    auto t = base_time();
    cardiac::SessionOptions opt;
    opt.mode = cardiac::SessionMode::SyntheticTest;
    opt.visualization_min_points = 2;
    opt.poor_signal_window = 6;
    cardiac::SessionController sess(opt);
    CHECK(sess.state() == cardiac::ApplicationState::DISCONNECTED, "start disc");
    SyntheticTestAdapter::Script script;
    script.measurements = make_script_beats(t, 16, 0.8, 0.8);
    auto adapter = std::make_shared<SyntheticTestAdapter>(std::move(script));
    sess.connect(adapter);
    wait_for([&] { return sess.snapshot().measurements.size() >= 16; });
    // Let async worker catch up.
    wait_for([&] { return sess.state() == cardiac::ApplicationState::ANALYSIS_READY; });
    CHECK(sess.state() == cardiac::ApplicationState::ANALYSIS_READY, "reaches analysis ready");
    CHECK(sess.valid_count() == 16, "all valid");
    auto viz = sess.visualization_snapshot();
    CHECK(!viz.points.empty() && viz.analysis.has_value(), "viz snapshot has points+analysis");
    sess.pause();
    CHECK(sess.state() == cardiac::ApplicationState::PAUSED, "paused");
    sess.ingest(beat(add_s(t, 100.0), 0.8));
    CHECK(sess.snapshot().measurements.size() == 16, "paused ingest ignored");
    sess.resume();
    sess.ingest(beat(add_s(t, 100.0), 0.8));
    CHECK(sess.snapshot().measurements.size() == 17, "resume accepts, break not interp");
    sess.stop();
    CHECK(sess.state() == cardiac::ApplicationState::STOPPED, "stopped");
    sess.disconnect();
}

void test_poor_signal_and_gap() {
    cardiac::SessionOptions opt;
    opt.mode = cardiac::SessionMode::SyntheticTest;
    opt.poor_signal_window = 6;
    opt.poor_signal_invalid_ratio = 0.5;
    cardiac::SessionController sess(opt);
    auto t = base_time();
    sess.inject_device_event({cardiac::DeviceEventKind::Connected, t});
    for (int i = 0; i < 6; ++i) {
        auto m = beat(add_s(t, 0.8 * i), i < 4 ? 5.0 : 0.8); // 4 invalid then valid
        sess.ingest(m);
    }
    CHECK(sess.state() == cardiac::ApplicationState::POOR_SIGNAL, "poor signal on invalid burst");
    // Gap breaks segment: two triples far apart must not connect.
    cardiac::SessionController g(opt);
    g.inject_device_event({cardiac::DeviceEventKind::Connected, t});
    for (int i = 0; i < 3; ++i) g.ingest(beat(add_s(t, 0.8 * i), 0.8));
    const auto seg_before = g.visualization_snapshot().points.back().segment_id;
    g.ingest(beat(add_s(t, 20.0), 0.8));
    g.ingest(beat(add_s(t, 20.8), 0.8));
    g.ingest(beat(add_s(t, 21.6), 0.8));
    g.ingest(beat(add_s(t, 22.4), 0.8));
    auto pts = g.visualization_snapshot().points;
    CHECK(!pts.empty() && pts.back().segment_id != seg_before, "gap starts new segment");
}

void test_mode_mismatch_and_events() {
    using namespace cardiac::testing;
    cardiac::SessionOptions opt;
    opt.mode = cardiac::SessionMode::Live; // synthetic adapter must be rejected
    cardiac::SessionController sess(opt);
    SyntheticTestAdapter::Script s;
    s.measurements = make_script_beats(base_time(), 2);
    auto adapter = std::make_shared<SyntheticTestAdapter>(std::move(s));
    bool threw = false;
    try { sess.connect(adapter); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "mode mismatch rejected");

    cardiac::SessionOptions opt2;
    opt2.mode = cardiac::SessionMode::SyntheticTest;
    cardiac::SessionController s2(opt2);
    s2.inject_device_event({cardiac::DeviceEventKind::Connected, base_time()});
    CHECK(s2.state() == cardiac::ApplicationState::COLLECTING, "connected->collecting");
    s2.inject_device_event({cardiac::DeviceEventKind::StreamError, base_time()});
    CHECK(s2.state() == cardiac::ApplicationState::POOR_SIGNAL, "stream error->poor");
    s2.inject_device_event({cardiac::DeviceEventKind::Disconnected, base_time()});
    CHECK(s2.state() == cardiac::ApplicationState::DISCONNECTED, "disconnect->disconnected");
    s2.inject_device_event({cardiac::DeviceEventKind::PermissionDenied, base_time()});
    CHECK(s2.state() == cardiac::ApplicationState::DISCONNECTED, "permission denied stays disc");
}

void test_ble_hrs_parser() {
    using namespace cardiac::ble;
    auto t = base_time();
    // flags=0x10 (RR present, HR 8-bit) + HR=70 + 2x RR(1024, 870 = 1.0s, ~0.85s).
    const std::vector<std::uint8_t> pkt{0x10, 70, 0x00, 0x04, 0x66, 0x03};
    auto s = parse_heart_rate_measurement(pkt, t);
    CHECK(s.beats.size() == 2, "two rr beats");
    CHECK(s.heart_rate_bpm && std::abs(*s.heart_rate_bpm - 70.0) < 1e-9, "hr passthrough");
    CHECK(s.beats[0].measurement_type == cardiac::MeasurementType::RRInterval, "rr type");
    CHECK(s.beats[0].interval_unit == cardiac::IntervalUnit::Ble1024thsOfSecond, "ble units");
    // Newest beat lands exactly at arrival; older beats step back by the
    // intervals between (consecutive, arrival-anchored, never interpolated).
    CHECK(s.beats[1].timestamp_utc == t, "newest at arrival");
    CHECK(s.beats[0].timestamp_utc == add_s(t, -(870.0 / 1024.0)), "beat reconstruction");
    CHECK(s.beats[0].timestamp_utc < s.beats[1].timestamp_utc, "oldest first");

    // HR-only packet: zero beats, never 60/HR.
    const std::vector<std::uint8_t> hr_only{0x00, 72};
    CHECK(parse_heart_rate_measurement(hr_only, t).beats.empty(), "hr-only no beats");

    // 16-bit HR + energy expended + 1 RR.
    const std::vector<std::uint8_t> big{0x19, 0x48, 0x00, 0x10, 0x00, 0x00, 0x04};
    auto s2 = parse_heart_rate_measurement(big, t);
    CHECK(s2.beats.size() == 1 && s2.energy_expended_present, "16bit+energy parsed");
    CHECK(s2.beats[0].timestamp_utc == t, "single rr at arrival");

    // Truncated / trailing fragment: drop, never partial beats.
    CHECK(parse_heart_rate_measurement(std::vector<std::uint8_t>{0x10}, t).beats.empty(), "short dropped");
    CHECK(parse_heart_rate_measurement(std::vector<std::uint8_t>{0x10, 70, 0x00}, t).beats.empty(), "fragment dropped");
    // RR=0 (unknown per spec) skipped, never emitted as 0 s.
    CHECK(parse_heart_rate_measurement(std::vector<std::uint8_t>{0x10, 70, 0x00, 0x00}, t).beats.empty(), "rr0 skipped");

    // End-to-end: BLE units normalize to seconds through the live pipeline.
    cardiac::DataNormalizer n;
    auto normed = n.normalize(s.beats[0]);
    CHECK(normed.interval_s && std::abs(*normed.interval_s - 1.0) < 1e-9, "ble1024->s");
}

void test_ble_adapter_live() {
    using namespace cardiac::ble;
    struct FakeTransport : BleHrsTransport {
        std::function<void(const std::vector<std::uint8_t>&, cardiac::UtcTime)> notify;
        std::function<void()> lost;
        void start(std::function<void(const std::vector<std::uint8_t>&, cardiac::UtcTime)> on_n,
                   std::function<void()> on_l) override {
            notify = std::move(on_n);
            lost = std::move(on_l);
        }
        void stop() noexcept override {}
    };
    auto transport = std::make_shared<FakeTransport>();
    BleHrsAdapter adapter(BleHrsAdapterOptions{}, transport);
    CHECK(adapter.source_mode() == cardiac::SessionMode::Live, "live mode");
    std::vector<cardiac::RawMeasurement> got;
    std::vector<cardiac::DeviceEventKind> events;
    cardiac::DeviceCallbacks cb;
    cb.measurement = [&](const cardiac::RawMeasurement& m) { got.push_back(m); };
    cb.event = [&](const cardiac::DeviceEvent& e) { events.push_back(e.kind); };
    adapter.connect(cb);
    wait_for([&] { return !events.empty(); });
    CHECK(!events.empty() && events[0] == cardiac::DeviceEventKind::Connected, "connected first");
    auto t = base_time();
    transport->notify({0x10, 70, 0x00, 0x04, 0x66, 0x03}, t);
    CHECK(got.size() == 2, "rr beats streamed");
    CHECK(got[1].timestamp_utc == t, "arrival-anchored timestamps");
    transport->notify({0x00, 72}, add_s(t, 1.0)); // HR-only: nothing emitted
    CHECK(got.size() == 2, "hr-only emits nothing");
    transport->lost();
    wait_for([&] { return events.size() >= 2; });
    CHECK(events.back() == cardiac::DeviceEventKind::Disconnected, "link loss->disconnected");
    adapter.disconnect();

    // Live session accepts BLE adapter and breaks segment on disconnect.
    cardiac::SessionOptions opt;
    opt.mode = cardiac::SessionMode::Live;
    cardiac::SessionController sess(opt);
    auto t2 = std::make_shared<FakeTransport>();
    auto live = std::make_shared<BleHrsAdapter>(BleHrsAdapterOptions{}, t2);
    sess.connect(live);
    wait_for([&] { return sess.state() == cardiac::ApplicationState::COLLECTING; });
    for (int i = 0; i < 4; ++i) {
        auto base = add_s(base_time(), 0.85 * i);
        t2->notify({0x10, 70, 0xCD, 0x03}, base); // ~0.8418 s each
    }
    // Reconnect path: disconnect breaks, session object preserved. After the
    // break the fresh segment is empty again, so BUILDING (not COLLECTING).
    t2->lost();
    CHECK(sess.state() == cardiac::ApplicationState::DISCONNECTED, "live disc on link loss");
    sess.reconnect();
    wait_for([&] { return sess.state() != cardiac::ApplicationState::CONNECTING; });
    CHECK(sess.state() == cardiac::ApplicationState::BUILDING_TRAJECTORY,
          "reconnect preserves session");
    sess.stop();
    sess.disconnect();
}

void test_rolling_metrics_and_timelines() {
    auto t = base_time();
    std::vector<cardiac::TrajectoryPoint> pts;
    for (std::uint64_t i = 0; i < 5; ++i) {
        cardiac::TrajectoryPoint p;
        p.index = i;
        p.segment_id = (i < 3 ? 0 : 1);
        p.timestamp_utc = add_s(t, 0.8 * static_cast<double>(i));
        p.position = cardiac::Vec3{0.8 + 0.1 * static_cast<double>(i), 0.8, 0.8};
        pts.push_back(p);
    }
    cardiac::AnalysisResult an;
    an.revision = 1;
    for (std::uint64_t i = 1; i + 1 < 5; ++i) {
        cardiac::TriangleArea a;
        a.center_point_index = i;
        a.segment_id = pts[static_cast<std::size_t>(i)].segment_id;
        a.area_s2 = 0.001 * static_cast<double>(i);
        an.triangle_areas.push_back(a);
    }
    std::vector<cardiac::NormalizedMeasurement> ms;
    for (int i = 0; i < 6; ++i) {
        cardiac::NormalizedMeasurement m;
        m.timestamp_utc = add_s(t, 0.8 * i);
        m.elapsed_time_s = 0.8 * i;
        m.is_valid = (i % 3 != 0);
        if (!m.is_valid) {
            m.quality_flags.push_back(cardiac::QualityFlag::IntervalOutOfBounds);
        }
        ms.push_back(m);
    }
    auto rm = cardiac::compute_rolling_metrics(pts, an, ms, 60, 60);
    CHECK(rm.window_points == 5, "rm points");
    CHECK(rm.window_areas == 3, "rm areas");
    CHECK(std::abs(rm.area_mean_s2 - 0.002) < 1e-12, "rm mean");
    CHECK(std::abs(rm.area_peak_s2 - 0.003) < 1e-12, "rm peak");
    CHECK(std::abs(rm.area_min_s2 - 0.001) < 1e-12, "rm min");
    CHECK(std::abs(rm.area_p50_s2 - 0.002) < 1e-12, "rm median");
    CHECK(std::abs(rm.ibi_mean_s - 1.0) < 1e-12, "rm ibi mean");
    CHECK(rm.gap_count == 1, "rm gaps");
    CHECK(std::abs(rm.valid_ratio - 4.0 / 6.0) < 1e-12, "rm valid ratio");
    auto empty = cardiac::compute_rolling_metrics({}, std::nullopt, {}, 60, 60);
    CHECK(empty.window_points == 0 && empty.window_areas == 0 &&
              empty.valid_ratio == 1.0,
          "rm empty");
    auto win = cardiac::compute_rolling_metrics(pts, an, ms, 2, 3);
    CHECK(win.window_areas == 2, "rm area window");
    CHECK(std::abs(win.area_mean_s2 - 0.0025) < 1e-12, "rm window mean");
    CHECK(std::abs(win.valid_ratio - 2.0 / 3.0) < 1e-12, "rm quality window");

    auto qt = cardiac::build_quality_timeline(ms);
    CHECK(qt.size() == 6, "qt size");
    CHECK(!qt[0].is_valid && qt[0].has_flag, "qt invalid flag");
    CHECK(qt[1].is_valid, "qt valid");
    CHECK(cardiac::build_quality_timeline({}, 10).empty(), "qt empty");

    auto at = cardiac::build_area_timeline(pts, an);
    CHECK(at.size() == 3, "at size");
    CHECK(at[0].point_index == 1 && at[2].point_index == 3, "at mapping");
    CHECK(at[2].segment_id == 1, "at segment");
    CHECK(at[0].elapsed_time_s == 0.0 && at[2].elapsed_time_s > 0.0, "at elapsed");
    CHECK(cardiac::build_area_timeline(pts, std::nullopt).empty(), "at none");
}

void test_export() {
    cardiac::SessionOptions opt;
    opt.mode = cardiac::SessionMode::SyntheticTest;
    cardiac::SessionController sess(opt);
    auto t = base_time();
    sess.inject_device_event({cardiac::DeviceEventKind::Connected, t});
    for (int i = 0; i < 6; ++i) sess.ingest(beat(add_s(t, 0.8 * i), 0.8));
    auto snap = sess.snapshot();
    auto viz = sess.visualization_snapshot();
    cardiac::SessionExporter ex;
    const auto dir = std::filesystem::temp_directory_path() / ("cardiac_test_" + snap.session_id);
    auto res = ex.export_session(snap, viz.points, viz.analysis, dir);
    CHECK(std::filesystem::exists(res.csv_path), "csv exists");
    CHECK(std::filesystem::exists(res.json_path), "json exists");
    CHECK(res.csv_path.string().find("SYNTHETIC_TEST") != std::string::npos, "synthetic labeled");
    std::ifstream csv(res.csv_path);
    std::string header; std::getline(csv, header);
    CHECK(header.find("timestamp_utc") != std::string::npos && header.find("audit_log") != std::string::npos, "csv schema");
    std::ifstream js(res.json_path);
    std::string body((std::istreambuf_iterator<char>(js)), std::istreambuf_iterator<char>());
    CHECK(body.find("SYNTHETIC_TEST") != std::string::npos, "json mode labeled");
    CHECK(body.find("no diagnosis") != std::string::npos, "json disclaimer, no disease score");
    CHECK(body.find("device_address") == std::string::npos && body.find("patient") == std::string::npos, "no pii keys");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
} // namespace

int main() {
    test_normalizer();
    test_quality_bounds();
    test_trajectory();
    test_async();
    test_session_states();
    test_poor_signal_and_gap();
    test_mode_mismatch_and_events();
    test_ble_hrs_parser();
    test_ble_adapter_live();
    test_rolling_metrics_and_timelines();
    test_export();
    if (failures == 0) {
        std::cout << "ALL TESTS PASSED (" << checks << " checks)\n";
        return 0;
    }
    std::cerr << failures << " failures / " << checks << " checks\n";
    return 1;
}