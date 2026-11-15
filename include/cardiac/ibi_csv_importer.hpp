#pragma once
// ibi_csv_importer.hpp - offline import of PhysioNet CHF2DB *_IBI.csv exports.
//
// Runs a de-identified research IBI series through the SAME pipeline a live
// session uses (DataNormalizer -> QualityController -> TrajectoryEngine), so an
// imported record produces exactly the geometry, quality flags and segment
// breaks the live path would. The result is a StoredSession ready for the async
// DatabaseManager and the advanced 3D renderer, plus frequency-domain (LF/HF)
// and rolling geometric descriptors.
//
// Provenance & privacy: the input is de-identified research data derived from
// the PhysioNet Congestive Heart Failure RR Interval Database (CHF2DB). No PII
// is read or written. Outputs are geometric/statistical signal descriptors only
// - never a diagnosis, a disease score, or a clinical HRV interpretation.
#include "cardiac/database_manager.hpp"
#include "cardiac/hrv_spectrum.hpp"
#include "cardiac/rolling_metrics.hpp"
#include "cardiac/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cardiac {

struct IbiCsvImportOptions {
    // Cap beats so the DB rows and renderer buffers stay bounded and sane.
    std::size_t max_beats = 20000;
    std::size_t trajectory_capacity = 20000;
    // Reuse the live quality bounds/gap policy verbatim.
    QualityConfig quality;
    // Synthetic, de-identified timeline origin (seconds since an arbitrary
    // epoch). Only relative timing matters to the pipeline.
    double epoch_offset_s = 0.0;
    // Windows for the rolling descriptors; default covers the whole series.
    std::size_t area_window = 600;
    std::size_t quality_window = 600;
};

struct ImportedSession {
    StoredSession session;   // record + points + measurements + analysis
    HrvSpectrum hrv;         // frequency-domain descriptors (may be unreliable)
    RollingMetrics rolling;  // geometric + signal-quality descriptors
    std::size_t rows_read = 0;     // data rows consumed (excluding header)
    std::size_t rows_imported = 0; // rows turned into RawMeasurements
    std::size_t rows_skipped = 0;  // empty first-beat IBI / unparsable rows
    std::string source_record;     // e.g. "chf207"
};

// Parses one CHF2DB IBI CSV and replays it through the live pipeline. On any
// IO/parse failure returns std::nullopt and, when provided, sets *error to a
// human-readable reason. Never throws for malformed rows: they are skipped and
// counted so a single bad line cannot abort a research import.
std::optional<ImportedSession> import_ibi_csv(
    const std::filesystem::path& csv_path,
    const IbiCsvImportOptions& options = {},
    std::string* error = nullptr);

} // namespace cardiac
