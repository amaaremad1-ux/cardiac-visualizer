#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cardiac {

using UtcTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;
UtcTime utc_now();
std::string format_utc(UtcTime timestamp);

enum class SessionMode { Live, SyntheticTest };
enum class SensorType { Unknown, ECG, PPG, BleHrs };
enum class MeasurementType { IBI, RRInterval, HeartRate };
enum class IntervalUnit { Seconds, Milliseconds, Ble1024thsOfSecond };
enum class ApplicationState {
    DISCONNECTED, CONNECTING, COLLECTING, POOR_SIGNAL, BUILDING_TRAJECTORY,
    VISUALIZATION_READY, ANALYSIS_READY, PAUSED, STOPPED
};
enum class QualityFlag {
    DeviceInvalid, UnknownSensor, MissingInterval, NonFiniteInterval,
    IntervalOutOfBounds, InvalidHeartRate, InconsistentMeasurementType,
    DuplicateTimestamp, NonMonotonicTimestamp, GapBefore, StreamInterrupted
};
enum class AuditCode {
    Received, IntervalConvertedToSeconds, HeartRateDerivedFromInterval,
    QualityAccepted, QualityRejected, ContinuityBroken
};
enum class SessionEventCode {
    SessionCreated, ConnectRequested, DeviceConnected, PermissionDenied,
    DeviceDisconnected, DeviceError, ReconnectRequested, StateChanged,
    Paused, Resumed, Stopped, MeasurementIgnoredWhileInactive,
    SourceModeMismatch, UnexpectedDeviceEvent
};

const char* to_string(SessionMode value);
const char* to_string(SensorType value);
const char* to_string(MeasurementType value);
const char* to_string(ApplicationState value);
const char* to_string(QualityFlag value);
const char* to_string(AuditCode value);
const char* to_string(SessionEventCode value);

struct RawMeasurement {
    UtcTime timestamp_utc{}; // Beat-event time, not packet arrival time.
    SensorType sensor_type{SensorType::Unknown};
    MeasurementType measurement_type{MeasurementType::IBI};
    std::optional<double> interval;
    IntervalUnit interval_unit{IntervalUnit::Seconds};
    std::optional<double> heart_rate_bpm;
    bool device_valid{true};
};

struct NormalizedMeasurement {
    UtcTime timestamp_utc{};
    double elapsed_time_s{};
    SensorType sensor_type{SensorType::Unknown};
    MeasurementType measurement_type{MeasurementType::IBI};
    std::optional<double> interval_s;
    std::optional<double> heart_rate_bpm;
    bool is_valid{true};
    std::vector<QualityFlag> quality_flags;
    std::vector<AuditCode> audit_log;
};

struct QualityConfig {
    // Bounds are fixed by the requested protocol, not user-tunable filters.
    static constexpr double min_interval_s = 0.3;
    static constexpr double max_interval_s = 2.0;
    double max_gap_s{2.5};
    double beat_timestamp_tolerance_s{0.25};
};

struct QualityDecision {
    NormalizedMeasurement measurement;
    bool break_before{false};
};

struct Vec3 { double x{}, y{}, z{}; };
struct TrajectoryPoint {
    std::uint64_t index{};
    std::uint64_t segment_id{};
    UtcTime timestamp_utc{};
    Vec3 position;
};
struct TriangleArea {
    std::uint64_t center_point_index{};
    std::uint64_t segment_id{};
    double area_s2{};
};
struct AnalysisResult {
    std::uint64_t revision{};
    std::vector<TriangleArea> triangle_areas;
};
struct SessionEvent {
    UtcTime timestamp_utc{};
    SessionEventCode code{SessionEventCode::SessionCreated};
    ApplicationState state{ApplicationState::DISCONNECTED};
};

// No device address, name, account, patient field, or free-text PII is stored.
struct SessionSnapshot {
    std::string session_id;
    SessionMode mode{SessionMode::Live};
    UtcTime started_utc{};
    ApplicationState state{ApplicationState::DISCONNECTED};
    QualityConfig quality_config;
    std::size_t trajectory_capacity{};
    std::vector<NormalizedMeasurement> measurements;
    std::vector<SessionEvent> events;
};

// Small, bounded snapshot for a future renderer. Full session logs are separate.
struct VisualizationSnapshot {
    ApplicationState state{ApplicationState::DISCONNECTED};
    SessionMode mode{SessionMode::Live};
    std::uint64_t revision{};
    std::vector<TrajectoryPoint> points;
    std::optional<AnalysisResult> analysis;
};

} // namespace cardiac