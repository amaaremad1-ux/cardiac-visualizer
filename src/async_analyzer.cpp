#include "cardiac/async_analyzer.hpp"
#include "cardiac/trajectory_engine.hpp"

#include <utility>

namespace cardiac {

AsyncAnalyzer::AsyncAnalyzer() : worker_(&AsyncAnalyzer::worker_loop, this) {}

AsyncAnalyzer::~AsyncAnalyzer() {
    shutdown();
}

void AsyncAnalyzer::request(std::vector<TrajectoryPoint> points, std::uint64_t revision) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;
        }
        pending_points_ = std::move(points);
        pending_revision_ = revision;
        has_pending_ = true;
    }
    cv_.notify_one();
}

std::optional<AnalysisResult> AsyncAnalyzer::latest_result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void AsyncAnalyzer::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            // Join exactly once even if shutdown is called repeatedly.
        }
        stop_ = true;
        has_pending_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AsyncAnalyzer::worker_loop() {
    for (;;) {
        std::vector<TrajectoryPoint> points;
        std::uint64_t revision = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || has_pending_; });
            if (stop_) {
                return;
            }
            // Coalesce bursts: only the newest revision is analyzed.
            points = std::move(pending_points_);
            revision = pending_revision_;
            has_pending_ = false;
        }
        AnalysisResult result;
        result.revision = revision;
        if (points.size() >= 3) {
            result.triangle_areas.reserve(points.size() - 2);
            for (std::size_t i = 1; i + 1 < points.size(); ++i) {
                TriangleArea area;
                area.center_point_index = points[i].index;
                area.segment_id = points[i].segment_id;
                area.area_s2 = local_triangle_area(points[i - 1], points[i], points[i + 1]);
                result.triangle_areas.push_back(area);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latest_ || result.revision >= latest_->revision) {
                latest_ = std::move(result);
            }
        }
    }
}

} // namespace cardiac