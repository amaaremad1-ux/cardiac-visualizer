#pragma once

#include "cardiac/device_adapter.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cardiac::ble {

// Transport seam between the live adapter and the OS BLE stack.
// A WinRT/Windows.Devices.Bluetooth implementation plugs in here; the parser
// and adapter stay portable and unit-testable. bytes are raw GATT 0x2A37
// Heart Rate Measurement notifications. Implementations must invoke callbacks
// serially (or document otherwise) and call on_link_loss() exactly once when
// notifications stop unexpectedly -- never synthesize replacement samples.
class BleHrsTransport {
public:
    virtual ~BleHrsTransport() = default;
    virtual void start(std::function<void(const std::vector<std::uint8_t>& bytes,
                                         UtcTime received_utc)>
                           on_notification,
                       std::function<void()> on_link_loss) = 0;
    virtual void stop() noexcept = 0;
};

struct BleHrsAdapterOptions {
    // Human-readable label only; never exported as identity (no address/name).
    std::string sensor_label{"BLE Heart Rate Monitor"};
    SensorType sensor_type{SensorType::BleHrs};
    // Empty = connect to first HRS device; otherwise match substring. The
    // match string itself is never logged/exported.
    std::string device_name_hint;
};

// Module A live path: streams real BLE HRS RR intervals through the pipeline.
// connect() starts the transport and emits Connected; link loss emits
// Disconnected (session inserts a segment break, preserves session state).
// disconnect() stops callbacks before returning. Never fabricates samples.
class BleHrsAdapter final : public DeviceAdapter {
public:
    BleHrsAdapter(BleHrsAdapterOptions options,
                  std::shared_ptr<BleHrsTransport> transport);
    ~BleHrsAdapter() override;
    SessionMode source_mode() const noexcept override { return SessionMode::Live; }
    PermissionStatus permission_status() const noexcept override { return permission_; }
    void connect(DeviceCallbacks callbacks) override;
    void disconnect() noexcept override;
    const BleHrsAdapterOptions& options() const noexcept { return options_; }
private:
    void on_notification(const std::vector<std::uint8_t>& bytes, UtcTime received_utc);
    void on_link_loss();
    BleHrsAdapterOptions options_;
    std::shared_ptr<BleHrsTransport> transport_;
    mutable std::mutex mutex_;
    DeviceCallbacks callbacks_;
    PermissionStatus permission_{PermissionStatus::Unknown};
    bool connected_{false};
    std::thread permission_probe_;
    std::atomic<bool> stop_probe_{false};
};

} // namespace cardiac::ble
