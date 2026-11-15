#include "cardiac/session_exporter.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace cardiac {
namespace {
std::string csv_cell(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}
std::string join_names(const std::vector<QualityFlag>& flags) {
    std::string out;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        if (i) {
            out += '|';
        }
        out += to_string(flags[i]);
    }
    return out;
}
std::string join_names(const std::vector<AuditCode>& codes) {
    std::string out;
    for (std::size_t i = 0; i < codes.size(); ++i) {
        if (i) {
            out += '|';
        }
        out += to_string(codes[i]);
    }
    return out;
}
std::string json_escape(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(c));
                out << std::dec << std::setfill(' ');
            } else {
                out << c;
            }
        }
    }
    out << '"';
    return out.str();
}
std::string format_double(double value) {
    if (!std::isfinite(value)) {
        return "";
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << std::fixed << value;
    std::string text = out.str();
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}
} // namespace

ExportResult SessionExporter::export_session(const SessionSnapshot& snapshot,
                                             const std::vector<TrajectoryPoint>& points,
                                             const std::optional<AnalysisResult>& analysis,
                                             const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::invalid_argument("Export directory must not be empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        throw std::runtime_error("Cannot create export directory: " + ec.message());
    }
    const std::string tag =
        snapshot.mode == SessionMode::SyntheticTest ? "SYNTHETIC_TEST" : "LIVE";
    ExportResult result;
    result.csv_path = directory / (snapshot.session_id + "_" + tag + "_measurements.csv");
    result.json_path = directory / (snapshot.session_id + "_" + tag + "_metadata.json");

    {
        std::ofstream csv(result.csv_path, std::ios::binary | std::ios::trunc);
        if (!csv) {
            throw std::runtime_error("Cannot open CSV export for writing");
        }
        csv.imbue(std::locale::classic());
        csv << "timestamp_utc,elapsed_time_s,sensor_type,measurement_type,interval_s,"
               "heart_rate_bpm,is_valid,quality_flags,audit_log\n";
        for (const auto& m : snapshot.measurements) {
            csv << csv_cell(format_utc(m.timestamp_utc)) << ','
                << format_double(m.elapsed_time_s) << ','
                << to_string(m.sensor_type) << ',' << to_string(m.measurement_type) << ','
                << (m.interval_s ? format_double(*m.interval_s) : std::string()) << ','
                << (m.heart_rate_bpm ? format_double(*m.heart_rate_bpm) : std::string()) << ','
                << (m.is_valid ? "1" : "0") << ',' << csv_cell(join_names(m.quality_flags))
                << ',' << csv_cell(join_names(m.audit_log)) << '\n';
        }
        csv.flush();
        if (!csv) {
            throw std::runtime_error("Failed while writing CSV export");
        }
    }

    {
        std::ofstream json(result.json_path, std::ios::binary | std::ios::trunc);
        if (!json) {
            throw std::runtime_error("Cannot open JSON export for writing");
        }
        json.imbue(std::locale::classic());
        json << "{\n";
        json << "  \"session_id\": " << json_escape(snapshot.session_id) << ",\n";
        json << "  \"mode\": " << json_escape(to_string(snapshot.mode)) << ",\n";
        json << "  \"disclaimer\": "
             << json_escape("Geometric visualization only; no diagnosis or disease score.") << ",\n";
        json << "  \"synthetic_data\": "
             << (snapshot.mode == SessionMode::SyntheticTest ? "true" : "false") << ",\n";
        json << "  \"started_utc\": " << json_escape(format_utc(snapshot.started_utc)) << ",\n";
        json << "  \"exported_utc\": " << json_escape(format_utc(utc_now())) << ",\n";
        json << "  \"final_state\": " << json_escape(to_string(snapshot.state)) << ",\n";
        json << "  \"interval_bounds_s\": [" << QualityConfig::min_interval_s << ", "
             << QualityConfig::max_interval_s << "],\n";
        json << "  \"max_gap_s\": " << snapshot.quality_config.max_gap_s << ",\n";
        json << "  \"beat_timestamp_tolerance_s\": "
             << snapshot.quality_config.beat_timestamp_tolerance_s << ",\n";
        json << "  \"trajectory_capacity\": " << snapshot.trajectory_capacity << ",\n";
        json << "  \"measurement_count\": " << snapshot.measurements.size() << ",\n";
        std::size_t valid = 0;
        for (const auto& m : snapshot.measurements) {
            if (m.is_valid) {
                ++valid;
            }
        }
        json << "  \"valid_count\": " << valid << ",\n";
        json << "  \"trajectory_point_count\": " << points.size() << ",\n";
        json << "  \"analysis_revision\": "
             << (analysis ? std::to_string(analysis->revision) : std::string("null")) << ",\n";
        json << "  \"triangle_area_count\": "
             << (analysis ? std::to_string(analysis->triangle_areas.size()) : std::string("0")) << ",\n";
        json << "  \"pii\": \"none; session exports exclude device/user identifiers\",\n";
        json << "  \"events\": [";
        for (std::size_t i = 0; i < snapshot.events.size(); ++i) {
            if (i) {
                json << ", ";
            }
            json << "{\"t\": " << json_escape(format_utc(snapshot.events[i].timestamp_utc))
                 << ", \"code\": " << json_escape(to_string(snapshot.events[i].code))
                 << ", \"state\": " << json_escape(to_string(snapshot.events[i].state)) << "}";
            if ((i + 1) % 4 == 0 && i + 1 < snapshot.events.size()) {
                json << "\n    ";
            }
        }
        json << "]\n}\n";
        json.flush();
        if (!json) {
            throw std::runtime_error("Failed while writing JSON export");
        }
    }
    return result;
}

} // namespace cardiac