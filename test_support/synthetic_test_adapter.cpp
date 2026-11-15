#include "synthetic_test_adapter.hpp"

#include <chrono>
#include <stdexcept>

namespace cardiac::testing {

SyntheticTestAdapter::SyntheticTestAdapter(Script script) : script_(std::move(script)) {}

SyntheticTestAdapter::~SyntheticTestAdapter() {
    disconnect();
}

void SyntheticTestAdapter::connect(DeviceCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_) {
        throw std::logic_error("Synthetic adapter already connected");
    }
    if (script_.fail_connect) {
        throw std::runtime_error("Synthetic connect failure (test seam)");
    }
    callbacks_ = std::move(callbacks);
    stop_requested_ = false;
    connected_ = true;
    worker_ = std::thread(&SyntheticTestAdapter::run_locked_callbacks, this);
}

void SyntheticTestAdapter::disconnect() noexcept {
    stop_requested_ = true;
    std::thread local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        if (worker_.joinable()) {
            // Join outside the mutex: the worker copies callbacks under lock.
            local = std::move(worker_);
        }
        callbacks_ = DeviceCallbacks{};
    }
    if (local.joinable()) {
        if (local.get_id() == std::this_thread::get_id()) {
            local.detach();
        } else {
            local.join();
        }
    }
}

void SyntheticTestAdapter::run_locked_callbacks() {
    DeviceCallbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks = callbacks_;
    }
    if (script_.emit_connected && callbacks.event) {
        callbacks.event(DeviceEvent{DeviceEventKind::Connected, utc_now()});
    }
    for (const auto& m : script_.measurements) {
        if (stop_requested_) {
            return;
        }
        if (callbacks.measurement) {
            callbacks.measurement(m);
        }
        if (script_.inter_sample_delay_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(script_.inter_sample_delay_ms));
        }
    }
    if (script_.emit_disconnected_at_end && !stop_requested_ && callbacks.event) {
        callbacks.event(DeviceEvent{DeviceEventKind::Disconnected, utc_now()});
    }
}

std::vector<RawMeasurement> make_script_beats(UtcTime base, std::size_t count, double interval_s,
                                              double step_s, SensorType sensor,
                                              MeasurementType kind) {
    std::vector<RawMeasurement> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        RawMeasurement m;
        m.timestamp_utc =
            base + std::chrono::duration_cast<UtcTime::duration>(std::chrono::duration<double>(step_s * i));
        m.sensor_type = sensor;
        m.measurement_type = kind;
        m.interval = interval_s;
        m.interval_unit = IntervalUnit::Seconds;
        m.device_valid = true;
        out.push_back(m);
    }
    return out;
}

} // namespace cardiac::testing