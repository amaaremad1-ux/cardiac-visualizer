#pragma once

#include "cardiac/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cardiac {

// Step 3: local geometric + signal-quality descriptors over the rolling
// trajectory/analysis stream. Pure functions over snapshots already in memory
// (no device access, no interpolation across segment gaps). All outputs are
// geometric/signal descriptors — never diagnoses or disease scores.
struct RollingMetrics {
    std::size_t window_points{0};   // trajectory points in the window
    std::size_t window_areas{0};    // triangle areas in the window
    double area_mean_s2{0.0};
    double area_peak_s2{0.0};
    double area_min_s2{0.0};
    double area_p50_s2{0.0};        // median of window areas
    double ibi_mean_s{0.0};         // mean of X (newest IBI) in window
    double ibi_sd_s{0.0};           // population SD of X in window
    double valid_ratio{1.0};        // valid / total in quality window
    std::size_t gap_count{0};       // segment breaks visible in window
};

// Last-N rolling metrics over trajectory points + async triangle areas +
// normalized measurements. Windows are counts (not seconds) so headless tests
// and the 30 FPS UI share exact semantics. Empty input -> zero windows.
RollingMetrics compute_rolling_metrics(const std::vector<TrajectoryPoint>& points,
                                       const std::optional<AnalysisResult>& analysis,
                                       const std::vector<NormalizedMeasurement>& measurements,
                                       std::size_t area_window = 60,
                                       std::size_t quality_window = 60);

// Per-sample signal-quality lane for the timeline: 1.0 valid, 0.0 rejected,
// with the dominant flag for coloring. Gaps/breaks are explicit entries, not
// interpolated values.
struct QualitySample {
    UtcTime timestamp_utc{};
    double elapsed_time_s{0.0};
    bool is_valid{true};
    QualityFlag flag{QualityFlag::GapBefore}; // meaningful only when !is_valid
    bool has_flag{false};
    std::uint64_t segment_id{0};
};

std::vector<QualitySample> build_quality_timeline(
    const std::vector<NormalizedMeasurement>& measurements, std::size_t max_samples = 600);

// Per-area timeline entries (timestamp from the matching trajectory point).
struct AreaSample {
    UtcTime timestamp_utc{};
    double elapsed_time_s{0.0};
    double area_s2{0.0};
    std::uint64_t point_index{0};
    std::uint64_t segment_id{0};
};

std::vector<AreaSample> build_area_timeline(const std::vector<TrajectoryPoint>& points,
                                            const std::optional<AnalysisResult>& analysis,
                                            std::size_t max_samples = 600);

} // namespace cardiac
