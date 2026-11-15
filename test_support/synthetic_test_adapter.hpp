#pragma once

#include "cardiac/device_adapter.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace cardiac::testing {

// Explicit synthetic source for Module A testing. Emits only the scripted
// RawMeasurement list on a worker thread; it never invents extra beats to
// keep an animation alive. Live sessions must reject this adapter because
// source_mode() is SyntheticTest.
class SyntheticTestAdapter final : public DeviceAdapter {
public:
    struct Script {
        std::vector<RawMeasurement> measurements;
        bool emit_connected{true};
        bool emit_disconnected_at_end{false};
        std::uint32_t inter_sample_delay_ms{0};
        bool fail_connect{false};
    };
    explicit SyntheticTestAdapter(Script script);
    ~SyntheticTestAdapter() override;
    SessionMode source_mode() const noexcept override { return SessionMode::SyntheticTest; }
    PermissionStatus permission_status() const noexcept override { return PermissionStatus::NotRequired; }
    void connect(DeviceCallbacks callbacks) override;
    void disconnect() noexcept override;
private:
    void run_locked_callbacks();
    Script script_;
    mutable std::mutex mutex_;
    DeviceCallbacks callbacks_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    bool connected_{false};
};

// Deterministic scripted beats. base + i*step_s timestamps, caller-chosen
// intervals, optional invalid/out-of-range/timestamp mutations.
std::vector<RawMeasurement> make_script_beats(
    UtcTime base, std::size_t count, double interval_s = 0.85,
    double step_s = 0.85, SensorType sensor = SensorType::PPG,
    MeasurementType kind = MeasurementType::IBI);

} // namespace cardiac::testing