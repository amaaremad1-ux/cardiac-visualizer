#include "cardiac/session_controller.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace cardiac {
namespace {
std::string random_hex(std::size_t digits) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::hex << std::setfill('0');
    std::size_t emitted = 0;
    while (emitted < digits) {
        const auto value = rng();
        for (int shift = 60; shift >= 0 && emitted < digits; shift -= 4) {
            out << std::setw(1) << ((value >> shift) & 0xFULL);
            ++emitted;
        }
    }
    return out.str();
}
} // namespace

SessionController::SessionController(SessionOptions options)
    : options_(options), session_id_(make_session_id()), started_utc_(utc_now()),
      quality_(options_.quality_config), trajectory_(options_.trajectory_capacity) {
    if (options_.trajectory_capacity == 0) {
        throw std::invalid_argument("Trajectory capacity must be positive");
    }
    if (options_.visualization_min_points == 0) {
        throw std::invalid_argument("Visualization threshold must be positive");
    }
    if (options_.poor_signal_window == 0) {
        throw std::invalid_argument("Poor-signal window must be positive");
    }
    if (options_.poor_signal_invalid_ratio < 0.0 || options_.poor_signal_invalid_ratio > 1.0) {
        throw std::invalid_argument("Poor-signal ratio must be in [0,1]");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    record_event_locked(SessionEventCode::SessionCreated);
}

SessionController::~SessionController() {
    disconnect();
    analyzer_.shutdown();
}

ApplicationState SessionController::state() const {
    // Derive ANALYSIS_READY on read: the background worker may finish after the
    // last sample, with no further ingest to trigger refresh_state_locked().
    // The stored state is still persisted on the next ingest/event path.
    std::optional<AnalysisResult> latest = analyzer_.latest_result();
    std::lock_guard<std::mutex> lock(mutex_);
    if ((state_ == ApplicationState::VISUALIZATION_READY ||
         state_ == ApplicationState::ANALYSIS_READY) &&
        !paused_ && !stop_requested_ && adapter_connected_) {
        const std::uint64_t revision = trajectory_.revision();
        const bool ready = latest && latest->revision == revision &&
                           !latest->triangle_areas.empty();
        return ready ? ApplicationState::ANALYSIS_READY
                     : ApplicationState::VISUALIZATION_READY;
    }
    return state_;
}

SessionSnapshot SessionController::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionSnapshot out;
    out.session_id = session_id_;
    out.mode = options_.mode;
    out.started_utc = started_utc_;
    out.state = state_;
    out.quality_config = options_.quality_config;
    out.trajectory_capacity = options_.trajectory_capacity;
    out.measurements = measurements_;
    out.events = events_;
    return out;
}

VisualizationSnapshot SessionController::visualization_snapshot() const {
    // Copy bounded points for the render thread; full logs stay in snapshot().
    std::vector<TrajectoryPoint> points = trajectory_.snapshot_points(options_.render_max_points);
    std::optional<AnalysisResult> analysis = analyzer_.latest_result();
    std::lock_guard<std::mutex> lock(mutex_);
    VisualizationSnapshot out;
    out.state = state_;
    out.mode = options_.mode;
    out.revision = trajectory_.revision();
    out.points = std::move(points);
    out.analysis = std::move(analysis);
    return out;
}

std::size_t SessionController::valid_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_count_;
}

std::size_t SessionController::invalid_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return invalid_count_;
}

void SessionController::connect(std::shared_ptr<DeviceAdapter> adapter) {
    if (!adapter) {
        throw std::invalid_argument("Device adapter must not be null");
    }
    if (adapter->source_mode() != options_.mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        record_event_locked(SessionEventCode::SourceModeMismatch);
        throw std::invalid_argument("Adapter source mode does not match session mode");
    }
    std::shared_ptr<DeviceAdapter> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            throw std::logic_error("Session is stopped; create a new session to reconnect");
        }
        if (adapter_connected_ || state_ == ApplicationState::CONNECTING) {
            throw std::logic_error("Session is already connected/connecting");
        }
        adapter_ = adapter;
        target = adapter_;
        // Bind once; disconnect() guarantees no callback runs after it returns.
        callbacks_.measurement = [this](const RawMeasurement& raw) { on_measurement(raw); };
        callbacks_.event = [this](const DeviceEvent& event) { on_device_event(event); };
        set_state_locked(ApplicationState::CONNECTING, SessionEventCode::ConnectRequested);
    }
    try {
        target->connect(callbacks_);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        adapter_connected_ = false;
        set_state_locked(ApplicationState::DISCONNECTED, SessionEventCode::DeviceError);
        throw;
    }
}

