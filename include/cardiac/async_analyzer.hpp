#pragma once

#include "cardiac/types.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace cardiac {

// Module D (slow path): computes local triangle areas off the render thread.
// The renderer keeps copying bounded snapshots at 30 FPS while this worker
// processes the latest requested revision in the background. Results are
// geometric descriptors only -- never disease scores or diagnoses.
class AsyncAnalyzer {
public:
    AsyncAnalyzer();
    ~AsyncAnalyzer();
    AsyncAnalyzer(const AsyncAnalyzer&) = delete;
    AsyncAnalyzer& operator=(const AsyncAnalyzer&) = delete;
    void request(std::vector<TrajectoryPoint> points, std::uint64_t revision);
    std::optional<AnalysisResult> latest_result() const;
    void shutdown();
private:
    void worker_loop();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_{false};
    bool has_pending_{false};
    std::vector<TrajectoryPoint> pending_points_;
    std::uint64_t pending_revision_{0};
    std::optional<AnalysisResult> latest_;
};

} // namespace cardiac