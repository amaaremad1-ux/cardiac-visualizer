#pragma once

#include "cardiac/async_analyzer.hpp"
#include "cardiac/data_normalizer.hpp"
#include "cardiac/device_adapter.hpp"
#include "cardiac/quality_controller.hpp"
#include "cardiac/trajectory_engine.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace cardiac {

struct SessionOptions {
    SessionMode mode{SessionMode::Live};
    QualityConfig quality_config;
    std::size_t trajectory_capacity{2048};
    // Points needed before the UI treats the trail as stable.
    std::size_t visualization_min_points{10};
    // Recent-sample window used to enter/exit POOR_SIGNAL.
    std::size_t poor_signal_window{20};
    double poor_signal_invalid_ratio{0.5};
    std::size_t render_max_points{512};
};

// Module glue + explicit application state machine. Owns the DeviceAdapter
// while connected. Control methods run on the application thread; device
// callbacks may arrive on other threads and are serialized internally.
// Live sessions never synthesize samples; SyntheticTest mode is the only path
// allowed to use SyntheticTestAdapter scripted data, and exports label it.
class SessionController {
public:
    explicit SessionController(SessionOptions options = {});
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    const std::string& session_id() const noexcept { return session_id_; }
    SessionMode mode() const noexcept { return options_.mode; }
    ApplicationState state() const;
    SessionSnapshot snapshot() const;
    VisualizationSnapshot visualization_snapshot() const;
    std::size_t valid_count() const;
    std::size_t invalid_count() const;

    void connect(std::shared_ptr<DeviceAdapter> adapter);
    void reconnect();
    void disconnect() noexcept;
    void pause();
    void resume();
    void stop();

    // Test/demo seam: injects one raw sample through the live pipeline.
    void ingest(const RawMeasurement& raw);
    void inject_device_event(const DeviceEvent& event);
private:
    void on_measurement(const RawMeasurement& raw);
    void on_device_event(const DeviceEvent& event);
    void set_state_locked(ApplicationState next, SessionEventCode code);
    void refresh_state_locked();
    void break_continuity_locked();
    void record_event_locked(SessionEventCode code);
    static std::string make_session_id();

    SessionOptions options_;
    std::string session_id_;
    UtcTime started_utc_;
    mutable std::mutex mutex_;
    ApplicationState state_{ApplicationState::DISCONNECTED};
    DataNormalizer normalizer_;
    QualityController quality_;
    TrajectoryEngine trajectory_;
    AsyncAnalyzer analyzer_;
    std::shared_ptr<DeviceAdapter> adapter_;
    DeviceCallbacks callbacks_;
    bool adapter_connected_{false};
    std::vector<NormalizedMeasurement> measurements_;
    std::vector<SessionEvent> events_;
    std::vector<bool> recent_valid_;
    std::size_t valid_count_{0};
    std::size_t invalid_count_{0};
    bool stop_requested_{false};
    bool paused_{false};
};

} // namespace cardiac