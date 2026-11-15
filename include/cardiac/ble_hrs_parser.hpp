#pragma once

#include "cardiac/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cardiac::ble {

// Portable Bluetooth SIG Heart Rate Measurement parser (GATT 0x2A37).
// Flags bit0: HR 8-bit vs 16-bit; bit3: energy expended present;
// bit4: RR-interval(s) present. RR units are 1/1024 s, little-endian.
// Only RR intervals become RawMeasurement samples (never 60/HR). HR without
// RR yields zero samples. Malformed/truncated input yields zero samples.
// received_utc is the packet arrival time; beat times are reconstructed as
// received_utc minus cumulative trailing RR durations (never arrival time).
struct HrsSample {
    std::vector<RawMeasurement> beats;
    std::optional<double> heart_rate_bpm;
    bool energy_expended_present{false};
};

HrsSample parse_heart_rate_measurement(const std::uint8_t* data, std::size_t size,
                                       UtcTime received_utc,
                                       SensorType sensor = SensorType::BleHrs);

inline HrsSample parse_heart_rate_measurement(const std::vector<std::uint8_t>& data,
                                              UtcTime received_utc,
                                              SensorType sensor = SensorType::BleHrs) {
    return parse_heart_rate_measurement(data.data(), data.size(), received_utc, sensor);
}

} // namespace cardiac::ble