void SessionController::reconnect() {
    std::shared_ptr<DeviceAdapter> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!adapter_) {
            throw std::logic_error("No adapter to reconnect");
        }
        if (stop_requested_ || paused_) {
            throw std::logic_error("Cannot reconnect while paused or stopped");
        }
        target = adapter_;
        record_event_locked(SessionEventCode::ReconnectRequested);
        break_continuity_locked();
        set_state_locked(ApplicationState::CONNECTING, SessionEventCode::StateChanged);
    }
    // Best effort: some transports require disconnect before a second connect.
    try {
        target->disconnect();
    } catch (...) {
        // Reconnect proceeds; the failure is already reflected as a break.
    }
    try {
        target->connect(callbacks_);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        adapter_connected_ = false;
        set_state_locked(ApplicationState::DISCONNECTED, SessionEventCode::DeviceError);
        throw;
    }
}

void SessionController::disconnect() noexcept {
    std::shared_ptr<DeviceAdapter> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = adapter_;
        adapter_connected_ = false;
        if (stop_requested_) {
            // Stay in STOPPED; still break continuity and stop callbacks.
            break_continuity_locked();
        } else if (!paused_) {
            break_continuity_locked();
            if (state_ != ApplicationState::DISCONNECTED) {
                set_state_locked(ApplicationState::DISCONNECTED, SessionEventCode::StateChanged);
            }
        } else {
            break_continuity_locked();
        }
    }
    if (target) {
        try {
            target->disconnect();
        } catch (...) {
            // disconnect() is noexcept by contract.
        }
    }
}

void SessionController::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ || stop_requested_) {
        return;
    }
    if (state_ == ApplicationState::DISCONNECTED || state_ == ApplicationState::CONNECTING) {
        throw std::logic_error("Cannot pause before streaming starts");
    }
    paused_ = true;
    break_continuity_locked();
    set_state_locked(ApplicationState::PAUSED, SessionEventCode::Paused);
}

void SessionController::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
        throw std::logic_error("Stopped sessions cannot resume");
    }
    if (!paused_) {
        return;
    }
    paused_ = false;
    record_event_locked(SessionEventCode::Resumed);
    // Fresh segment after the pause; refresh decides COLLECTING vs BUILDING.
    set_state_locked(ApplicationState::COLLECTING, SessionEventCode::StateChanged);
    refresh_state_locked();
}

void SessionController::stop() {
    std::shared_ptr<DeviceAdapter> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            return;
        }
        stop_requested_ = true;
        paused_ = false;
        target = adapter_;
        adapter_connected_ = false;
        break_continuity_locked();
        set_state_locked(ApplicationState::STOPPED, SessionEventCode::Stopped);
    }
    if (target) {
        try {
            target->disconnect();
        } catch (...) {
        }
    }
}

void SessionController::ingest(const RawMeasurement& raw) {
    on_measurement(raw);
}

void SessionController::inject_device_event(const DeviceEvent& event) {
    on_device_event(event);
}

void SessionController::on_measurement(const RawMeasurement& raw) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (paused_ || stop_requested_ || state_ == ApplicationState::DISCONNECTED ||
        state_ == ApplicationState::CONNECTING || state_ == ApplicationState::PAUSED ||
        state_ == ApplicationState::STOPPED) {
        record_event_locked(SessionEventCode::MeasurementIgnoredWhileInactive);
        return;
    }
    NormalizedMeasurement normalized = normalizer_.normalize(raw);
    QualityDecision decision = quality_.evaluate(std::move(normalized));
    measurements_.push_back(decision.measurement);
    recent_valid_.push_back(decision.measurement.is_valid);
    if (recent_valid_.size() > options_.poor_signal_window) {
        recent_valid_.erase(recent_valid_.begin());
    }
    if (decision.measurement.is_valid) {
        ++valid_count_;
    } else {
        ++invalid_count_;
    }
    if (decision.break_before) {
        trajectory_.break_segment();
    }
    bool added_point = false;
    if (decision.measurement.is_valid && decision.measurement.interval_s) {
        added_point = trajectory_.push_interval(decision.measurement.timestamp_utc,
                                               *decision.measurement.interval_s);
    }
    refresh_state_locked();
    const bool needs_analysis = added_point;
    const std::uint64_t revision = trajectory_.revision();
    lock.unlock();
    if (needs_analysis) {
        // Bounded copy for the worker; renderer keeps using snapshot_points().
        analyzer_.request(trajectory_.snapshot_all_points(), revision);
    }
}

