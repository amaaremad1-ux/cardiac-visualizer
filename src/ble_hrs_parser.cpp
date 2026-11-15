#include "cardiac/ble_hrs_parser.hpp"

#include <algorithm>

namespace cardiac::ble {

HrsSample parse_heart_rate_measurement(const std::uint8_t* data, std::size_t size,
                                       UtcTime received_utc, SensorType sensor) {
    HrsSample out;
    if (data == nullptr || size < 2) {
        return out; // Too short for flags + HR: report nothing, never invent.
    }
    const std::uint8_t flags = data[0];
    const bool hr_16bit = (flags & 0x01) != 0;
    const bool energy_present = (flags & 0x08) != 0;
    const bool rr_present = (flags & 0x10) != 0;
    std::size_t offset = 1;
    if (hr_16bit) {
        if (size < offset + 2) {
            return HrsSample{};
        }
        const auto raw = static_cast<std::uint16_t>(data[offset]) |
                         (static_cast<std::uint16_t>(data[offset + 1]) << 8);
        out.heart_rate_bpm = static_cast<double>(raw);
        offset += 2;
    } else {
        out.heart_rate_bpm = static_cast<double>(data[offset]);
        offset += 1;
    }
    if (energy_present) {
        if (size < offset + 2) {
            return HrsSample{};
        }
        out.energy_expended_present = true;
        offset += 2;
    }
    if (!rr_present) {
        return out; // HR-only packet: no interval samples, by contract.
    }
    std::vector<std::uint16_t> rr_raw;
    while (offset + 2 <= size) {
        const auto raw = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[offset]) |
            (static_cast<std::uint16_t>(data[offset + 1]) << 8));
        offset += 2;
        // RR=0 means "value not known" per spec: skip, never emit 0 s.
        if (raw == 0) {
            continue;
        }
        rr_raw.push_back(raw);
    }
    if (offset != size) {
        return HrsSample{}; // Trailing fragment: drop whole packet, no partial beats.
    }
    // Reconstruct beat-event times: the newest beat lands exactly at packet
    // arrival; older beats step back by the intervals between (consecutive,
    // arrival-anchored, never interpolated). rr_raw[0] is the oldest interval.
    // Emit oldest-first for in-order pipeline ingest. Raw counts are preserved
    // exactly (no float round-trip) so downstream conversion is exact.
    double newer_sum = 0.0;
    for (std::size_t i = rr_raw.size(); i-- > 0;) {
        const double rr_s = static_cast<double>(rr_raw[i]) / 1024.0;
        const double back = newer_sum; // newest beat: back == 0 -> arrival
        RawMeasurement beat;
        beat.timestamp_utc = received_utc -
                             std::chrono::duration_cast<UtcTime::duration>(
                                 std::chrono::duration<double>(back));
        beat.sensor_type = sensor;
        beat.measurement_type = MeasurementType::RRInterval;
        beat.interval = static_cast<double>(rr_raw[i]);
        beat.interval_unit = IntervalUnit::Ble1024thsOfSecond;
        beat.heart_rate_bpm = out.heart_rate_bpm;
        beat.device_valid = true;
        out.beats.push_back(beat);
        newer_sum += rr_s;
    }
    // out.beats was built newest-first; reverse to oldest-first.
    std::reverse(out.beats.begin(), out.beats.end());
    return out;
}

} // namespace cardiac::ble
