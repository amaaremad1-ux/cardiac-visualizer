#include "cardiac/rolling_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace cardiac {
namespace {
[[maybe_unused]] double mean_of(const std::vector<double>& v) {
    if (v.empty()) {
        return 0.0;
    }
    double s = 0.0;
    for (double x : v) {
        s += x;
    }
    return s / static_cast<double>(v.size());
}
} // namespace

RollingMetrics compute_rolling_metrics(const std::vector<TrajectoryPoint>& points,
                                       const std::optional<AnalysisResult>& analysis,
                                       const std::vector<NormalizedMeasurement>& measurements,
                                       std::size_t area_window,
                                       std::size_t quality_window) {
    RollingMetrics out;
    if (!points.empty()) {
        const std::size_t n = points.size();
        out.window_points = n;
        // IBI stats over X (newest IBI per point).
        double m = 0.0;
        for (const auto& p : points) {
            m += p.position.x;
        }
        m /= static_cast<double>(n);
        out.ibi_mean_s = m;
        double v = 0.0;
        for (const auto& p : points) {
            const double d = p.position.x - m;
            v += d * d;
        }
        out.ibi_sd_s = std::sqrt(v / static_cast<double>(n));
        // Gap count = segment transitions inside the visible window.
        for (std::size_t i = 1; i < n; ++i) {
            if (points[i].segment_id != points[i - 1].segment_id) {
                ++out.gap_count;
            }
        }
    }
    if (analysis && !analysis->triangle_areas.empty() && area_window > 0) {
        const auto& all = analysis->triangle_areas;
        const std::size_t n = std::min(area_window, all.size());
        const std::size_t start = all.size() - n;
        out.window_areas = n;
        std::vector<double> vals;
        vals.reserve(n);
        double peak = 0.0;
        double mn = all[start].area_s2;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = all[start + i].area_s2;
            vals.push_back(a);
            sum += a;
            peak = std::max(peak, a);
            mn = std::min(mn, a);
        }
        out.area_mean_s2 = sum / static_cast<double>(n);
        out.area_peak_s2 = peak;
        out.area_min_s2 = mn;
        std::sort(vals.begin(), vals.end());
        out.area_p50_s2 = (n % 2 == 1) ? vals[n / 2]
                                       : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
    }
    if (!measurements.empty() && quality_window > 0) {
        const std::size_t n = std::min(quality_window, measurements.size());
        const std::size_t start = measurements.size() - n;
        std::size_t valid = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (measurements[start + i].is_valid) {
                ++valid;
            }
        }
        out.valid_ratio = static_cast<double>(valid) / static_cast<double>(n);
    }
    return out;
}

std::vector<QualitySample> build_quality_timeline(
    const std::vector<NormalizedMeasurement>& measurements, std::size_t max_samples) {
    std::vector<QualitySample> out;
    if (measurements.empty() || max_samples == 0) {
        return out;
    }
    const std::size_t n = std::min(max_samples, measurements.size());
    const std::size_t start = measurements.size() - n;
    out.reserve(n);
    std::uint64_t seg = 0;
    for (std::size_t i = start; i < measurements.size(); ++i) {
        const auto& m = measurements[i];
        // ContinuityBroken audit marks a new segment (gap/reject/pause/reconnect).
        for (const auto code : m.audit_log) {
            if (code == AuditCode::ContinuityBroken) {
                ++seg;
                break;
            }
        }
        QualitySample q;
        q.timestamp_utc = m.timestamp_utc;
        q.elapsed_time_s = m.elapsed_time_s;
        q.is_valid = m.is_valid;
        q.segment_id = seg;
        if (!m.is_valid && !m.quality_flags.empty()) {
            q.flag = m.quality_flags.front();
            q.has_flag = true;
        }
        out.push_back(q);
    }
    return out;
}

std::vector<AreaSample> build_area_timeline(const std::vector<TrajectoryPoint>& points,
                                            const std::optional<AnalysisResult>& analysis,
                                            std::size_t max_samples) {
    std::vector<AreaSample> out;
    if (!analysis || analysis->triangle_areas.empty() || max_samples == 0) {
        return out;
    }
    std::unordered_map<std::uint64_t, const TrajectoryPoint*> by_index;
    by_index.reserve(points.size() * 2);
    for (const auto& p : points) {
        by_index[p.index] = &p;
    }
    const auto& all = analysis->triangle_areas;
    const std::size_t n = std::min(max_samples, all.size());
    const std::size_t start = all.size() - n;
    out.reserve(n);
    for (std::size_t i = start; i < all.size(); ++i) {
        const auto& a = all[i];
        AreaSample s;
        s.area_s2 = a.area_s2;
        s.point_index = a.center_point_index;
        s.segment_id = a.segment_id;
        auto it = by_index.find(a.center_point_index);
        if (it != by_index.end()) {
            s.timestamp_utc = it->second->timestamp_utc;
            const double x = it->second->position.x;
            (void)x;
        }
        // elapsed is resolved by caller from session start when available;
        // default 0 keeps the function usable with points-only snapshots.
        out.push_back(s);
    }
    // Fill elapsed_time_s from timestamps relative to first sample in window.
    if (!out.empty()) {
        const UtcTime t0 = out.front().timestamp_utc;
        for (auto& s : out) {
            s.elapsed_time_s =
                std::chrono::duration<double>(s.timestamp_utc - t0).count();
        }
    }
    return out;
}

} // namespace cardiac
