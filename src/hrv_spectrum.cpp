// hrv_spectrum.cpp - Welch PSD + LF/HF integration over an IBI series.
#include "cardiac/hrv_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace cardiac {
namespace {

using Cd = std::complex<double>;

std::size_t next_pow2(std::size_t v) {
  std::size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

// In-place iterative radix-2 FFT. n must be a power of two.
void fft_inplace(std::vector<Cd>& a) {
  const std::size_t n = a.size();
  if (n < 2) return;
  // Bit-reversal permutation.
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
    Cd wl(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      Cd w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        Cd u = a[i + k];
        Cd v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

// Linear interpolation of the beat series at time t. Assumes beats sorted and
// t within [times.front(), times.back()].
double interp(const std::vector<double>& times, const std::vector<double>& vals, double t,
              std::size_t& hint) {
  while (hint + 1 < times.size() && times[hint + 1] < t) ++hint;
  while (hint > 0 && times[hint] > t) --hint;
  if (hint + 1 >= times.size()) return vals.back();
  double t0 = times[hint], t1 = times[hint + 1];
  double span = t1 - t0;
  if (span <= 1e-9) return vals[hint];
  double a = (t - t0) / span;
  return vals[hint] * (1.0 - a) + vals[hint + 1] * a;
}

} // namespace

std::vector<HrvBandInput> hrv_input_from_measurements(
    const std::vector<NormalizedMeasurement>& measurements, std::size_t max_beats) {
  std::vector<HrvBandInput> out;
  out.reserve(std::min(measurements.size(), max_beats));
  for (const auto& m : measurements) {
    if (out.size() >= max_beats) break;
    if (!m.is_valid || !m.interval_s) continue;
    HrvBandInput b;
    b.elapsed_time_s = m.elapsed_time_s;
    b.interval_s = *m.interval_s;
    out.push_back(b);
  }
  return out;
}

HrvSpectrum compute_hrv_spectrum(const std::vector<HrvBandInput>& ibi, const HrvOptions& opts) {
  HrvSpectrum spec;
  spec.sample_rate_hz = opts.resample_hz;

  if (ibi.size() < opts.min_beats) {
    spec.reason = "insufficient accepted beats for a reliable spectrum";
    return spec;
  }

  // Beat times relative to the first accepted beat; values in milliseconds.
  std::vector<double> times, vals;
  times.reserve(ibi.size());
  vals.reserve(ibi.size());
  const double t0 = ibi.front().elapsed_time_s;
  for (const auto& b : ibi) {
    times.push_back(b.elapsed_time_s - t0);
    vals.push_back(b.interval_s * 1000.0);
  }
  const double span = times.back() - times.front();
  if (span < opts.min_coverage_s) {
    spec.reason = "time span too short for LF/HF separation";
    return spec;
  }

  const double fs = opts.resample_hz;
  const std::size_t n = static_cast<std::size_t>(std::llround(span * fs)) + 1;
  if (n < 64) {
    spec.reason = "resampled series too short";
    return spec;
  }

  // Resample onto a uniform grid (linear interpolation between accepted beats).
  std::vector<double> grid(n);
  std::size_t hint = 0;
  for (std::size_t k = 0; k < n; ++k) {
    double t = static_cast<double>(k) / fs;
    grid[k] = interp(times, vals, t, hint);
  }

  // Coverage guard: fraction of grid points that sit near a real beat. If large
  // gaps dominate, the interpolation would be inventing data -> mark unreliable.
  std::size_t near = 0;
  {
    std::size_t j = 0;
    for (std::size_t k = 0; k < n; ++k) {
      double t = static_cast<double>(k) / fs;
      while (j + 1 < times.size() && times[j + 1] < t) ++j;
      double d0 = std::fabs(t - times[j]);
      double d1 = (j + 1 < times.size()) ? std::fabs(times[j + 1] - t) : d0;
      if (std::min(d0, d1) <= opts.max_gap_s) ++near;
    }
  }
  double coverage_ratio = static_cast<double>(near) / static_cast<double>(n);
  spec.coverage_s = span * coverage_ratio;
  if (coverage_ratio < 0.5) {
    spec.reason = "series too gappy; interpolation would fabricate intervals";
    return spec;
  }

  // Detrend (remove mean) so the DC component does not leak into LF.
  double mean = 0.0;
  for (double v : grid) mean += v;
  mean /= static_cast<double>(n);
  for (double& v : grid) v -= mean;

  // Welch parameters.
  std::size_t seg = static_cast<std::size_t>(std::llround(opts.welch_segment_s * fs));
  seg = std::min<std::size_t>(seg, n);
  std::size_t nfft = next_pow2(std::max<std::size_t>(seg, 64));
  if (nfft > n) nfft = next_pow2(std::max<std::size_t>(n / 2, 64));
  if (nfft < 64) {
    spec.reason = "window too small for the requested bands";
    return spec;
  }
  const std::size_t hop = std::max<std::size_t>(nfft / 2, 1);

  // Hann window + its power normalization.
  std::vector<double> hann(nfft);
  double wsum2 = 0.0;
  for (std::size_t i = 0; i < nfft; ++i) {
    hann[i] = 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 *
                                     static_cast<double>(i) / static_cast<double>(nfft)));
    wsum2 += hann[i] * hann[i];
  }
  const double scale = 1.0 / (fs * std::max(wsum2, 1e-12));

  const std::size_t half = nfft / 2;
  std::vector<double> psd(half + 1, 0.0);
  std::vector<Cd> buf(nfft);
  std::size_t windows = 0;
  for (std::size_t start = 0; start + nfft <= n; start += hop) {
    for (std::size_t i = 0; i < nfft; ++i) buf[i] = Cd(grid[start + i] * hann[i], 0.0);
    fft_inplace(buf);
    for (std::size_t j = 0; j <= half; ++j) {
      double mag2 = std::norm(buf[j]);
      double factor = (j == 0 || j == half) ? 1.0 : 2.0; // one-sided
      psd[j] += factor * mag2 * scale;
    }
    ++windows;
  }
  if (windows == 0) {
    spec.reason = "no complete Welch windows fit the series";
    return spec;
  }
  for (double& p : psd) p /= static_cast<double>(windows);

  // Integrate bands with the trapezoid rule over bin spacing df = fs/nfft.
  const double df = fs / static_cast<double>(nfft);
  auto band = [&](double lo, double hi) {
    double acc = 0.0;
    for (std::size_t j = 0; j <= half; ++j) {
      double f = static_cast<double>(j) * df;
      if (f >= lo && f <= hi) acc += psd[j];
    }
    return acc * df;
  };
  spec.lf_power_ms2 = band(opts.lf_low, opts.lf_high);
  spec.hf_power_ms2 = band(opts.hf_low, opts.hf_high);
  spec.total_power_ms2 = band(opts.lf_low, opts.hf_high);
  spec.lf_hf_ratio = spec.hf_power_ms2 > 1e-12 ? spec.lf_power_ms2 / spec.hf_power_ms2 : 0.0;
  spec.windows = windows;
  spec.resampled_points = n;

  if (opts.keep_psd) {
    spec.psd_freq_hz.reserve(half);
    spec.psd_power.reserve(half);
    for (std::size_t j = 1; j <= half; ++j) {
      spec.psd_freq_hz.push_back(static_cast<float>(static_cast<double>(j) * df));
      spec.psd_power.push_back(static_cast<float>(psd[j]));
    }
  }

  spec.reliable = true;
  spec.reason = "Welch PSD over accepted IBIs; research descriptor only";
  return spec;
}

} // namespace cardiac
