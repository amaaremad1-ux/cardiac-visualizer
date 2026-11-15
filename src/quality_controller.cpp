#include "cardiac/quality_controller.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace cardiac {

QualityController::QualityController(QualityConfig config) : config_(config) {
    if (!std::isfinite(config_.max_gap_s) || config_.max_gap_s < QualityConfig::max_interval_s ||
        !std::isfinite(config_.beat_timestamp_tolerance_s) || config_.beat_timestamp_tolerance_s < 0.0) {
        throw std::invalid_argument("Gap limit must be >= 2 seconds; timestamp tolerance must be nonnegative");
    }
}

QualityDecision QualityController::evaluate(NormalizedMeasurement measurement) {
    QualityDecision result{std::move(measurement), false};
    auto& sample = result.measurement;
    const auto reject = [&sample](QualityFlag flag) {
        sample.is_valid = false;
        sample.quality_flags.push_back(flag);
    };
    if (!sample.interval_s) {
        if (sample.is_valid) {
            reject(QualityFlag::MissingInterval);
        }
    } else if (!std::isfinite(*sample.interval_s)) {
        if (sample.is_valid) {
            reject(QualityFlag::NonFiniteInterval);
        }
    } else if (*sample.interval_s < QualityConfig::min_interval_s ||
               *sample.interval_s > QualityConfig::max_interval_s) {
        reject(QualityFlag::IntervalOutOfBounds);
    }

    if (!seen_timestamps_.insert(sample.timestamp_utc.time_since_epoch().count()).second) {
        reject(QualityFlag::DuplicateTimestamp);
    }
    if (high_watermark_ && sample.timestamp_utc < *high_watermark_) {
        reject(QualityFlag::NonMonotonicTimestamp);
    }
    if (!high_watermark_ || sample.timestamp_utc > *high_watermark_) {
        high_watermark_ = sample.timestamp_utc;
    }

    if (interrupted_) {
        sample.quality_flags.push_back(QualityFlag::StreamInterrupted);
        result.break_before = true;
        interrupted_ = false;
    }
    if (sample.is_valid && previous_accepted_) {
        const double delta = std::chrono::duration<double>(sample.timestamp_utc - *previous_accepted_).count();
        // The second condition catches lost beats even below the absolute limit.
        if (delta > config_.max_gap_s || delta > *sample.interval_s + config_.beat_timestamp_tolerance_s) {
            sample.quality_flags.push_back(QualityFlag::GapBefore);
            result.break_before = true;
        }
    }
    if (!sample.is_valid) {
        result.break_before = true;
        previous_accepted_.reset();
    } else {
        previous_accepted_ = sample.timestamp_utc;
    }
    sample.audit_log.push_back(sample.is_valid ? AuditCode::QualityAccepted : AuditCode::QualityRejected);
    if (result.break_before) {
        sample.audit_log.push_back(AuditCode::ContinuityBroken);
    }
    return result;
}

void QualityController::interrupt() {
    previous_accepted_.reset();
    interrupted_ = true;
    // Preserve session-wide duplicate detection and the timestamp watermark.
}

} // namespace cardiac