#pragma once
// Measurements the modulation effects share (chorus, flanger, phaser,
// tremolo, vibrato, rotary), on top of effect_test_helpers.h: the pitch of a
// rendered sine cycle by cycle, the notches of a magnitude response, and the
// two-tempo check for a rate that follows the song.
#include "effects/dsp/TempoSync.h"
#include "effects/effect_test_helpers.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace modtest {

using namespace fxtest;

inline constexpr double kRate = 48000.0;

inline int samples(double seconds) { return static_cast<int>(std::lround(seconds * kRate)); }
inline int ms(double milliseconds) { return samples(milliseconds * 0.001); }

/** The frequency of a rendered sine, one value per carrier cycle, read off
    the spacing of its upward zero crossings (linearly interpolated). `time`
    is the middle of each cycle in seconds. */
struct CycleFrequencies {
  std::vector<double> time;
  std::vector<double> hz;
};

inline CycleFrequencies cycleFrequencies(const std::vector<float>& x, double fs = kRate,
                                         size_t skip = 0) {
  std::vector<double> crossings;
  for (size_t i = std::max<size_t>(skip, 1); i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double frac = static_cast<double>(-x[i - 1]) /
                          (static_cast<double>(x[i]) - static_cast<double>(x[i - 1]));
      crossings.push_back((static_cast<double>(i - 1) + frac) / fs);
    }
  CycleFrequencies out;
  for (size_t i = 1; i < crossings.size(); ++i) {
    out.time.push_back(0.5 * (crossings[i] + crossings[i - 1]));
    out.hz.push_back(1.0 / (crossings[i] - crossings[i - 1]));
  }
  return out;
}

inline double cents(double hz, double referenceHz) { return 1200.0 * std::log2(hz / referenceHz); }

struct Deviation {
  double maxCents{0.0};
  double minCents{0.0};
  double meanHz{0.0};
};

/** Highest and lowest cycle frequency between `from` and `to` seconds, in
    cents against `carrierHz`, and their mean. */
inline Deviation deviationOf(const CycleFrequencies& series, double carrierHz, double from,
                             double to) {
  Deviation d;
  d.maxCents = -1.0e9;
  d.minCents = 1.0e9;
  double sum = 0.0;
  int count = 0;
  for (size_t i = 0; i < series.hz.size(); ++i) {
    if (series.time[i] < from || series.time[i] > to)
      continue;
    const double c = cents(series.hz[i], carrierHz);
    d.maxCents = std::max(d.maxCents, c);
    d.minCents = std::min(d.minCents, c);
    sum += series.hz[i];
    ++count;
  }
  d.meanHz = count > 0 ? sum / count : 0.0;
  return d;
}

/** Frequencies of the dips in an impulse response's magnitude between
    `fromHz` and `toHz` (scanned every `stepHz`): local minima at least
    `minDepthDb` under the highest point of the response in that band. */
inline std::vector<double> responseNotches(const std::vector<float>& ir, double fromHz,
                                           double toHz, double stepHz, double minDepthDb,
                                           double fs = kRate) {
  std::vector<double> freqs;
  for (double f = fromHz; f <= toHz; f += stepHz)
    freqs.push_back(f);
  const std::vector<double> response = magnitudeResponseDb(ir, freqs, fs);
  const double top = *std::max_element(response.begin(), response.end());
  std::vector<double> notches;
  for (size_t i = 1; i + 1 < response.size(); ++i)
    if (response[i] < response[i - 1] && response[i] <= response[i + 1] &&
        response[i] < top - minDepthDb)
      notches.push_back(freqs[i]);
  return notches;
}


/** The largest sample-to-sample difference between two renders. */
inline float maxDifference(const std::vector<float>& a, const std::vector<float>& b,
                           size_t from = 0) {
  float worst = 0.0f;
  for (size_t i = from; i < std::min(a.size(), b.size()); ++i)
    worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

/** RMS of a - b relative to the RMS of b, in dB. */
inline double relativeDifferenceDb(const std::vector<float>& a, const std::vector<float>& b,
                                   size_t from = 0) {
  std::vector<float> difference(std::min(a.size(), b.size()));
  for (size_t i = 0; i < difference.size(); ++i)
    difference[i] = a[i] - b[i];
  return toDb(rmsOf(difference, from)) - toDb(rmsOf(b, from));
}

/** A parameter value on a kind run alone (DirectKind), by id. */
inline void setKindParam(DirectKind& kind, const juce::String& id, double value) {
  const int index = kind.engine().findParameter(id);
  ASSERT_GE(index, 0) << id;
  kind.engine().setParameter(index, static_cast<float>(value));
}

}  // namespace modtest
