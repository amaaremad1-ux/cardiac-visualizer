#pragma once

#include "cardiac/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace cardiac {

// Module D (fast path): gap-aware rolling buffer for the delay embedding
//   X[i] = IBI[i], Y[i] = IBI[i-1], Z[i] = IBI[i-2].
// Only valid, consecutive intervals in the same segment emit a point. Gaps,
// rejected samples, pause/resume and reconnects call break_segment() so the
// renderer never draws an interpolated line across missing data. Thread-safe
// for one producer and N renderer/analysis consumers. Capacity is bounded so
// the 30 FPS render loop copies at most max_points per frame.
class TrajectoryEngine {
public:
    explicit TrajectoryEngine(std::size_t capacity = 2048);
    void configure(std::size_t capacity);
    // Start a new segment. Drops the 0-2 pending IBIs so the next point needs
    // three fresh intervals. Keeps already-emitted points for the trail.
    void break_segment();
    // Push one accepted interval. Returns true when a new 3D point was added.
    bool push_interval(UtcTime timestamp_utc, double interval_s);
    void clear();
    std::size_t capacity() const noexcept { return capacity_; }
    std::uint64_t revision() const;
    std::uint64_t segment_id() const;
    std::size_t point_count() const;
    std::size_t current_segment_point_count() const;
    std::vector<TrajectoryPoint> snapshot_points(std::size_t max_points) const;
    std::vector<TrajectoryPoint> snapshot_all_points() const;
private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::vector<TrajectoryPoint> points_;
    std::vector<double> pending_;
    std::optional<UtcTime> pending_time_;
    std::uint64_t next_index_{0};
    std::uint64_t segment_id_{0};
    std::uint64_t revision_{0};
    std::size_t segment_point_count_{0};
};

// Pure geometry helper shared by the async worker and tests.
// A[i] = 0.5 * || cross(P[i]-P[i-1], P[i+1]-P[i]) ||. Returns 0 when the
// neighbours belong to different segments (a gap), never an interpolated value.
double local_triangle_area(const TrajectoryPoint& prev,
                           const TrajectoryPoint& center,
                           const TrajectoryPoint& next);

} // namespace cardiac