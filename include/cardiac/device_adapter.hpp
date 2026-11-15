#pragma once

#include "cardiac/types.hpp"

#include <functional>

namespace cardiac {

enum class PermissionStatus { Unknown, Granted, Denied, NotRequired };
enum class DeviceEventKind { Connected, PermissionDenied, Disconnected, StreamError };
struct DeviceEvent {
    DeviceEventKind kind{DeviceEventKind::Disconnected};
    UtcTime timestamp_utc{};
};
struct DeviceCallbacks {
    std::function<void(const RawMeasurement&)> measurement;
    std::function<void(const DeviceEvent&)> event;
};

// Platform/SDK boundary. Drivers must use real device IBI/RR data, never 60/HR.
// connect() manages permissions and emits Connected before measurements; errors
// are reported as events. It may return while the connection is pending.
// disconnect() must stop/join callbacks before returning, even after link loss.
// Control methods run on one application control thread; callbacks may run on
// device threads. Drivers reconstruct beat timestamps for multi-RR packets and
// report transport loss instead of fabricating, resampling, or interpolating.
class DeviceAdapter {
public:
    virtual ~DeviceAdapter() = default;
    virtual SessionMode source_mode() const noexcept = 0;
    virtual PermissionStatus permission_status() const noexcept = 0;
    virtual void connect(DeviceCallbacks callbacks) = 0;
    virtual void disconnect() noexcept = 0;
};

} // namespace cardiac