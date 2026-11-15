#include "cardiac/ibi_csv_importer.hpp"

#include "cardiac/data_normalizer.hpp"
#include "cardiac/quality_controller.hpp"
#include "cardiac/trajectory_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cardiac {
namespace {

std::string trim_copy(std::string s) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

// Case-insensitive column matching: CHF2DB ships two header layouts
// (the full `time_seconds/ibi_seconds` export and the compact
// `beat_time_s/IBI_s` export), so keys are folded to lower-case before lookup.
std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// CHF2DB CSVs contain no quoted fields; a plain comma split is exact. A single
// trailing comma (an empty final column, e.g. a blank qc_flag) still counts as
// one field so column indices line up with the header.
std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream stream(line);
    while (std::getline(stream, field, ',')) {
        out.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        out.push_back(std::string());
    }
    return out;
}

std::optional<double> parse_optional_double(const std::string& raw) {
    const std::string text = trim_copy(raw);
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size()) {
            return std::nullopt; // trailing junk -> treat as missing
        }
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

UtcTime make_timestamp(double seconds_since_epoch) {
    const long long micros = std::llround(seconds_since_epoch * 1e6);
    return UtcTime{std::chrono::microseconds(micros)};
}

// Builds triangle areas exactly like the async worker does: neighbours that
// cross a segment boundary contribute 0 (never an interpolated area).
AnalysisResult build_analysis(const std::vector<TrajectoryPoint>& points,
                              std::uint64_t revision) {
    AnalysisResult result;
    result.revision = revision;
    if (points.size() < 3) {
        return result;
    }
    result.triangle_areas.reserve(points.size());
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        TriangleArea area;
        area.center_point_index = points[i].index;
        area.segment_id = points[i].segment_id;
        area.area_s2 = local_triangle_area(points[i - 1], points[i], points[i + 1]);
        result.triangle_areas.push_back(area);
    }
    return result;
}

} // namespace

