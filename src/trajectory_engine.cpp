#include "cardiac/trajectory_engine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cardiac {
namespace {
Vec3 subtract_vec(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
} // namespace

TrajectoryEngine::TrajectoryEngine(std::size_t capacity) {
    configure(capacity);
}

void TrajectoryEngine::configure(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("Trajectory capacity must be positive");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = capacity;
    points_.clear();
    points_.reserve(capacity_);
    pending_.clear();
    pending_time_.reset();
    next_index_ = 0;
    segment_id_ = 0;
    segment_point_count_ = 0;
    ++revision_;
}

void TrajectoryEngine::break_segment() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++segment_id_;
    segment_point_count_ = 0;
    pending_.clear();
    pending_time_.reset();
    ++revision_;
}

bool TrajectoryEngine::push_interval(UtcTime timestamp_utc, double interval_s) {
    if (!std::isfinite(interval_s)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(interval_s);
    pending_time_ = timestamp_utc;
    if (pending_.size() < 3) {
        return false;
    }
    TrajectoryPoint point;
    point.index = next_index_++;
    point.segment_id = segment_id_;
    point.timestamp_utc = *pending_time_;
    // X[i] = newest sample; the window slides forward by one interval.
    point.position = Vec3{pending_[2], pending_[1], pending_[0]};
    pending_.erase(pending_.begin());
    if (points_.size() == capacity_) {
        points_.erase(points_.begin());
    }
    points_.push_back(point);
    ++segment_point_count_;
    ++revision_;
    return true;
}

void TrajectoryEngine::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    points_.clear();
    pending_.clear();
    pending_time_.reset();
    next_index_ = 0;
    segment_id_ = 0;
    segment_point_count_ = 0;
    ++revision_;
}

std::uint64_t TrajectoryEngine::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

std::uint64_t TrajectoryEngine::segment_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return segment_id_;
}

std::size_t TrajectoryEngine::point_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return points_.size();
}

std::size_t TrajectoryEngine::current_segment_point_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return segment_point_count_;
}

std::vector<TrajectoryPoint> TrajectoryEngine::snapshot_points(std::size_t max_points) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = std::min(max_points, points_.size());
    return std::vector<TrajectoryPoint>(points_.end() - static_cast<std::ptrdiff_t>(count), points_.end());
}

std::vector<TrajectoryPoint> TrajectoryEngine::snapshot_all_points() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return points_;
}

double local_triangle_area(const TrajectoryPoint& prev,
                           const TrajectoryPoint& center,
                           const TrajectoryPoint& next) {
    if (prev.segment_id != center.segment_id || center.segment_id != next.segment_id) {
        return 0.0;
    }
    const Vec3 a = subtract_vec(center.position, prev.position);
    const Vec3 b = subtract_vec(next.position, center.position);
    const Vec3 cross{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
    return 0.5 * std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
}

} // namespace cardiac