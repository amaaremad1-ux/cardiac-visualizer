#include "cardiac/types.hpp"

#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace cardiac {

UtcTime utc_now() {
    return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
}

std::string format_utc(UtcTime timestamp) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(timestamp);
    const auto fraction = std::chrono::duration_cast<std::chrono::microseconds>(timestamp - seconds).count();
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) {
        throw std::runtime_error("UTC timestamp cannot be formatted");
    }
#else
    if (gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("UTC timestamp cannot be formatted");
    }
#endif
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(6) << std::setfill('0') << fraction << 'Z';
    return output.str();
}

const char* to_string(SessionMode value) {
    switch (value) {
    case SessionMode::Live: return "LIVE";
    case SessionMode::SyntheticTest: return "SYNTHETIC_TEST";
    }
    return "UNKNOWN";
}
const char* to_string(SensorType value) {
    switch (value) {
    case SensorType::Unknown: return "UNKNOWN";
    case SensorType::ECG: return "ECG";
    case SensorType::PPG: return "PPG";
    case SensorType::BleHrs: return "BLE_HRS";
    }
    return "UNKNOWN";
}
const char* to_string(MeasurementType value) {
    switch (value) {
    case MeasurementType::IBI: return "IBI";
    case MeasurementType::RRInterval: return "RR_INTERVAL";
    case MeasurementType::HeartRate: return "HEART_RATE";
    }
    return "UNKNOWN";
}
const char* to_string(ApplicationState value) {
    switch (value) {
    case ApplicationState::DISCONNECTED: return "DISCONNECTED";
    case ApplicationState::CONNECTING: return "CONNECTING";
    case ApplicationState::COLLECTING: return "COLLECTING";
    case ApplicationState::POOR_SIGNAL: return "POOR_SIGNAL";
    case ApplicationState::BUILDING_TRAJECTORY: return "BUILDING_TRAJECTORY";
    case ApplicationState::VISUALIZATION_READY: return "VISUALIZATION_READY";
    case ApplicationState::ANALYSIS_READY: return "ANALYSIS_READY";
    case ApplicationState::PAUSED: return "PAUSED";
    case ApplicationState::STOPPED: return "STOPPED";
    }
    return "UNKNOWN";
}
const char* to_string(QualityFlag value) {
    switch (value) {
    case QualityFlag::DeviceInvalid: return "DEVICE_INVALID";
    case QualityFlag::UnknownSensor: return "UNKNOWN_SENSOR";
    case QualityFlag::MissingInterval: return "MISSING_INTERVAL";
    case QualityFlag::NonFiniteInterval: return "NON_FINITE_INTERVAL";
    case QualityFlag::IntervalOutOfBounds: return "INTERVAL_OUT_OF_BOUNDS";
    case QualityFlag::InvalidHeartRate: return "INVALID_HEART_RATE";
    case QualityFlag::InconsistentMeasurementType: return "INCONSISTENT_MEASUREMENT_TYPE";
    case QualityFlag::DuplicateTimestamp: return "DUPLICATE_TIMESTAMP";
    case QualityFlag::NonMonotonicTimestamp: return "NON_MONOTONIC_TIMESTAMP";
    case QualityFlag::GapBefore: return "GAP_BEFORE";
    case QualityFlag::StreamInterrupted: return "STREAM_INTERRUPTED";
    }
    return "UNKNOWN";
}
const char* to_string(AuditCode value) {
    switch (value) {
    case AuditCode::Received: return "RECEIVED";
    case AuditCode::IntervalConvertedToSeconds: return "INTERVAL_CONVERTED_TO_SECONDS";
    case AuditCode::HeartRateDerivedFromInterval: return "HEART_RATE_DERIVED_FROM_INTERVAL";
    case AuditCode::QualityAccepted: return "QUALITY_ACCEPTED";
    case AuditCode::QualityRejected: return "QUALITY_REJECTED";
    case AuditCode::ContinuityBroken: return "CONTINUITY_BROKEN";
    }
    return "UNKNOWN";
}
const char* to_string(SessionEventCode value) {
    switch (value) {
    case SessionEventCode::SessionCreated: return "SESSION_CREATED";
    case SessionEventCode::ConnectRequested: return "CONNECT_REQUESTED";
    case SessionEventCode::DeviceConnected: return "DEVICE_CONNECTED";
    case SessionEventCode::PermissionDenied: return "PERMISSION_DENIED";
    case SessionEventCode::DeviceDisconnected: return "DEVICE_DISCONNECTED";
    case SessionEventCode::DeviceError: return "DEVICE_ERROR";
    case SessionEventCode::ReconnectRequested: return "RECONNECT_REQUESTED";
    case SessionEventCode::StateChanged: return "STATE_CHANGED";
    case SessionEventCode::Paused: return "PAUSED";
    case SessionEventCode::Resumed: return "RESUMED";
    case SessionEventCode::Stopped: return "STOPPED";
    case SessionEventCode::MeasurementIgnoredWhileInactive: return "MEASUREMENT_IGNORED_WHILE_INACTIVE";
    case SessionEventCode::SourceModeMismatch: return "SOURCE_MODE_MISMATCH";
    case SessionEventCode::UnexpectedDeviceEvent: return "UNEXPECTED_DEVICE_EVENT";
    }
    return "UNKNOWN";
}

} // namespace cardiac