void SessionController::on_device_event(const DeviceEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (event.kind) {
    case DeviceEventKind::Connected:
        adapter_connected_ = true;
        record_event_locked(SessionEventCode::DeviceConnected);
        if (!paused_ && !stop_requested_) {
            set_state_locked(ApplicationState::COLLECTING, SessionEventCode::StateChanged);
            refresh_state_locked();
        }
        break;
    case DeviceEventKind::PermissionDenied:
        adapter_connected_ = false;
        record_event_locked(SessionEventCode::PermissionDenied);
        if (!paused_ && !stop_requested_) {
            break_continuity_locked();
            set_state_locked(ApplicationState::DISCONNECTED, SessionEventCode::StateChanged);
        }
        break;
    case DeviceEventKind::Disconnected:
        adapter_connected_ = false;
        record_event_locked(SessionEventCode::DeviceDisconnected);
        break_continuity_locked();
        if (!paused_ && !stop_requested_) {
            set_state_locked(ApplicationState::DISCONNECTED, SessionEventCode::StateChanged);
        }
        break;
    case DeviceEventKind::StreamError:
        record_event_locked(SessionEventCode::DeviceError);
        break_continuity_locked();
        if (!paused_ && !stop_requested_ && adapter_connected_) {
            set_state_locked(ApplicationState::POOR_SIGNAL, SessionEventCode::StateChanged);
        }
        break;
    default:
        record_event_locked(SessionEventCode::UnexpectedDeviceEvent);
        break;
    }
}

void SessionController::set_state_locked(ApplicationState next, SessionEventCode code) {
    if (state_ == next) {
        // Still log explicit transitions requested by control/event paths,
        // except redundant refresh-driven no-ops handled by the caller.
        if (code == SessionEventCode::StateChanged) {
            return;
        }
    }
    state_ = next;
    SessionEvent event;
    event.timestamp_utc = utc_now();
    event.code = code;
    event.state = state_;
    events_.push_back(event);
}

void SessionController::refresh_state_locked() {
    if (paused_ || stop_requested_ || !adapter_connected_) {
        return;
    }
    if (state_ == ApplicationState::DISCONNECTED || state_ == ApplicationState::CONNECTING) {
        return;
    }
    if (!recent_valid_.empty() && recent_valid_.size() >= 5) {
        std::size_t invalid = 0;
        for (bool ok : recent_valid_) {
            if (!ok) {
                ++invalid;
            }
        }
        const double ratio =
            static_cast<double>(invalid) / static_cast<double>(recent_valid_.size());
        if (ratio >= options_.poor_signal_invalid_ratio) {
            set_state_locked(ApplicationState::POOR_SIGNAL, SessionEventCode::StateChanged);
            return;
        }
    }
    const std::size_t segment_points = trajectory_.current_segment_point_count();
    const std::size_t total_points = trajectory_.point_count();
    if (total_points == 0 && segment_points == 0) {
        set_state_locked(ApplicationState::COLLECTING, SessionEventCode::StateChanged);
        return;
    }
    if (segment_points < options_.visualization_min_points) {
        set_state_locked(ApplicationState::BUILDING_TRAJECTORY, SessionEventCode::StateChanged);
        return;
    }
    const std::optional<AnalysisResult> latest = analyzer_.latest_result();
    const std::uint64_t revision = trajectory_.revision();
    if (latest && latest->revision == revision && !latest->triangle_areas.empty()) {
        set_state_locked(ApplicationState::ANALYSIS_READY, SessionEventCode::StateChanged);
    } else {
        set_state_locked(ApplicationState::VISUALIZATION_READY, SessionEventCode::StateChanged);
    }
}

void SessionController::break_continuity_locked() {
    quality_.interrupt();
    trajectory_.break_segment();
}

void SessionController::record_event_locked(SessionEventCode code) {
    SessionEvent event;
    event.timestamp_utc = utc_now();
    event.code = code;
    event.state = state_;
    events_.push_back(event);
}

std::string SessionController::make_session_id() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "sess-" << now << "-" << random_hex(8);
    return out.str();
}

} // namespace cardiac