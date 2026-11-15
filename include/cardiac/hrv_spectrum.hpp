#pragma once
// hrv_spectrum.hpp - frequency-domain HRV descriptors (LF/HF) from an IBI series.
//
// Pure, deterministic, side-effect free. Resamples accepted inter-beat
// intervals onto a uniform grid, estimates a power spectral density with a
// Welch periodogram (Hann windows + radix-2 FFT), and integrates the standard
// bands. These are research signal descriptors ONLY - not a diagnosis, not a
// disease score, and not clinical HRV interpretation.
#include "cardiac/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cardiac {

struct HrvBandInput {
  double elapsed_time_s{0.0}; // beat time relative to series start
  double interval_s{0.0};     // accepted IBI in seconds
};

struct HrvSpectrum {
  bool reliable{false};       // false => not enough clean data; do not display bands
  std::string reason;         // why it is/isn't reliable (human readable)
  double sample_rate_hz{4.0}; // resampling grid
  std::size_t resampled_points{0};
  std::size_t windows{0};     // Welch segments averaged
  double lf_power_ms2{0.0};   // 0.04-0.15 Hz
  double hf_power_ms2{0.0};   // 0.15-0.40 Hz
  double lf_hf_ratio{0.0};
  double total_power_ms2{0.0};
  double coverage_s{0.0};     // time span actually analysed
  // Optional: the averaged PSD curve for plotting (freq_hz, power_ms2/Hz).
  std::vector<float> psd_freq_hz;
  std::vector<float> psd_power;
};

struct HrvOptions {
  double resample_hz{4.0};
  double lf_low{0.04};
  double lf_high{0.15};
  double hf_low{0.15};
  double hf_high{0.40};
  int welch_segment_s{64};    // window length in seconds (rounded to pow2 bins)
  std::size_t min_beats{64};  // need at least this many accepted beats
  double min_coverage_s{90.0};// and at least this many seconds of span
  double max_gap_s{5.0};      // interpolate across gaps up to this long only
  bool keep_psd{true};
};

// Computes the spectrum from accepted IBI samples (already quality-gated and in
// chronological order). Never invents data across long gaps: if the series is
// too sparse/short, returns reliable=false with a reason and zeroed bands.
HrvSpectrum compute_hrv_spectrum(const std::vector<HrvBandInput>& ibi,
                                 const HrvOptions& opts = {});

// Convenience: pull accepted IBIs out of a normalized measurement log.
std::vector<HrvBandInput> hrv_input_from_measurements(
    const std::vector<NormalizedMeasurement>& measurements, std::size_t max_beats = 200000);

} // namespace cardiac