std::optional<ImportedSession> import_ibi_csv(const std::filesystem::path& csv_path,
                                              const IbiCsvImportOptions& options,
                                              std::string* error) {
    const auto fail = [error](const std::string& message) -> std::optional<ImportedSession> {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };

    std::ifstream file(csv_path);
    if (!file) {
        return fail("Cannot open CSV: " + csv_path.string());
    }

    std::string header_line;
    if (!std::getline(file, header_line)) {
        return fail("CSV is empty (no header): " + csv_path.string());
    }
    // Strip a UTF-8 BOM if a spreadsheet tool left one behind.
    if (header_line.size() >= 3 && static_cast<unsigned char>(header_line[0]) == 0xEF &&
        static_cast<unsigned char>(header_line[1]) == 0xBB &&
        static_cast<unsigned char>(header_line[2]) == 0xBF) {
        header_line.erase(0, 3);
    }

    std::unordered_map<std::string, std::size_t> column;
    {
        const std::vector<std::string> names = split_csv(header_line);
        for (std::size_t i = 0; i < names.size(); ++i) {
            column[to_lower(trim_copy(names[i]))] = i;
        }
    }
    // Returns the index of the first alias present in the header, or npos.
    const auto find_any = [&column](std::initializer_list<const char*> aliases) -> std::size_t {
        for (const char* name : aliases) {
            const auto it = column.find(to_lower(name));
            if (it != column.end()) {
                return it->second;
            }
        }
        return static_cast<std::size_t>(-1);
    };
    const std::size_t c_record = find_any({"record"});
    const std::size_t c_time = find_any({"time_seconds", "beat_time_s"});
    const std::size_t c_ibi = find_any({"ibi_seconds", "ibi_s"});
    const std::size_t c_hr = find_any({"heart_rate_bpm"});
    if (c_time == static_cast<std::size_t>(-1) || c_ibi == static_cast<std::size_t>(-1)) {
        return fail("CSV header missing a time/IBI column (time_seconds|beat_time_s, "
                    "ibi_seconds|ibi_s): " +
                    csv_path.string());
    }

    DataNormalizer normalizer;
    QualityController quality(options.quality);
    TrajectoryEngine trajectory(options.trajectory_capacity);

    ImportedSession imported;
    std::vector<NormalizedMeasurement> measurements;
    measurements.reserve(std::min<std::size_t>(options.max_beats, 4096));

    std::size_t valid_count = 0;
    std::string line;
    while (std::getline(file, line) && imported.rows_imported < options.max_beats) {
        if (trim_copy(line).empty()) {
            continue;
        }
        ++imported.rows_read;
        const std::vector<std::string> fields = split_csv(line);
        // Only the columns we actually read must be present; a truncated or
        // blank trailing column (e.g. empty qc_flag) is tolerated.
        if (c_time >= fields.size() || c_ibi >= fields.size()) {
            ++imported.rows_skipped;
            continue;
        }
        const std::optional<double> time_s = parse_optional_double(fields[c_time]);
        if (!time_s) {
            ++imported.rows_skipped;
            continue;
        }
        const std::optional<double> ibi_s = parse_optional_double(fields[c_ibi]);
        if (!ibi_s) {
            // First beat has no previous interval (and unparsable rows): skipped,
            // never fabricated. The quality controller's gap logic will break the
            // segment when a long stretch of beats is missing.
            ++imported.rows_skipped;
            continue;
        }
        if (imported.source_record.empty() && c_record < fields.size()) {
            imported.source_record = trim_copy(fields[c_record]);
        }

        RawMeasurement raw;
        raw.timestamp_utc = make_timestamp(options.epoch_offset_s + *time_s);
        raw.sensor_type = SensorType::ECG;
        raw.measurement_type = MeasurementType::IBI;
        raw.interval = ibi_s;
        raw.interval_unit = IntervalUnit::Seconds;
        if (c_hr < fields.size()) {
            raw.heart_rate_bpm = parse_optional_double(fields[c_hr]);
        }
        raw.device_valid = true;

        // --- mirror SessionController::on_measurement exactly ---
        NormalizedMeasurement normalized = normalizer.normalize(raw);
        QualityDecision decision = quality.evaluate(std::move(normalized));
        measurements.push_back(decision.measurement);
        if (decision.measurement.is_valid) {
            ++valid_count;
        }
        if (decision.break_before) {
            trajectory.break_segment();
        }
        if (decision.measurement.is_valid && decision.measurement.interval_s) {
            trajectory.push_interval(decision.measurement.timestamp_utc,
                                     *decision.measurement.interval_s);
        }
        ++imported.rows_imported;
    }

    if (imported.rows_imported == 0) {
        return fail("No usable IBI rows found in: " + csv_path.string());
    }

    std::vector<TrajectoryPoint> points = trajectory.snapshot_all_points();
    AnalysisResult analysis = build_analysis(points, trajectory.revision());

    imported.rolling = compute_rolling_metrics(points, analysis, measurements,
                                               options.area_window, options.quality_window);
    imported.hrv = compute_hrv_spectrum(hrv_input_from_measurements(measurements));

    // --- assemble the de-identified StoredSession ---
    SessionRecord& rec = imported.session.record;
    rec.mode = "CHF2DB_RESEARCH";
    if (!imported.source_record.empty()) {
        rec.source = imported.source_record;
    } else {
        // No `record` column: fall back to the file stem, dropping a trailing
        // "_IBI" suffix so chf225_IBI.csv yields source "chf225".
        std::string stem = csv_path.stem().string();
        if (stem.size() >= 4 && to_lower(stem.substr(stem.size() - 4)) == "_ibi") {
            stem.erase(stem.size() - 4);
        }
        rec.source = stem;
    }
    rec.session_id = "chf2db-" + rec.source;
    rec.started_utc = measurements.empty() ? std::string()
                                           : format_utc(measurements.front().timestamp_utc);
    rec.saved_utc = format_utc(utc_now());
    rec.final_state = to_string(ApplicationState::ANALYSIS_READY);
    rec.measurement_count = measurements.size();
    rec.valid_count = valid_count;
    rec.trajectory_point_count = points.size();
    rec.area_mean_s2 = imported.rolling.area_mean_s2;
    rec.ibi_mean_s = imported.rolling.ibi_mean_s;
    rec.ibi_sd_s = imported.rolling.ibi_sd_s;
    rec.lf_power_ms2 = imported.hrv.reliable ? imported.hrv.lf_power_ms2 : 0.0;
    rec.hf_power_ms2 = imported.hrv.reliable ? imported.hrv.hf_power_ms2 : 0.0;
    rec.lf_hf_ratio = imported.hrv.reliable ? imported.hrv.lf_hf_ratio : 0.0;
    rec.disclaimer =
        "Geometric research visualization of de-identified PhysioNet CHF2DB IBI "
        "data. Not a diagnosis or disease score. Local SQLite storage is not a "
        "certified medical record store.";

    imported.session.points = std::move(points);
    imported.session.measurements = std::move(measurements);
    imported.session.analysis = std::move(analysis);
    return imported;
}

} // namespace cardiac
