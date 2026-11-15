#pragma once

#include "cardiac/types.hpp"

#include <unordered_set>

namespace cardiac {

class QualityController {
public:
    explicit QualityController(QualityConfig config = {});
    QualityDecision evaluate(NormalizedMeasurement measurement);
    void interrupt();
    const QualityConfig& config() const noexcept { return config_; }
private:
    QualityConfig config_;
    std::optional<UtcTime> high_watermark_;
    std::optional<UtcTime> previous_accepted_;
    std::unordered_set<std::int64_t> seen_timestamps_;
    bool interrupted_{false};
};

} // namespace cardiac