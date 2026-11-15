#include "cardiac/data_normalizer.hpp"

#include <cmath>

namespace cardiac {

NormalizedMeasurement DataNormalizer::normalize(const RawMeasurement& raw) {
    if (!origin_) {
        origin_ = raw.timestamp_utc;
    }
    NormalizedMeasurement result;
    result.timestamp_utc = raw.timestamp_utc;
    result.elapsed_time_s = std::chrono::duration<double>(raw.timestamp_utc - *origin_).count();
    result.sensor_type = raw.sensor_type;
    result.measurement_type = raw.measurement_type;
    result.heart_rate_bpm = raw.heart_rate_bpm;
    result.audit_log.push_back(AuditCode::Received);
    const auto reject = [&result](QualityFlag flag) {
        result.is_valid = false;
        result.quality_flags.push_back(flag);
    };
    if (!raw.device_valid) {
        reject(QualityFlag::DeviceInvalid);
    }
    if (raw.sensor_type == SensorType::Unknown) {
        reject(QualityFlag::UnknownSensor);
    }
    if (raw.interval) {
        double interval = *raw.interval;
        switch (raw.interval_unit) {
        case IntervalUnit::Seconds: break;
        case IntervalUnit::Milliseconds: interval /= 1000.0; break;
        case IntervalUnit::Ble1024thsOfSecond: interval /= 1024.0; break;
        }
        result.interval_s = interval;
        if (raw.interval_unit != IntervalUnit::Seconds) {
            result.audit_log.push_back(AuditCode::IntervalConvertedToSeconds);
        }
        if (!std::isfinite(interval)) {
            reject(QualityFlag::NonFiniteInterval);
        }
        if (raw.measurement_type == MeasurementType::HeartRate) {
            reject(QualityFlag::InconsistentMeasurementType);
        }
        // HR is supplemental, derived only from an actual positive interval.
        if (!result.heart_rate_bpm && std::isfinite(interval) && interval > 0.0) {
            result.heart_rate_bpm = 60.0 / interval;
            result.audit_log.push_back(AuditCode::HeartRateDerivedFromInterval);
        }
    } else {
        // Deliberately never infer an IBI from a reported heart rate.
        reject(QualityFlag::MissingInterval);
    }
    if (result.heart_rate_bpm &&
        (!std::isfinite(*result.heart_rate_bpm) || *result.heart_rate_bpm <= 0.0)) {
        reject(QualityFlag::InvalidHeartRate);
    }
    return result;
}

} // namespace cardiac