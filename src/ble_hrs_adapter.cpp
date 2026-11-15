#include "cardiac/ble_hrs_adapter.hpp"
#include "cardiac/ble_hrs_parser.hpp"

#include <chrono>
#include <stdexcept>

namespace cardiac::ble {

BleHrsAdapter::BleHrsAdapter(BleHrsAdapterOptions options,
                             std::shared_ptr<BleHrsTransport> transport)
    : options_(std::move(options)), transport_(std::move(transport)) {
    if (!transport_) {
        throw std::invalid_argument("BLE transport must not be null");
    }
}

BleHrsAdapter::~BleHrsAdapter() {
    disconnect();
}

void BleHrsAdapter::connect(DeviceCallbacks callbacks) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            throw std::logic_error("BLE adapter already connected");
        }
        callbacks_ = std::move(callbacks);
        connected_ = true;
        permission_ = PermissionStatus::Granted; // transport owns OS consent UX
        stop_probe_ = false;
    }
    // Best-effort radio check off-thread; failures surface as events below.
    stop_probe_ = false;
    permission_probe_ = std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stop_probe_) {
            return;
        }
        DeviceCallbacks copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_) {
                return;
            }
            copy = callbacks_;
        }
        if (copy.event) {
            copy.event(DeviceEvent{DeviceEventKind::Connected, utc_now()});
        }
    });
    try {
        transport_->start(
            [this](const std::vector<std::uint8_t>& b, UtcTime t) { on_notification(b, t); },
            [this] { on_link_loss(); });
    } catch (...) {
        stop_probe_ = true;
        if (permission_probe_.joinable()) {
            permission_probe_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        permission_ = PermissionStatus::Denied;
        if (callbacks_.event) {
            callbacks_.event(DeviceEvent{DeviceEventKind::PermissionDenied, utc_now()});
        }
        throw;
    }
}

void BleHrsAdapter::disconnect() noexcept {
    stop_probe_ = true;
    if (permission_probe_.joinable() &&
        permission_probe_.get_id() != std::this_thread::get_id()) {
        permission_probe_.join();
    } else if (permission_probe_.joinable()) {
        permission_probe_.detach();
    }
    try {
        transport_->stop();
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    callbacks_ = DeviceCallbacks{};
}

void BleHrsAdapter::on_notification(const std::vector<std::uint8_t>& bytes,
                                    UtcTime received_utc) {
    DeviceCallbacks copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            return;
        }
        copy = callbacks_;
    }
    HrsSample sample = parse_heart_rate_measurement(bytes, received_utc, options_.sensor_type);
    if (!copy.measurement) {
        return;
    }
    // Oldest-first; HR-only packets yield zero beats (no 60/HR invention).
    for (const auto& beat : sample.beats) {
        copy.measurement(beat);
    }
}

void BleHrsAdapter::on_link_loss() {
    DeviceCallbacks copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            return;
        }
        connected_ = false;
        copy = callbacks_;
    }
    // SessionController moves to DISCONNECTED and breaks the segment; the
    // session object itself is preserved for reconnect().
    if (copy.event) {
        copy.event(DeviceEvent{DeviceEventKind::Disconnected, utc_now()});
    }
}

} // namespace cardiac::ble
