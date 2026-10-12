#include "EffectMatch.h"

#include "AudioMeasure.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <mutex>
#include <numeric>
#include <random>

namespace EffectMatch {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Positions of the signal (seconds). Segments are placed one after the other,
// each followed by its tail of silence.
constexpr double kLeadSeconds = 0.25;
constexpr double kMarkerTailSeconds = 1.0;
constexpr double kSweepSeconds = 8.0;
constexpr double kSweepTailSeconds = 4.0;
constexpr double kSineSeconds = 6.0;
constexpr double kSineTailSeconds = 1.0;
constexpr double kStaircaseStepSeconds = 0.3;
constexpr int kStaircaseSteps = 21;
constexpr double kStaircaseTailSeconds = 1.0;
constexpr double kBurstSeconds = 0.2;
constexpr double kBurstTailSeconds = 4.0;
constexpr double kSilenceSeconds = 3.0;
constexpr double kSilenceTailSeconds = 0.5;

// Rounded to a multiple of 1 / perUnit (and never -0).
double tidy(double value, double perUnit) { return std::round(value * perUnit) / perUnit + 0.0; }

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

// ####################
// SIGNAL BUILDING BLOCKS
// ####################

// Raised-cosine fade over the first and last `fade` samples.
void applyFades(std::vector<float>& x, int fade) {
  const int n = static_cast<int>(x.size());
  fade = std::min(fade, n / 2);
  for (int i = 0; i < fade; ++i) {
    const float g = static_cast<float>(0.5 - 0.5 * std::cos(kPi * (i + 0.5) / fade));
    x[static_cast<size_t>(i)] *= g;
    x[static_cast<size_t>(n - 1 - i)] *= g;
  }
}

// Exponential sine sweep with a peak of `amplitude` (Farina). The phase is
// K (exp(t / L) - 1), so the frequency rises f1 x exp(t / L).
std::vector<float> makeSweep(double rate, double f1, double f2, double seconds, double amplitude,
                             double fadeSeconds) {
  const int n = static_cast<int>(std::llround(seconds * rate));
  const double l = seconds / std::log(f2 / f1);
  const double k = kTwoPi * f1 * l;
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] =
        static_cast<float>(amplitude * std::sin(k * (std::exp(i / (rate * l)) - 1.0)));
  applyFades(x, static_cast<int>(std::llround(fadeSeconds * rate)));
  return x;
}

// The inverse filter of a sweep made by makeSweep with the same arguments:
// the sweep reversed in time, tilted by -6 dB per octave so that the
// convolution of the two is an impulse.
std::vector<float> makeInverse(const std::vector<float>& sweep, double rate, double f1, double f2,
                               double seconds) {
  const int n = static_cast<int>(sweep.size());
  const double l = seconds / std::log(f2 / f1);
  std::vector<float> inverse(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    inverse[static_cast<size_t>(i)] = static_cast<float>(
        sweep[static_cast<size_t>(n - 1 - i)] * std::exp(-i / (rate * l)));
  return inverse;
}

// Two chirps, up then down, with a gap: unlike a click it correlates sharply
// and survives a filter, a compressor or a short echo in front of it.
std::vector<float> makeMarker(double rate) {
  const int chirp = static_cast<int>(std::llround(0.05 * rate));
  const int gap = static_cast<int>(std::llround(0.02 * rate));
  std::vector<float> x(static_cast<size_t>(2 * chirp + gap), 0.0f);
  constexpr double kLowHz = 500.0, kHighHz = 8000.0;
  for (int piece = 0; piece < 2; ++piece) {
    const double from = piece == 0 ? kLowHz : kHighHz;
    const double to = piece == 0 ? kHighHz : kLowHz;
    const int offset = piece == 0 ? 0 : chirp + gap;
    const double seconds = chirp / rate;
    for (int i = 0; i < chirp; ++i) {
      const double t = i / rate;
      const double phase = kTwoPi * (from * t + (to - from) * t * t / (2.0 * seconds));
      const double window = 0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / chirp);
      x[static_cast<size_t>(offset + i)] = static_cast<float>(0.5 * window * std::sin(phase));
    }
  }
  return x;
}

std::vector<float> makeSine(double rate, double hz, double seconds, double amplitude) {
  const int n = static_cast<int>(std::llround(seconds * rate));
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(kTwoPi * hz * i / rate));
  applyFades(x, static_cast<int>(std::llround(0.05 * rate)));
  return x;
}

// A sine at 1 kHz whose amplitude follows `levelsDb` (one entry per hold),
// moving between holds with a raised-cosine ramp of `rampSamples` that starts
// at each boundary. The phase never jumps.
std::vector<float> makeLevelSteps(double rate, double hz, const std::vector<double>& levelsDb,
                                  const std::vector<int>& holdSamples, int rampSamples,
                                  int fadeSamples) {
  int total = 0;
  for (const int hold : holdSamples)
    total += hold;
  std::vector<float> x(static_cast<size_t>(total));
  int position = 0;
  double previous = 0.0;
  for (size_t step = 0; step < levelsDb.size(); ++step) {
    const double target = dbToGain(levelsDb[step]);
    for (int i = 0; i < holdSamples[step]; ++i, ++position) {
      double amplitude = target;
      if (i < rampSamples) {
        const double w = 0.5 - 0.5 * std::cos(kPi * (i + 0.5) / rampSamples);
        amplitude = previous + (target - previous) * w;
      }
      x[static_cast<size_t>(position)] =
          static_cast<float>(amplitude * std::sin(kTwoPi * hz * position / rate));
    }
    previous = target;
  }
  applyFades(x, fadeSamples);
  return x;
}

// Pink-ish noise (Paul Kellet's filter) from a fixed seed, scaled to the
// requested RMS.
std::vector<float> makePinkBurst(double rate, double seconds, double rmsDb) {
  const int n = static_cast<int>(std::llround(seconds * rate));
  std::mt19937 rng(0x50454441u);
  double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
  std::vector<double> pink(static_cast<size_t>(n));
  double sum = 0.0;
  for (int i = 0; i < n; ++i) {
    const double w = (static_cast<double>(rng() >> 8) / 16777216.0) * 2.0 - 1.0;
    b0 = 0.99886 * b0 + w * 0.0555179;
    b1 = 0.99332 * b1 + w * 0.0750759;
    b2 = 0.96900 * b2 + w * 0.1538520;
    b3 = 0.86650 * b3 + w * 0.3104856;
    b4 = 0.55000 * b4 + w * 0.5329522;
    b5 = -0.7616 * b5 - w * 0.0168980;
    const double value = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
    b6 = w * 0.115926;
    pink[static_cast<size_t>(i)] = value;
    sum += value * value;
  }
  const double scale = dbToGain(rmsDb) / std::sqrt(std::max(1.0e-12, sum / std::max(1, n)));
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = static_cast<float>(pink[static_cast<size_t>(i)] * scale);
  applyFades(x, static_cast<int>(std::llround(0.005 * rate)));
  return x;
}

// ####################
// FFT HELPERS
// ####################
int orderFor(size_t count) {
  int order = 1;
  while ((static_cast<size_t>(1) << order) < count)
    ++order;
  return order;
}

// The circular convolution (or, with `correlate`, cross-correlation) of two
// real signals, computed at a power-of-two size that holds both without
// wrapping. Convolution: result[i] = sum a[j] b[i - j]. Correlation:
// result[k] = sum a[k + j] b[j], with negative lags wrapped to the end.
std::vector<float> fftProduct(const std::vector<float>& a, const std::vector<float>& b,
                              bool correlate) {
  const int order = orderFor(a.size() + b.size());
  const size_t size = static_cast<size_t>(1) << order;
  juce::dsp::FFT fft(order);
  std::vector<float> fa(size * 2, 0.0f), fb(size * 2, 0.0f);
  std::copy(a.begin(), a.end(), fa.begin());
  std::copy(b.begin(), b.end(), fb.begin());
  fft.performRealOnlyForwardTransform(fa.data(), true);
  fft.performRealOnlyForwardTransform(fb.data(), true);
  for (size_t bin = 0; bin <= size / 2; ++bin) {
    const std::complex<double> x(fa[bin * 2], fa[bin * 2 + 1]);
    std::complex<double> y(fb[bin * 2], fb[bin * 2 + 1]);
    if (correlate)
      y = std::conj(y);
    const std::complex<double> product = x * y;
    fa[bin * 2] = static_cast<float>(product.real());
    fa[bin * 2 + 1] = static_cast<float>(product.imag());
  }
  fft.performRealOnlyInverseTransform(fa.data());
  fa.resize(size);
  return fa;
}

// Signal values from `start` for `count` samples; outside the vector, zeros.
std::vector<float> sliceOf(const std::vector<float>& x, std::int64_t start, std::int64_t count) {
  std::vector<float> out(static_cast<size_t>(std::max<std::int64_t>(0, count)), 0.0f);
  for (std::int64_t i = 0; i < count; ++i) {
    const std::int64_t at = start + i;
    if (at >= 0 && at < static_cast<std::int64_t>(x.size()))
      out[static_cast<size_t>(i)] = x[static_cast<size_t>(at)];
  }
  return out;
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty())
    return 0.0;
  const size_t index = static_cast<size_t>(
      std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1) + 0.5);
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                   values.end());
  return values[index];
}

// Peak-to-peak spread without the outliers: the 98th minus the 2nd percentile.
double spread(const std::vector<double>& values) {
  return percentile(values, 0.98) - percentile(values, 0.02);
}

double median(std::vector<double> values) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const size_t mid = values.size() / 2;
  return values.size() % 2 == 1 ? values[mid] : 0.5 * (values[mid - 1] + values[mid]);
}

// ####################
// ALIGNMENT
// ####################
struct MarkerHit {
  double position = 0.0;  // sample of the marker's first sample, fractional
  double score = 0.0;     // normalised correlation at the peak
};

// Normalised cross-correlation of `marker` against recording[from, to): the
// best position, refined to a fraction of a sample.
MarkerHit findMarker(const std::vector<float>& recording, const std::vector<float>& marker,
                     std::int64_t from, std::int64_t to) {
  const std::int64_t total = static_cast<std::int64_t>(recording.size());
  from = std::clamp<std::int64_t>(from, 0, total);
  to = std::clamp<std::int64_t>(to, 0, total);
  const std::int64_t markerLength = static_cast<std::int64_t>(marker.size());
  if (to - from < markerLength)
    return {};

  const std::vector<float> region = sliceOf(recording, from, to - from);
  const std::vector<float> correlation = fftProduct(region, marker, true);

  double markerEnergy = 0.0;
  for (const float v : marker)
    markerEnergy += static_cast<double>(v) * v;

  // Running energy of the region under the marker.
  std::vector<double> prefix(region.size() + 1, 0.0);
  for (size_t i = 0; i < region.size(); ++i)
    prefix[i + 1] = prefix[i] + static_cast<double>(region[i]) * region[i];

  const std::int64_t positions = static_cast<std::int64_t>(region.size()) - markerLength + 1;
  std::vector<double> score(static_cast<size_t>(positions), 0.0);
  std::int64_t best = 0;
  for (std::int64_t k = 0; k < positions; ++k) {
    const double energy = prefix[static_cast<size_t>(k + markerLength)] - prefix[static_cast<size_t>(k)];
    const double denominator = std::sqrt(markerEnergy * std::max(energy, 1.0e-9));
    // The sign is the polarity of the path: a unit may invert.
    score[static_cast<size_t>(k)] = std::abs(correlation[static_cast<size_t>(k)]) / denominator;
    if (score[static_cast<size_t>(k)] > score[static_cast<size_t>(best)])
      best = k;
  }

  // The score ignores level, so an echo of the marker matches as well as the
  // marker itself: take the earliest peak that is nearly as good as the best.
  const double bestScore = score[static_cast<size_t>(best)];
  for (std::int64_t k = 0; k < best; ++k) {
    const double here = score[static_cast<size_t>(k)];
    if (here >= 0.9 * bestScore && (k == 0 || here >= score[static_cast<size_t>(k - 1)]) &&
        here >= score[static_cast<size_t>(k + 1)]) {
      best = k;
      break;
    }
  }

  MarkerHit hit;
  hit.score = score[static_cast<size_t>(best)];
  double offset = 0.0;
  if (best > 0 && best + 1 < positions) {
    const double left = score[static_cast<size_t>(best - 1)];
    const double mid = score[static_cast<size_t>(best)];
    const double right = score[static_cast<size_t>(best + 1)];
    const double curve = left - 2.0 * mid + right;
    if (curve < -1.0e-12)
      offset = std::clamp(0.5 * (left - right) / curve, -0.5, 0.5);
  }
  hit.position = static_cast<double>(from + best) + offset;
  return hit;
}

// Where the sent signal's positions land in the recording.
struct Mapper {
  double originRecording = 0.0;
  double originNominal = 0.0;
  double scale = 1.0;
  double at(double nominal) const { return originRecording + (nominal - originNominal) * scale; }
  std::int64_t sliceStart(double nominal, double extraBefore = 0.0) const {
    return static_cast<std::int64_t>(std::llround(at(nominal) - extraBefore));
  }
};

// ####################
// SWEEP ANALYSIS
// ####################
constexpr double kIrKeepSeconds = 0.01;       // kept before the zero lag
constexpr double kSweepPreSeconds = 0.1;      // slice start before the sweep
constexpr double kEarlyPreSeconds = 0.005;    // early window before the direct sound
constexpr double kEarlySeconds = 0.1;         // and after it
constexpr double kEnvelopeSeconds = 0.001;
constexpr double kEchoRadiusSeconds = 0.005;
constexpr double kEchoWindowSeconds = 0.002;
constexpr double kEchoFloorDb = -40.0;
constexpr int kMaxEchoes = 8;

struct SweepKit {
  double f1 = 20.0, f2 = 20000.0, seconds = kSweepSeconds;
  std::vector<float> inverse;
  // The inverse filter's spectrum at the size every sweep slice needs, so
  // that each recorded sweep costs two transforms instead of three.
  int sliceSamples = 0;
  int order = 0;
  std::vector<float> inverseSpectrum;
  // The unit sweep played back unprocessed: its impulse response window.
  std::vector<float> referenceIr;
  int referenceDirect = 0;
  std::array<double, kNumBands> referencePower{};
};

// Mean power per bin inside each 1/3-octave band of the early response: the
// impulse response from 5 ms before `direct` to 100 ms after it, with short
// fades at both ends.
std::array<double, kNumBands> earlyBandPower(const std::vector<float>& ir, int direct,
                                             double rate) {
  const int pre = static_cast<int>(std::llround(kEarlyPreSeconds * rate));
  const int length = pre + static_cast<int>(std::llround(kEarlySeconds * rate));
  const int fadeIn = static_cast<int>(std::llround(0.001 * rate));
  const int fadeOut = static_cast<int>(std::llround(0.02 * rate));
  const int order = 16;
  const size_t size = static_cast<size_t>(1) << order;
  std::vector<float> data(size * 2, 0.0f);
  for (int i = 0; i < length; ++i) {
    const int at = direct - pre + i;
    if (at < 0 || at >= static_cast<int>(ir.size()))
      continue;
    double window = 1.0;
    if (i < fadeIn)
      window = 0.5 - 0.5 * std::cos(kPi * (i + 0.5) / fadeIn);
    if (i >= length - fadeOut)
      window = std::min(window, 0.5 - 0.5 * std::cos(kPi * (length - i - 0.5) / fadeOut));
    data[static_cast<size_t>(i)] = static_cast<float>(ir[static_cast<size_t>(at)] * window);
  }
  juce::dsp::FFT fft(order);
  fft.performRealOnlyForwardTransform(data.data(), true);

  std::array<double, kNumBands> power{};
  const double binHz = rate / static_cast<double>(size);
  const double edge = std::pow(2.0, 1.0 / 6.0);
  for (int band = 0; band < kNumBands; ++band) {
    const double low = kBandCentresHz[static_cast<size_t>(band)] / edge;
    const double high = kBandCentresHz[static_cast<size_t>(band)] * edge;
    const size_t first = static_cast<size_t>(std::ceil(low / binHz));
    const size_t last = std::min(size / 2, static_cast<size_t>(std::floor(high / binHz)));
    double sum = 0.0;
    size_t count = 0;
    for (size_t bin = first; bin <= last; ++bin, ++count)
      sum += static_cast<double>(data[bin * 2]) * data[bin * 2] +
             static_cast<double>(data[bin * 2 + 1]) * data[bin * 2 + 1];
    power[static_cast<size_t>(band)] = count > 0 ? sum / static_cast<double>(count) : 0.0;
  }
  return power;
}

SweepKit makeSweepKit(double rate) {
  SweepKit kit;
  const std::vector<float> sweep = makeSweep(rate, kit.f1, kit.f2, kit.seconds, 1.0, 0.01);
  kit.inverse = makeInverse(sweep, rate, kit.f1, kit.f2, kit.seconds);
  const std::vector<float> response = fftProduct(sweep, kit.inverse, false);

  kit.sliceSamples = static_cast<int>(std::llround(kSweepPreSeconds * rate)) +
                     static_cast<int>(sweep.size()) +
                     static_cast<int>(std::llround(kSweepTailSeconds * rate));
  kit.order = orderFor(static_cast<size_t>(kit.sliceSamples) + kit.inverse.size());
  {
    juce::dsp::FFT fft(kit.order);
    kit.inverseSpectrum.assign((static_cast<size_t>(1) << kit.order) * 2, 0.0f);
    std::copy(kit.inverse.begin(), kit.inverse.end(), kit.inverseSpectrum.begin());
    fft.performRealOnlyForwardTransform(kit.inverseSpectrum.data(), true);
  }

  // Zero lag sits at sweep.size() - 1.
  const int zero = static_cast<int>(sweep.size()) - 1;
  const int keep = static_cast<int>(std::llround(kIrKeepSeconds * rate));
  const int length = keep + static_cast<int>(std::llround(0.2 * rate));
  kit.referenceIr.assign(response.begin() + (zero - keep), response.begin() + (zero - keep + length));
  int peak = 0;
  for (int i = 0; i < length; ++i)
    if (std::abs(kit.referenceIr[static_cast<size_t>(i)]) >
        std::abs(kit.referenceIr[static_cast<size_t>(peak)]))
      peak = i;
  kit.referenceDirect = peak;
  kit.referencePower = earlyBandPower(kit.referenceIr, peak, rate);
  return kit;
}

// The deconvolution of a recorded sweep slice: the slice convolved with the
// inverse filter.
std::vector<float> deconvolve(const SweepKit& kit, const std::vector<float>& slice) {
  if (static_cast<int>(slice.size()) != kit.sliceSamples)
    return fftProduct(slice, kit.inverse, false);
  const size_t size = static_cast<size_t>(1) << kit.order;
  juce::dsp::FFT fft(kit.order);
  std::vector<float> data(size * 2, 0.0f);
  std::copy(slice.begin(), slice.end(), data.begin());
  fft.performRealOnlyForwardTransform(data.data(), true);
  for (size_t bin = 0; bin <= size / 2; ++bin) {
    const std::complex<double> product(data[bin * 2], data[bin * 2 + 1]);
    const std::complex<double> filter(kit.inverseSpectrum[bin * 2], kit.inverseSpectrum[bin * 2 + 1]);
    const std::complex<double> result = product * filter;
    data[bin * 2] = static_cast<float>(result.real());
    data[bin * 2 + 1] = static_cast<float>(result.imag());
  }
  fft.performRealOnlyInverseTransform(data.data());
  data.resize(size);
  return data;
}

// The kit for a sample rate, made once.
const SweepKit& sweepKitFor(double rate) {
  static std::mutex lock;
  static std::map<double, SweepKit> kits;
  const std::lock_guard<std::mutex> guard(lock);
  auto found = kits.find(rate);
  if (found == kits.end())
    found = kits.emplace(rate, makeSweepKit(rate)).first;
  return found->second;
}

// 1 ms RMS envelope of an impulse response (amplitude, not dB).
std::vector<double> irEnvelope(const std::vector<float>& ir, double rate) {
  const int window = std::max(1, static_cast<int>(std::llround(kEnvelopeSeconds * rate)));
  std::vector<double> prefix(ir.size() + 1, 0.0);
  for (size_t i = 0; i < ir.size(); ++i)
    prefix[i + 1] = prefix[i] + static_cast<double>(ir[i]) * ir[i];
  std::vector<double> envelope(ir.size(), 0.0);
  const int half = window / 2;
  for (int i = 0; i < static_cast<int>(ir.size()); ++i) {
    const int from = std::max(0, i - half);
    const int to = std::min(static_cast<int>(ir.size()), i - half + window);
    if (to > from)
      envelope[static_cast<size_t>(i)] =
          std::sqrt((prefix[static_cast<size_t>(to)] - prefix[static_cast<size_t>(from)]) /
                    (to - from));
  }
  return envelope;
}

// Energy-based decay fit. `energy` holds the squared signal per sample from
// the start of the decay. The Schroeder integral is taken up to where the
// signal meets the noise (the median of the last fifth), and a reverberation
// time is read from the slope between two levels of it. Unset when the decay
// does not drop far enough above the noise.
struct DecayFit {
  std::optional<double> edt;
  std::optional<double> t30;
  std::optional<double> t20;
};

DecayFit fitDecay(const std::vector<double>& energy, double rate) {
  DecayFit fit;
  const int block = std::max(1, static_cast<int>(std::llround(0.02 * rate)));
  const int blocks = static_cast<int>(energy.size()) / block;
  if (blocks < 8)
    return fit;
  std::vector<double> blockEnergy(static_cast<size_t>(blocks), 0.0);
  for (int b = 0; b < blocks; ++b)
    for (int i = 0; i < block; ++i)
      blockEnergy[static_cast<size_t>(b)] += energy[static_cast<size_t>(b * block + i)];
  const int peakBlock = static_cast<int>(
      std::max_element(blockEnergy.begin(), blockEnergy.end()) - blockEnergy.begin());
  const double peak = blockEnergy[static_cast<size_t>(peakBlock)];
  if (peak <= 1.0e-18)
    return fit;
  const int tailFrom = blocks - std::max(2, blocks / 5);
  const double noise = median(std::vector<double>(blockEnergy.begin() + tailFrom, blockEnergy.end()));
  // The usable dynamic range, in dB, between the loudest block and the noise.
  const double range = 10.0 * std::log10(peak / std::max(noise, 1.0e-30));

  // Cut where the decay first meets twice the noise.
  int cut = blocks;
  for (int b = peakBlock; b < blocks; ++b)
    if (blockEnergy[static_cast<size_t>(b)] <= 2.0 * noise) {
      cut = b;
      break;
    }
  const int end = std::max(1, cut * block);
  std::vector<double> curve(static_cast<size_t>(end), 0.0);
  double running = 0.0;
  for (int i = end - 1; i >= 0; --i) {
    running += energy[static_cast<size_t>(i)];
    curve[static_cast<size_t>(i)] = running;
  }
  const double total = curve[0];
  if (total <= 0.0)
    return fit;

  // Least-squares slope of the curve in dB between two levels.
  const auto reverbTime = [&](double fromDb, double toDb,
                              double neededRangeDb) -> std::optional<double> {
    // The noise must sit well under the lower level, or the curve's bend
    // towards the cut would be read as decay.
    if (range < neededRangeDb)
      return std::nullopt;
    int first = -1, last = -1;
    for (int i = 0; i < end; ++i) {
      const double db = 10.0 * std::log10(std::max(curve[static_cast<size_t>(i)], 1.0e-30) / total);
      if (first < 0 && db <= fromDb)
        first = i;
      if (db <= toDb) {
        last = i;
        break;
      }
    }
    // The curve must come within sight of the lower level before the cut.
    if (first < 0 || last <= first + 1 || last >= end - 1)
      return std::nullopt;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const int count = last - first + 1;
    for (int i = first; i <= last; ++i) {
      const double t = i / rate;
      const double y = 10.0 * std::log10(std::max(curve[static_cast<size_t>(i)], 1.0e-30) / total);
      sx += t;
      sy += y;
      sxx += t * t;
      sxy += t * y;
    }
    const double denominator = count * sxx - sx * sx;
    if (std::abs(denominator) < 1.0e-18)
      return std::nullopt;
    const double slope = (count * sxy - sx * sy) / denominator;
    if (slope >= -1.0e-6)
      return std::nullopt;
    return 60.0 / -slope;
  };

  // EDT from 0 to -10 dB: the curve starts at 0, so begin the fit at the top.
  fit.edt = reverbTime(-0.01, -10.0, 20.0);
  fit.t30 = reverbTime(-5.0, -35.0, 45.0);
  fit.t20 = reverbTime(-5.0, -25.0, 35.0);
  return fit;
}

SweepAnalysis analyseSweep(const std::vector<float>& recording, const Segment& segment,
                           const Mapper& mapper, const SweepKit& kit, double rate) {
  SweepAnalysis result;
  const double peakDb = segment.params.count("peakDb") ? segment.params.at("peakDb") : -18.0;
  const int pre = static_cast<int>(std::llround(kSweepPreSeconds * rate));
  const int sweepLength = static_cast<int>(segment.lengthSamples);
  const int tail = static_cast<int>(segment.params.at("tailSamples"));
  const std::int64_t sliceStart = mapper.sliceStart(static_cast<double>(segment.startSample), pre);
  const std::vector<float> slice = sliceOf(recording, sliceStart, pre + sweepLength + tail);

  const std::vector<float> response = deconvolve(kit, slice);
  const int zero = pre + sweepLength - 1;
  const int keep = static_cast<int>(std::llround(kIrKeepSeconds * rate));
  const int irLength = keep + tail - static_cast<int>(std::llround(0.2 * rate));
  const double gain = 1.0 / dbToGain(peakDb);
  std::vector<float> ir(static_cast<size_t>(irLength));
  for (int i = 0; i < irLength; ++i)
    ir[static_cast<size_t>(i)] =
        static_cast<float>(response[static_cast<size_t>(zero - keep + i)] * gain);

  // The direct sound: the first envelope peak within 20 dB of the strongest,
  // from a few ms before the zero lag on.
  const std::vector<double> envelope = irEnvelope(ir, rate);
  const int searchFrom = std::max(1, keep - static_cast<int>(std::llround(kEarlyPreSeconds * rate)));
  double strongest = 0.0;
  for (size_t i = static_cast<size_t>(searchFrom); i < envelope.size(); ++i)
    strongest = std::max(strongest, envelope[i]);
  int directEnvelope = searchFrom;
  if (strongest > 0.0) {
    int i = searchFrom;
    while (i < irLength - 1 && envelope[static_cast<size_t>(i)] < 0.1 * strongest)
      ++i;
    while (i < irLength - 1 && envelope[static_cast<size_t>(i + 1)] >= envelope[static_cast<size_t>(i)])
      ++i;
    directEnvelope = i;
  }
  // The sharpest sample near it, so that the windows of the recording and of
  // the unprocessed sweep are cut at the same point of the same wave.
  const int near1ms = std::max(1, static_cast<int>(std::llround(0.001 * rate)));
  int direct = directEnvelope;
  for (int i = std::max(searchFrom, directEnvelope - near1ms);
       i <= std::min(irLength - 1, directEnvelope + near1ms); ++i)
    if (std::abs(ir[static_cast<size_t>(i)]) > std::abs(ir[static_cast<size_t>(direct)]))
      direct = i;

  // Response of the early part against the unprocessed sweep's.
  const std::array<double, kNumBands> power = earlyBandPower(ir, direct, rate);
  std::array<double, kNumBands> relative{};
  for (int band = 0; band < kNumBands; ++band) {
    const double ref = kit.referencePower[static_cast<size_t>(band)];
    const double ratio = power[static_cast<size_t>(band)] / std::max(ref, 1.0e-30);
    relative[static_cast<size_t>(band)] = ratio > 1.0e-12 ? 10.0 * std::log10(ratio) : AudioMeasure::kFloorDb;
  }
  result.levelGainDb = relative[static_cast<size_t>(kReferenceBand)];
  for (int band = 0; band < kNumBands; ++band)
    result.responseDb[static_cast<size_t>(band)] =
        relative[static_cast<size_t>(band)] - result.levelGainDb;

  // Echoes: envelope peaks after the direct sound that stand clear of their
  // surroundings, within 40 dB of the strongest.
  const int radius = std::max(1, static_cast<int>(std::llround(kEchoRadiusSeconds * rate)));
  const int window = std::max(1, static_cast<int>(std::llround(kEchoWindowSeconds * rate)));
  const auto windowEnergy = [&](int centre) {
    double sum = 0.0;
    for (int i = std::max(0, centre - window); i <= std::min(irLength - 1, centre + window); ++i)
      sum += static_cast<double>(ir[static_cast<size_t>(i)]) * ir[static_cast<size_t>(i)];
    return sum;
  };
  const double directEnergy = windowEnergy(directEnvelope);
  const double floorLevel = strongest * dbToGain(kEchoFloorDb);
  std::vector<int> peaks;
  for (int i = directEnvelope + radius;
       i < irLength - radius && static_cast<int>(peaks.size()) < kMaxEchoes; ++i) {
    const double value = envelope[static_cast<size_t>(i)];
    if (value < floorLevel)
      continue;
    bool isPeak = true;
    for (int j = i - radius; j <= i + radius && isPeak; ++j)
      if (envelope[static_cast<size_t>(j)] > value)
        isPeak = false;
    if (isPeak) {
      peaks.push_back(i);
      i += radius;
    }
  }
  for (const int peak : peaks) {
    Echo echo;
    echo.timeMs = 1000.0 * (peak - directEnvelope) / rate;
    echo.levelDb = AudioMeasure::kFloorDb;
    const double energy = windowEnergy(peak);
    if (energy > 0.0 && directEnergy > 0.0)
      echo.levelDb = 10.0 * std::log10(energy / directEnergy);
    result.echoes.push_back(echo);
  }

  // A regular train: the gaps (direct sound to first echo, echo to echo) agree
  // within 15%. The delay is their median; the feedback is the median level
  // step between echoes.
  if (!result.echoes.empty()) {
    std::vector<double> gaps;
    double last = 0.0;
    for (const Echo& echo : result.echoes) {
      gaps.push_back(echo.timeMs - last);
      last = echo.timeMs;
    }
    const double typical = median(gaps);
    double deviation = 0.0;
    for (const double gap : gaps)
      deviation += (gap - typical) * (gap - typical);
    deviation = std::sqrt(deviation / static_cast<double>(gaps.size()));
    if (gaps.size() == 1 || deviation < 0.15 * typical) {
      result.delayMs = typical;
      if (result.echoes.size() >= 2) {
        std::vector<double> steps;
        for (size_t i = 1; i < result.echoes.size(); ++i)
          steps.push_back(result.echoes[i].levelDb - result.echoes[i - 1].levelDb);
        result.feedbackDb = median(steps);
      }
    }
  }

  // Decay of what follows the direct sound (and the 10 ms after it, which
  // hold the dry impulse), unless the response is a clean echo train.
  if (!result.delayMs.has_value()) {
    const int start = directEnvelope + static_cast<int>(std::llround(0.01 * rate));
    if (start < irLength) {
      std::vector<double> energy(static_cast<size_t>(irLength - start));
      for (int i = start; i < irLength; ++i)
        energy[static_cast<size_t>(i - start)] =
            static_cast<double>(ir[static_cast<size_t>(i)]) * ir[static_cast<size_t>(i)];
      double tailEnergy = 0.0;
      for (const double e : energy)
        tailEnergy += e;
      // Less than -30 dB of the direct sound's energy left after it: the
      // ringing of the sweep's band limits and nothing the unit added.
      if (tailEnergy > directEnergy * 1.0e-3) {
        const DecayFit fit = fitDecay(energy, rate);
        result.decay.edtS = fit.edt;
        result.decay.t30S = fit.t30;
      }
    }
  }
  return result;
}

// ####################
// SINE ANALYSIS
// ####################

// The dominant frequency of the variation in `signal` (sampled at `rate`),
// between lowHz and highHz, refined between bins.
std::optional<double> dominantRate(const std::vector<double>& signal, double rate, double lowHz,
                                   double highHz) {
  if (signal.size() < 64)
    return std::nullopt;
  const double mean = std::accumulate(signal.begin(), signal.end(), 0.0) / signal.size();
  const int order = orderFor(signal.size()) + 2;
  const size_t size = static_cast<size_t>(1) << order;
  std::vector<float> data(size * 2, 0.0f);
  const double n = static_cast<double>(signal.size());
  for (size_t i = 0; i < signal.size(); ++i)
    data[i] = static_cast<float>((signal[i] - mean) * (0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / n)));
  juce::dsp::FFT fft(order);
  fft.performRealOnlyForwardTransform(data.data(), true);
  const double binHz = rate / static_cast<double>(size);
  const auto magnitude = [&](size_t bin) {
    return std::hypot(static_cast<double>(data[bin * 2]), static_cast<double>(data[bin * 2 + 1]));
  };
  size_t best = 0;
  double bestValue = 0.0;
  for (size_t bin = std::max<size_t>(2, static_cast<size_t>(lowHz / binHz));
       bin <= std::min(size / 2 - 1, static_cast<size_t>(highHz / binHz)); ++bin)
    if (magnitude(bin) > bestValue) {
      bestValue = magnitude(bin);
      best = bin;
    }
  if (best == 0 || bestValue <= 0.0)
    return std::nullopt;
  const double left = std::log(std::max(magnitude(best - 1), 1.0e-30));
  const double mid = std::log(std::max(magnitude(best), 1.0e-30));
  const double right = std::log(std::max(magnitude(best + 1), 1.0e-30));
  const double curve = left - 2.0 * mid + right;
  const double offset = curve < -1.0e-12 ? std::clamp(0.5 * (left - right) / curve, -0.5, 0.5) : 0.0;
  return (static_cast<double>(best) + offset) * binHz;
}

SineAnalysis analyseSine(const std::vector<float>& x, double rate, double hz) {
  SineAnalysis result;
  result.hz = hz;
  const int count = static_cast<int>(x.size());
  if (count < static_cast<int>(rate))
    return result;

  // Harmonic distortion from the spectrum: fundamental and harmonics 2 to 10
  // are each the power within a few Hz of their frequency, so the sidebands
  // of a modulation stay with the line they belong to.
  {
    const int order = orderFor(static_cast<size_t>(count));
    const size_t size = static_cast<size_t>(1) << order;
    std::vector<float> data(size * 2, 0.0f);
    for (int i = 0; i < count; ++i)
      data[static_cast<size_t>(i)] = static_cast<float>(
          x[static_cast<size_t>(i)] * (0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / count)));
    juce::dsp::FFT fft(order);
    fft.performRealOnlyForwardTransform(data.data(), true);
    const double binHz = rate / static_cast<double>(size);
    const auto power = [&](size_t bin) {
      return static_cast<double>(data[bin * 2]) * data[bin * 2] +
             static_cast<double>(data[bin * 2 + 1]) * data[bin * 2 + 1];
    };
    // The carrier: the strongest bin within 10% of the nominal frequency.
    size_t carrier = 0;
    double carrierPower = 0.0;
    for (size_t bin = static_cast<size_t>(0.9 * hz / binHz); bin <= static_cast<size_t>(1.1 * hz / binHz);
         ++bin)
      if (power(bin) > carrierPower) {
        carrierPower = power(bin);
        carrier = bin;
      }
    const double f0 = static_cast<double>(carrier) * binHz;
    const double half = std::min(25.0, 0.25 * f0);
    const auto bandPower = [&](double centre) {
      double sum = 0.0;
      const size_t from = static_cast<size_t>(std::max(1.0, (centre - half) / binHz));
      const size_t to = std::min(size / 2, static_cast<size_t>((centre + half) / binHz));
      for (size_t bin = from; bin <= to; ++bin)
        sum += power(bin);
      return sum;
    };
    const double fundamental = bandPower(f0);
    double harmonics = 0.0;
    for (int k = 2; k <= 10 && k * f0 < 0.45 * rate; ++k)
      harmonics += bandPower(k * f0);
    // Normalised like the windowed sum of a unit sine: a fundamental under
    // -80 dBFS is too small to judge.
    double windowSum = 0.0;
    for (int i = 0; i < count; ++i)
      windowSum += 0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / count);
    const double amplitude = 2.0 * std::sqrt(fundamental) / windowSum;
    if (carrier > 0 && amplitude > 1.0e-4 && fundamental > 0.0)
      result.thdPct = 100.0 * std::sqrt(harmonics / fundamental);
  }

  // Demodulate against the nominal frequency, then low-pass with three
  // boxcars: what is left is the complex envelope. Its magnitude is the
  // amplitude, the drift of its phase the pitch.
  // A boxcar of whole carrier cycles (about 5 ms, one cycle at the lowest
  // carrier) has a null on the carrier's image at twice its frequency.
  const double cycles = std::max(1.0, std::round(0.005 * hz));
  const int boxcar = std::max(1, static_cast<int>(std::llround(cycles * rate / hz)));
  std::vector<std::complex<double>> z(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
    z[static_cast<size_t>(i)] =
        static_cast<double>(x[static_cast<size_t>(i)]) *
        std::complex<double>(std::cos(kTwoPi * hz * i / rate), -std::sin(kTwoPi * hz * i / rate));
  const auto smooth = [&](const std::vector<std::complex<double>>& in) {
    std::vector<std::complex<double>> out(in.size());
    std::complex<double> sum{0.0, 0.0};
    for (size_t i = 0; i < in.size(); ++i) {
      sum += in[i];
      if (i >= static_cast<size_t>(boxcar))
        sum -= in[i - static_cast<size_t>(boxcar)];
      out[i] = sum / static_cast<double>(boxcar);
    }
    return out;
  };
  const std::vector<std::complex<double>> baseband = smooth(smooth(smooth(z)));
  const size_t valid = static_cast<size_t>(3 * boxcar);
  if (baseband.size() <= valid + 16)
    return result;

  std::vector<double> envelopeDb, phase;
  double meanAmplitude = 0.0;
  double lastPhase = 0.0;
  double unwrapped = 0.0;
  for (size_t i = valid; i < baseband.size(); ++i) {
    const double amplitude = 2.0 * std::abs(baseband[i]);
    meanAmplitude += amplitude;
    envelopeDb.push_back(AudioMeasure::amplitudeToDb(amplitude));
    const double angle = std::arg(baseband[i]);
    if (i == valid) {
      unwrapped = angle;
    } else {
      double delta = angle - lastPhase;
      while (delta > kPi)
        delta -= kTwoPi;
      while (delta < -kPi)
        delta += kTwoPi;
      unwrapped += delta;
    }
    lastPhase = angle;
    phase.push_back(unwrapped);
  }
  meanAmplitude /= static_cast<double>(envelopeDb.size());
  result.levelDb = AudioMeasure::amplitudeToDb(meanAmplitude);

  // The variation is slow: look at it a thousand times a second.
  const size_t stride = std::max<size_t>(1, static_cast<size_t>(std::llround(rate / 1000.0)));
  const double decimatedRate = rate / static_cast<double>(stride);

  std::vector<double> amplitudeTrace;
  for (size_t i = 0; i < envelopeDb.size(); i += stride)
    amplitudeTrace.push_back(envelopeDb[i]);
  result.ampDepthDb = spread(amplitudeTrace);
  if (result.ampDepthDb >= 0.05)
    result.ampRateHz = dominantRate(amplitudeTrace, decimatedRate, 0.3, 40.0);

  // Instantaneous frequency from the phase over +-0.5 ms, as cents.
  const size_t reach = std::max<size_t>(1, static_cast<size_t>(std::llround(0.0005 * rate)));
  std::vector<double> frequency;
  for (size_t i = reach; i + reach < phase.size(); i += stride)
    frequency.push_back((phase[i + reach] - phase[i - reach]) / static_cast<double>(2 * reach) *
                        rate / kTwoPi);
  const double meanFrequency = std::accumulate(frequency.begin(), frequency.end(), 0.0) /
                               std::max<size_t>(1, frequency.size());
  std::vector<double> cents;
  for (const double f : frequency)
    cents.push_back(1200.0 * std::log2(std::max(1.0e-6, 1.0 + (f - meanFrequency) / hz)));
  result.pitchDepthCents = spread(cents);
  if (result.pitchDepthCents >= 0.5)
    result.pitchRateHz = dominantRate(cents, decimatedRate, 0.3, 40.0);
  return result;
}

// ####################
// STEPS
// ####################

// Level in dBFS (peak of an equivalent sine) over a sliding window of one
// millisecond, centred on each sample.
std::vector<double> levelTrace(const std::vector<float>& x, double rate) {
  const int window = std::max(1, static_cast<int>(std::llround(0.001 * rate)));
  std::vector<double> prefix(x.size() + 1, 0.0);
  for (size_t i = 0; i < x.size(); ++i)
    prefix[i + 1] = prefix[i] + static_cast<double>(x[i]) * x[i];
  std::vector<double> db(x.size(), AudioMeasure::kFloorDb);
  const int half = window / 2;
  for (int i = 0; i < static_cast<int>(x.size()); ++i) {
    const int from = std::max(0, i - half);
    const int to = std::min(static_cast<int>(x.size()), i - half + window);
    if (to <= from)
      continue;
    const double meanSquare =
        (prefix[static_cast<size_t>(to)] - prefix[static_cast<size_t>(from)]) / (to - from);
    db[static_cast<size_t>(i)] = AudioMeasure::powerToDb(2.0 * meanSquare);
  }
  return db;
}

// Time for the level to cover 63% of its way from where it is just after the
// step at `stepIndex` to where it settles (mean power over finalFrom to
// finalTo). Unset when it moves less than 1 dB.
std::optional<double> stepTime(const std::vector<double>& levels,
                               const std::vector<float>& samples, int stepIndex, int finalFrom,
                               int finalTo, double rate) {
  const int total = static_cast<int>(levels.size());
  const int settle = stepIndex + static_cast<int>(std::llround(0.001 * rate));
  finalFrom = std::clamp(finalFrom, 0, total);
  finalTo = std::clamp(finalTo, 0, total);
  if (settle >= total || finalTo - finalFrom < 16)
    return std::nullopt;
  double sum = 0.0;
  for (int i = finalFrom; i < finalTo; ++i)
    sum += static_cast<double>(samples[static_cast<size_t>(i)]) * samples[static_cast<size_t>(i)];
  const double finalDb = AudioMeasure::powerToDb(2.0 * sum / (finalTo - finalFrom));
  const double startDb = levels[static_cast<size_t>(settle)];
  const double change = finalDb - startDb;
  if (std::abs(change) < 1.0)
    return std::nullopt;
  const double target = startDb + 0.632 * change;
  for (int i = settle; i < finalTo; ++i) {
    const double value = levels[static_cast<size_t>(i)];
    if ((change > 0.0 && value >= target) || (change < 0.0 && value <= target)) {
      const double before = levels[static_cast<size_t>(i - 1)];
      const double fraction =
          std::abs(value - before) > 1.0e-12 ? (target - before) / (value - before) : 1.0;
      return 1000.0 * (static_cast<double>(i - 1) + std::clamp(fraction, 0.0, 1.0) - stepIndex) / rate;
    }
  }
  return std::nullopt;
}

// ####################
// ONE CHANNEL
// ####################
ChannelAnalysis analyseChannel(const std::vector<float>& x, double rate, const TestSignal& reference,
                               const Mapper& mapper, const SweepKit& kit) {
  ChannelAnalysis result;
  const auto seconds = [rate](double s) { return static_cast<int>(std::llround(s * rate)); };

  if (const Segment* segment = reference.find("sweepLow"))
    result.sweepLow = analyseSweep(x, *segment, mapper, kit, rate);
  if (const Segment* segment = reference.find("sweepHigh"))
    result.sweepHigh = analyseSweep(x, *segment, mapper, kit, rate);

  for (const char* name : {"sine100", "sine1k"}) {
    const Segment* segment = reference.find(name);
    if (segment == nullptr)
      continue;
    // The steady part: after the fade-in, before the fade-out.
    const std::int64_t start = mapper.sliceStart(static_cast<double>(segment->startSample) + seconds(0.5));
    const std::vector<float> steady = sliceOf(x, start, seconds(5.0));
    SineAnalysis sine = analyseSine(steady, rate, segment->params.at("hz"));
    if (std::string(name) == "sine100")
      result.sine100 = sine;
    else
      result.sine1k = sine;
  }

  if (const Segment* segment = reference.find("staircase")) {
    const int stepSamples = static_cast<int>(segment->params.at("stepSamples"));
    const int steps = static_cast<int>(segment->params.at("steps"));
    for (int step = 0; step < steps; ++step) {
      const double nominal = static_cast<double>(segment->startSample) + step * stepSamples +
                             0.19 * rate;
      const std::vector<float> part = sliceOf(x, mapper.sliceStart(nominal), seconds(0.1));
      CurvePoint point;
      point.inDb = segment->params.at("startDb") + step * segment->params.at("stepDb");
      point.outDb = AudioMeasure::toneLevelDb(part.data(), static_cast<int>(part.size()),
                                              segment->params.at("hz"), rate);
      result.staticCurve.push_back(point);
    }
  }

  if (const Segment* segment = reference.find("steps")) {
    const std::int64_t start = mapper.sliceStart(static_cast<double>(segment->startSample));
    const std::vector<float> part = sliceOf(x, start, segment->lengthSamples);
    const std::vector<double> levels = levelTrace(part, rate);
    // The positions inside the segment stretch with the recording's clock.
    const int up = static_cast<int>(std::llround(segment->params.at("upSample") * mapper.scale));
    const int down = static_cast<int>(std::llround(segment->params.at("downSample") * mapper.scale));
    const int end = static_cast<int>(part.size());
    result.steps.attackMs = stepTime(levels, part, up, down - seconds(0.2), down - seconds(0.02), rate);
    result.steps.releaseMs = stepTime(levels, part, down, end - seconds(0.5), end - seconds(0.05), rate);
  }

  if (const Segment* segment = reference.find("burst")) {
    const std::int64_t start = mapper.sliceStart(static_cast<double>(segment->startSample));
    const int burst = static_cast<int>(segment->lengthSamples);
    const int tail = static_cast<int>(segment->params.at("tailSamples"));
    const std::vector<float> part = sliceOf(x, start, burst + tail);
    juce::AudioBuffer<float> buffer(1, static_cast<int>(part.size()));
    std::copy(part.begin(), part.end(), buffer.getWritePointer(0));
    bool truncated = false;
    result.burst.tailEndMs = 1000.0 * AudioMeasure::tailSeconds(buffer, burst, rate, &truncated);
    result.burst.truncated = truncated;
    std::vector<double> energy(static_cast<size_t>(tail));
    for (int i = 0; i < tail; ++i)
      energy[static_cast<size_t>(i)] =
          static_cast<double>(part[static_cast<size_t>(burst + i)]) * part[static_cast<size_t>(burst + i)];
    const DecayFit fit = fitDecay(energy, rate);
    result.burst.tailT60s = fit.t30.has_value() ? fit.t30 : fit.t20;
  }

  if (const Segment* segment = reference.find("silence")) {
    // Skip the first quarter second, where the previous segment may still be
    // dying away.
    const std::int64_t start = mapper.sliceStart(static_cast<double>(segment->startSample) + seconds(0.25));
    const std::vector<float> part = sliceOf(x, start, segment->lengthSamples - seconds(0.25));
    double sum = 0.0;
    for (const float v : part)
      sum += static_cast<double>(v) * v;
    const double rms = std::sqrt(sum / std::max<size_t>(1, part.size()));
    // The quietest tenth of the windows, as AudioMeasure reads a noise floor,
    // unless the whole take is quieter than that.
    result.noiseFloorDb = std::min(AudioMeasure::amplitudeToDb(rms),
                                   AudioMeasure::noiseFloorDb(part.data(), static_cast<int>(part.size()), rate));
  }
  return result;
}

}  // namespace

// ####################
// TEST SIGNAL
// ####################
const Segment* TestSignal::find(const std::string& name) const {
  for (const Segment& segment : segments)
    if (segment.name == name)
      return &segment;
  return nullptr;
}

TestSignal makeTestSignal(double rate) {
  TestSignal signal;
  signal.sampleRate = rate;
  const auto seconds = [rate](double s) { return static_cast<std::int64_t>(std::llround(s * rate)); };

  signal.samples.assign(static_cast<size_t>(seconds(kLeadSeconds)), 0.0f);
  const auto place = [&](const std::string& name, const std::string& kind,
                         const std::vector<float>& part, double tailSeconds,
                         std::map<std::string, double> params) {
    Segment segment;
    segment.name = name;
    segment.kind = kind;
    segment.startSample = static_cast<std::int64_t>(signal.samples.size());
    segment.lengthSamples = static_cast<std::int64_t>(part.size());
    params["tailSamples"] = static_cast<double>(seconds(tailSeconds));
    segment.params = std::move(params);
    signal.samples.insert(signal.samples.end(), part.begin(), part.end());
    signal.samples.insert(signal.samples.end(), static_cast<size_t>(seconds(tailSeconds)), 0.0f);
    signal.segments.push_back(std::move(segment));
  };

  const std::vector<float> marker = makeMarker(rate);
  place("syncStart", "sync", marker, kMarkerTailSeconds, {{"peakDb", -6.0}});

  for (const auto& [name, peakDb] : {std::pair<const char*, double>{"sweepLow", -18.0},
                                     std::pair<const char*, double>{"sweepHigh", -6.0}}) {
    place(name, "sweep", makeSweep(rate, 20.0, 20000.0, kSweepSeconds, dbToGain(peakDb), 0.01),
          kSweepTailSeconds,
          {{"f1Hz", 20.0}, {"f2Hz", 20000.0}, {"seconds", kSweepSeconds}, {"peakDb", peakDb}});
  }

  place("sine100", "sine", makeSine(rate, 100.0, kSineSeconds, dbToGain(-18.0)), kSineTailSeconds,
        {{"hz", 100.0}, {"peakDb", -18.0}});
  place("sine1k", "sine", makeSine(rate, 1000.0, kSineSeconds, dbToGain(-18.0)), kSineTailSeconds,
        {{"hz", 1000.0}, {"peakDb", -18.0}});

  {
    std::vector<double> levels;
    std::vector<int> holds;
    for (int step = 0; step < kStaircaseSteps; ++step) {
      levels.push_back(-60.0 + 3.0 * step);
      holds.push_back(static_cast<int>(seconds(kStaircaseStepSeconds)));
    }
    place("staircase", "staircase",
          makeLevelSteps(rate, 1000.0, levels, holds, static_cast<int>(seconds(0.002)),
                         static_cast<int>(seconds(0.002))),
          kStaircaseTailSeconds,
          {{"hz", 1000.0},
           {"startDb", -60.0},
           {"stepDb", 3.0},
           {"steps", static_cast<double>(kStaircaseSteps)},
           {"stepSamples", static_cast<double>(seconds(kStaircaseStepSeconds))},
           {"rampSamples", static_cast<double>(seconds(0.002))}});
  }

  {
    const std::vector<double> levels = {-40.0, -10.0, -40.0};
    const std::vector<int> holds = {static_cast<int>(seconds(1.0)), static_cast<int>(seconds(1.0)),
                                    static_cast<int>(seconds(2.0))};
    place("steps", "steps",
          makeLevelSteps(rate, 1000.0, levels, holds, static_cast<int>(seconds(0.0005)),
                         static_cast<int>(seconds(0.002))),
          1.0,
          {{"hz", 1000.0},
           {"lowDb", -40.0},
           {"highDb", -10.0},
           {"upSample", static_cast<double>(seconds(1.0))},
           {"downSample", static_cast<double>(seconds(2.0))},
           {"rampSamples", static_cast<double>(seconds(0.0005))}});
  }

  place("burst", "noiseBurst", makePinkBurst(rate, kBurstSeconds, -12.0), kBurstTailSeconds,
        {{"rmsDb", -12.0}});
  place("silence", "silence",
        std::vector<float>(static_cast<size_t>(seconds(kSilenceSeconds)), 0.0f), kSilenceTailSeconds,
        {});
  place("syncEnd", "sync", marker, kMarkerTailSeconds, {{"peakDb", -6.0}});
  return signal;
}

juce::var toVar(const TestSignal& signal) {
  juce::Array<juce::var> segments;
  for (const Segment& segment : signal.segments) {
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    for (const auto& [key, value] : segment.params)
      params->setProperty(juce::Identifier(key.c_str()), value);
    segments.add(object({
        {"name", juce::String(segment.name)},
        {"kind", juce::String(segment.kind)},
        {"startSample", static_cast<juce::int64>(segment.startSample)},
        {"lengthSamples", static_cast<juce::int64>(segment.lengthSamples)},
        {"params", juce::var(params.get())},
    }));
  }
  return object({
      {"signalVersion", signal.version},
      {"sampleRate", signal.sampleRate},
      {"channels", 1},
      {"totalSamples", static_cast<juce::int64>(signal.samples.size())},
      {"durationSeconds", tidy(static_cast<double>(signal.samples.size()) / signal.sampleRate, 100.0)},
      {"segments", segments},
  });
}

// ####################
// ANALYSIS
// ####################
MatchAnalysis analyseRecording(const juce::AudioBuffer<float>& recording, double rate,
                               const TestSignal& reference) {
  if (recording.getNumChannels() < 1 || recording.getNumSamples() < 1)
    throw MatchError("the recording is empty");
  if (std::abs(rate - reference.sampleRate) > 0.01)
    throw MatchError("the recording is at " + std::to_string(rate) + " Hz but the signal was made at " +
                     std::to_string(reference.sampleRate) + " Hz; resample the recording first");
  const Segment* first = reference.find("syncStart");
  const Segment* last = reference.find("syncEnd");
  if (first == nullptr || last == nullptr)
    throw MatchError("the signal has no sync markers");

  const int channels = std::min(2, recording.getNumChannels());
  std::vector<std::vector<float>> data(static_cast<size_t>(channels));
  for (int ch = 0; ch < channels; ++ch)
    data[static_cast<size_t>(ch)].assign(recording.getReadPointer(ch),
                                         recording.getReadPointer(ch) + recording.getNumSamples());
  // Alignment runs on whichever of the channels and their mix holds the
  // marker best, so a one-sided stereo effect (or an inverted side) is still
  // found.
  std::vector<std::vector<float>> candidates = {data[0]};
  if (channels == 2) {
    candidates.push_back(data[1]);
    std::vector<float> mixed = data[0];
    for (size_t i = 0; i < mixed.size(); ++i)
      mixed[i] = 0.5f * (data[0][i] + data[1][i]);
    candidates.push_back(std::move(mixed));
  }

  const std::vector<float> marker =
      sliceOf(reference.samples, first->startSample, first->lengthSamples);
  constexpr double kMinScore = 0.2;
  const std::int64_t total = static_cast<std::int64_t>(data[0].size());
  const auto seconds = [rate](double s) { return static_cast<std::int64_t>(std::llround(s * rate)); };

  MarkerHit start;
  size_t chosen = 0;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const MarkerHit hit = findMarker(candidates[i], marker, 0, first->startSample + seconds(6.0));
    if (i == 0 || hit.score > start.score) {
      start = hit;
      chosen = i;
    }
  }
  const std::vector<float>& mix = candidates[chosen];
  if (start.score < kMinScore)
    throw MatchError(
        "the start sync marker was not found in the first seconds of the recording (best match " +
        std::to_string(start.score) + "); check the routing, the levels and that the file is the "
        "recording of the test signal");

  const double expectedEnd = start.position + static_cast<double>(last->startSample - first->startSample);
  const MarkerHit end = findMarker(mix, marker, static_cast<std::int64_t>(expectedEnd) - seconds(2.0),
                                   static_cast<std::int64_t>(expectedEnd) + seconds(2.0) +
                                       static_cast<std::int64_t>(marker.size()));
  if (end.score < kMinScore)
    throw MatchError(
        "the end sync marker was not found where it should be (best match " +
        std::to_string(end.score) + ", recording length " + std::to_string(total) +
        " samples); the recording is cut short or its clock differs a lot from the signal's");

  const double nominalGap = static_cast<double>(last->startSample - first->startSample);
  Mapper mapper;
  mapper.originRecording = start.position;
  mapper.originNominal = static_cast<double>(first->startSample);
  mapper.scale = (end.position - start.position) / nominalGap;

  MatchAnalysis result;
  result.version = reference.version;
  result.sampleRate = rate;
  result.channels = channels;
  result.alignment.latencyMs = 1000.0 * (start.position - static_cast<double>(first->startSample)) / rate;
  result.alignment.driftPpm = (mapper.scale - 1.0) * 1.0e6;
  result.alignment.startScore = start.score;
  result.alignment.endScore = end.score;
  if (std::abs(result.alignment.driftPpm) > 20000.0)
    throw MatchError("the clock of the recording differs by " +
                     std::to_string(result.alignment.driftPpm) +
                     " ppm from the signal's; the markers were probably mismatched");

  const SweepKit& kit = sweepKitFor(rate);
  result.left = analyseChannel(data[0], rate, reference, mapper, kit);
  if (channels == 2) {
    result.right = analyseChannel(data[1], rate, reference, mapper, kit);
    if (const Segment* burst = reference.find("burst")) {
      const std::int64_t from = mapper.sliceStart(static_cast<double>(burst->startSample));
      const std::int64_t count = burst->lengthSamples + static_cast<std::int64_t>(burst->params.at("tailSamples"));
      juce::AudioBuffer<float> part(2, static_cast<int>(count));
      for (int ch = 0; ch < 2; ++ch) {
        const std::vector<float> piece = sliceOf(data[static_cast<size_t>(ch)], from, count);
        std::copy(piece.begin(), piece.end(), part.getWritePointer(ch));
      }
      const AudioMeasure::Stereo stereo = AudioMeasure::measureStereo(part);
      result.stereo = StereoAnalysis{stereo.correlation, stereo.width};
    }
  }
  return result;
}

// ####################
// JSON
// ####################
namespace {

juce::String bandLabel(int band) {
  const double centre = kBandCentresHz[static_cast<size_t>(band)];
  return std::abs(centre - std::round(centre)) < 1.0e-9 ? juce::String(static_cast<int>(centre))
                                                        : juce::String(centre, 1);
}

juce::var optionalVar(const std::optional<double>& value, double perUnit) {
  return value.has_value() ? juce::var(tidy(*value, perUnit)) : juce::var();
}

juce::var toVar(const SweepAnalysis& sweep) {
  juce::DynamicObject::Ptr response = new juce::DynamicObject();
  for (int band = 0; band < kNumBands; ++band)
    response->setProperty(juce::Identifier(bandLabel(band)),
                          AudioMeasure::tidyDb(sweep.responseDb[static_cast<size_t>(band)]));
  juce::Array<juce::var> echoes;
  for (const Echo& echo : sweep.echoes)
    echoes.add(object({{"timeMs", tidy(echo.timeMs, 100.0)},
                       {"levelDb", AudioMeasure::tidyDb(echo.levelDb)}}));
  return object({
      {"responseDb", juce::var(response.get())},
      {"levelGainDb", AudioMeasure::tidyDb(sweep.levelGainDb)},
      {"echoes", echoes},
      {"delayMs", optionalVar(sweep.delayMs, 100.0)},
      {"feedbackDb", optionalVar(sweep.feedbackDb, 100.0)},
      {"decay", object({{"edtS", optionalVar(sweep.decay.edtS, 1000.0)},
                        {"t30S", optionalVar(sweep.decay.t30S, 1000.0)}})},
  });
}

juce::var toVar(const SineAnalysis& sine) {
  return object({
      {"hz", sine.hz},
      {"levelDb", AudioMeasure::tidyDb(sine.levelDb)},
      {"thdPct", optionalVar(sine.thdPct, 10000.0)},
      {"ampDepthDb", tidy(sine.ampDepthDb, 1000.0)},
      {"ampRateHz", optionalVar(sine.ampRateHz, 100.0)},
      {"pitchDepthCents", tidy(sine.pitchDepthCents, 100.0)},
      {"pitchRateHz", optionalVar(sine.pitchRateHz, 100.0)},
  });
}

juce::var toVar(const ChannelAnalysis& channel) {
  juce::Array<juce::var> curve;
  for (const CurvePoint& point : channel.staticCurve)
    curve.add(object({{"inDb", AudioMeasure::tidyDb(point.inDb)},
                      {"outDb", AudioMeasure::tidyDb(point.outDb)}}));
  return object({
      {"sweepLow", channel.sweepLow ? toVar(*channel.sweepLow) : juce::var()},
      {"sweepHigh", channel.sweepHigh ? toVar(*channel.sweepHigh) : juce::var()},
      {"sine100", channel.sine100 ? toVar(*channel.sine100) : juce::var()},
      {"sine1k", channel.sine1k ? toVar(*channel.sine1k) : juce::var()},
      {"staticCurve", curve},
      {"steps", object({{"attackMs", optionalVar(channel.steps.attackMs, 10.0)},
                        {"releaseMs", optionalVar(channel.steps.releaseMs, 10.0)}})},
      {"burst", object({{"tailT60s", optionalVar(channel.burst.tailT60s, 1000.0)},
                        {"tailEndMs", tidy(channel.burst.tailEndMs, 10.0)},
                        {"truncated", channel.burst.truncated}})},
      {"noiseFloorDb", AudioMeasure::tidyDb(channel.noiseFloorDb)},
  });
}

double number(const juce::var& source, const char* key, double fallback = 0.0) {
  const juce::var value = source.getProperty(juce::Identifier(key), juce::var());
  return value.isVoid() ? fallback : static_cast<double>(value);
}

std::optional<double> optionalNumber(const juce::var& source, const char* key) {
  const juce::var value = source.getProperty(juce::Identifier(key), juce::var());
  if (value.isVoid())
    return std::nullopt;
  return static_cast<double>(value);
}

SweepAnalysis sweepFromVar(const juce::var& json) {
  SweepAnalysis sweep;
  const juce::var response = json.getProperty("responseDb", juce::var());
  for (int band = 0; band < kNumBands; ++band)
    sweep.responseDb[static_cast<size_t>(band)] = number(response, bandLabel(band).toRawUTF8());
  sweep.levelGainDb = number(json, "levelGainDb");
  if (const auto* echoes = json.getProperty("echoes", juce::var()).getArray())
    for (const juce::var& echo : *echoes)
      sweep.echoes.push_back({number(echo, "timeMs"), number(echo, "levelDb")});
  sweep.delayMs = optionalNumber(json, "delayMs");
  sweep.feedbackDb = optionalNumber(json, "feedbackDb");
  const juce::var decay = json.getProperty("decay", juce::var());
  sweep.decay.edtS = optionalNumber(decay, "edtS");
  sweep.decay.t30S = optionalNumber(decay, "t30S");
  return sweep;
}

SineAnalysis sineFromVar(const juce::var& json) {
  SineAnalysis sine;
  sine.hz = number(json, "hz");
  sine.levelDb = number(json, "levelDb");
  sine.thdPct = optionalNumber(json, "thdPct");
  sine.ampDepthDb = number(json, "ampDepthDb");
  sine.ampRateHz = optionalNumber(json, "ampRateHz");
  sine.pitchDepthCents = number(json, "pitchDepthCents");
  sine.pitchRateHz = optionalNumber(json, "pitchRateHz");
  return sine;
}

ChannelAnalysis channelFromVar(const juce::var& json) {
  ChannelAnalysis channel;
  if (!json.isObject())
    throw MatchError("the analysis has no channel data");
  const juce::var low = json.getProperty("sweepLow", juce::var());
  const juce::var high = json.getProperty("sweepHigh", juce::var());
  const juce::var s100 = json.getProperty("sine100", juce::var());
  const juce::var s1k = json.getProperty("sine1k", juce::var());
  if (low.isObject())
    channel.sweepLow = sweepFromVar(low);
  if (high.isObject())
    channel.sweepHigh = sweepFromVar(high);
  if (s100.isObject())
    channel.sine100 = sineFromVar(s100);
  if (s1k.isObject())
    channel.sine1k = sineFromVar(s1k);
  if (const auto* curve = json.getProperty("staticCurve", juce::var()).getArray())
    for (const juce::var& point : *curve)
      channel.staticCurve.push_back({number(point, "inDb"), number(point, "outDb")});
  const juce::var steps = json.getProperty("steps", juce::var());
  channel.steps.attackMs = optionalNumber(steps, "attackMs");
  channel.steps.releaseMs = optionalNumber(steps, "releaseMs");
  const juce::var burst = json.getProperty("burst", juce::var());
  channel.burst.tailT60s = optionalNumber(burst, "tailT60s");
  channel.burst.tailEndMs = number(burst, "tailEndMs");
  channel.burst.truncated = static_cast<bool>(burst.getProperty("truncated", false));
  channel.noiseFloorDb = number(json, "noiseFloorDb", AudioMeasure::kFloorDb);
  return channel;
}

}  // namespace

juce::var toVar(const MatchAnalysis& analysis) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("signalVersion", analysis.version);
  obj->setProperty("sampleRate", analysis.sampleRate);
  obj->setProperty("channels", analysis.channels);
  obj->setProperty("alignment",
                   object({{"latencyMs", tidy(analysis.alignment.latencyMs, 100.0)},
                           {"driftPpm", tidy(analysis.alignment.driftPpm, 10.0)},
                           {"startScore", tidy(analysis.alignment.startScore, 1000.0)},
                           {"endScore", tidy(analysis.alignment.endScore, 1000.0)}}));
  obj->setProperty("left", toVar(analysis.left));
  if (analysis.right)
    obj->setProperty("right", toVar(*analysis.right));
  if (analysis.stereo)
    obj->setProperty("stereo", object({{"correlation", tidy(analysis.stereo->correlation, 10000.0)},
                                       {"width", tidy(analysis.stereo->width, 10000.0)}}));
  return juce::var(obj.get());
}

MatchAnalysis analysisFromVar(const juce::var& json) {
  if (!json.isObject() || !json.hasProperty("left") || !json.hasProperty("alignment"))
    throw MatchError("not an analysis: expected the JSON written by 'PedaleiraMatch analyse'");
  MatchAnalysis analysis;
  analysis.version = static_cast<int>(number(json, "signalVersion", kSignalVersion));
  if (analysis.version != kSignalVersion)
    throw MatchError("the analysis was made with test signal version " +
                     std::to_string(analysis.version) + ", this build uses " +
                     std::to_string(kSignalVersion));
  analysis.sampleRate = number(json, "sampleRate", kSignalRate);
  analysis.channels = static_cast<int>(number(json, "channels", 1));
  const juce::var alignment = json.getProperty("alignment", juce::var());
  analysis.alignment.latencyMs = number(alignment, "latencyMs");
  analysis.alignment.driftPpm = number(alignment, "driftPpm");
  analysis.alignment.startScore = number(alignment, "startScore");
  analysis.alignment.endScore = number(alignment, "endScore");
  analysis.left = channelFromVar(json.getProperty("left", juce::var()));
  if (json.hasProperty("right"))
    analysis.right = channelFromVar(json.getProperty("right", juce::var()));
  if (json.hasProperty("stereo")) {
    const juce::var stereo = json.getProperty("stereo", juce::var());
    analysis.stereo = StereoAnalysis{number(stereo, "correlation", 1.0), number(stereo, "width")};
  }
  return analysis;
}

// ####################
// COMPARISON
// ####################
namespace {

class Collector {
public:
  explicit Collector(const Tolerances& t) : tolerances(t) {}

  // Adds one metric. Both missing: nothing to compare. One missing: a
  // failure, since the two units then differ in kind.
  void add(const std::string& group, const std::string& name, const std::string& unit,
           const std::optional<double>& reference, const std::optional<double>& candidate,
           double absolute, double relative = 0.0, bool judged = true) {
    if (!reference && !candidate)
      return;
    MetricComparison metric;
    metric.group = group;
    metric.name = name;
    metric.unit = unit;
    metric.reference = reference;
    metric.candidate = candidate;
    metric.judged = judged;
    if (reference && candidate) {
      metric.difference = *candidate - *reference;
      metric.allowed = std::max(absolute, relative * std::abs(*reference));
      metric.pass = std::abs(metric.difference) <= metric.allowed + 1.0e-9;
    } else {
      metric.allowed = absolute;
      metric.pass = false;
      metric.note = reference ? "missing in the candidate" : "missing in the reference";
    }
    if (!judged) {
      metric.pass = true;
      metric.note = "not judged";
    }
    out.metrics.push_back(std::move(metric));
  }

  void finish() {
    out.judged = 0;
    out.passed = 0;
    for (const MetricComparison& metric : out.metrics) {
      if (!metric.judged)
        continue;
      ++out.judged;
      if (metric.pass)
        ++out.passed;
    }
    out.score = out.judged > 0 ? tidy(100.0 * out.passed / out.judged, 10.0) : 100.0;
    out.allPass = out.passed == out.judged;
  }

  const Tolerances& tolerances;
  MatchComparison out;
};

void compareSweep(Collector& c, const std::string& group, const std::optional<SweepAnalysis>& ref,
                  const std::optional<SweepAnalysis>& cand) {
  if (!ref && !cand)
    return;
  const Tolerances& t = c.tolerances;
  const auto opt = [](const std::optional<SweepAnalysis>& s, auto getter) -> std::optional<double> {
    return s ? getter(*s) : std::nullopt;
  };
  for (int band = 0; band < kNumBands; ++band) {
    const size_t i = static_cast<size_t>(band);
    c.add(group, "response " + bandLabel(band).toStdString() + " Hz", "dB",
          opt(ref, [i](const SweepAnalysis& s) { return std::optional<double>(s.responseDb[i]); }),
          opt(cand, [i](const SweepAnalysis& s) { return std::optional<double>(s.responseDb[i]); }),
          t.responseDb);
  }
  c.add(group, "level gain at 1 kHz", "dB",
        opt(ref, [](const SweepAnalysis& s) { return std::optional<double>(s.levelGainDb); }),
        opt(cand, [](const SweepAnalysis& s) { return std::optional<double>(s.levelGainDb); }),
        t.levelGainDb);
  c.add(group, "delay time", "ms", opt(ref, [](const SweepAnalysis& s) { return s.delayMs; }),
        opt(cand, [](const SweepAnalysis& s) { return s.delayMs; }), t.delayMs);
  c.add(group, "feedback", "dB", opt(ref, [](const SweepAnalysis& s) { return s.feedbackDb; }),
        opt(cand, [](const SweepAnalysis& s) { return s.feedbackDb; }), t.feedbackDb);
  c.add(group, "decay EDT", "s", opt(ref, [](const SweepAnalysis& s) { return s.decay.edtS; }),
        opt(cand, [](const SweepAnalysis& s) { return s.decay.edtS; }), t.decayAbsoluteS,
        t.decayRelative);
  c.add(group, "decay T30", "s", opt(ref, [](const SweepAnalysis& s) { return s.decay.t30S; }),
        opt(cand, [](const SweepAnalysis& s) { return s.decay.t30S; }), t.decayAbsoluteS,
        t.decayRelative);
}

void compareSine(Collector& c, const std::string& group, const std::optional<SineAnalysis>& ref,
                 const std::optional<SineAnalysis>& cand) {
  if (!ref && !cand)
    return;
  const Tolerances& t = c.tolerances;
  const auto opt = [](const std::optional<SineAnalysis>& s, auto getter) -> std::optional<double> {
    return s ? getter(*s) : std::nullopt;
  };
  c.add(group, "level", "dB", opt(ref, [](const SineAnalysis& s) { return std::optional<double>(s.levelDb); }),
        opt(cand, [](const SineAnalysis& s) { return std::optional<double>(s.levelDb); }), t.levelGainDb);
  c.add(group, "THD", "%", opt(ref, [](const SineAnalysis& s) { return s.thdPct; }),
        opt(cand, [](const SineAnalysis& s) { return s.thdPct; }), t.thdAbsolutePct, t.thdRelative);
  c.add(group, "amplitude modulation depth", "dB",
        opt(ref, [](const SineAnalysis& s) { return std::optional<double>(s.ampDepthDb); }),
        opt(cand, [](const SineAnalysis& s) { return std::optional<double>(s.ampDepthDb); }),
        t.ampDepthAbsoluteDb, t.depthRelative);
  c.add(group, "amplitude modulation rate", "Hz",
        opt(ref, [](const SineAnalysis& s) { return s.ampRateHz; }),
        opt(cand, [](const SineAnalysis& s) { return s.ampRateHz; }), t.rateAbsoluteHz, t.rateRelative);
  c.add(group, "pitch modulation depth", "cents",
        opt(ref, [](const SineAnalysis& s) { return std::optional<double>(s.pitchDepthCents); }),
        opt(cand, [](const SineAnalysis& s) { return std::optional<double>(s.pitchDepthCents); }),
        t.pitchDepthAbsoluteCents, t.depthRelative);
  c.add(group, "pitch modulation rate", "Hz",
        opt(ref, [](const SineAnalysis& s) { return s.pitchRateHz; }),
        opt(cand, [](const SineAnalysis& s) { return s.pitchRateHz; }), t.rateAbsoluteHz, t.rateRelative);
}

void compareChannel(Collector& c, const std::string& prefix, const ChannelAnalysis& ref,
                    const ChannelAnalysis& cand) {
  const Tolerances& t = c.tolerances;
  compareSweep(c, prefix + "sweepLow", ref.sweepLow, cand.sweepLow);
  compareSweep(c, prefix + "sweepHigh", ref.sweepHigh, cand.sweepHigh);
  compareSine(c, prefix + "sine100", ref.sine100, cand.sine100);
  compareSine(c, prefix + "sine1k", ref.sine1k, cand.sine1k);

  const size_t points = std::min(ref.staticCurve.size(), cand.staticCurve.size());
  for (size_t i = 0; i < points; ++i)
    c.add(prefix + "staticCurve",
          "output at " + juce::String(ref.staticCurve[i].inDb, 0).toStdString() + " dBFS in", "dBFS",
          ref.staticCurve[i].outDb, cand.staticCurve[i].outDb, t.staticCurveDb);

  c.add(prefix + "steps", "attack", "ms", ref.steps.attackMs, cand.steps.attackMs, t.stepAbsoluteMs,
        t.stepRelative);
  c.add(prefix + "steps", "release", "ms", ref.steps.releaseMs, cand.steps.releaseMs, t.stepAbsoluteMs,
        t.stepRelative);
  c.add(prefix + "burst", "tail T60", "s", ref.burst.tailT60s, cand.burst.tailT60s, 0.05,
        t.tailRelative);
  c.add(prefix + "burst", "tail end (60 dB down)", "ms", ref.burst.tailEndMs, cand.burst.tailEndMs,
        t.tailAbsoluteMs, t.tailRelative);
  c.add(prefix + "silence", "noise floor", "dBFS", ref.noiseFloorDb, cand.noiseFloorDb, t.noiseFloorDb);
}

juce::String formatValue(const std::optional<double>& value, const std::string& unit, int places) {
  if (!value)
    return "n/a";
  return juce::String(*value, places) + (unit.empty() ? "" : " " + juce::String(unit));
}

}  // namespace

MatchComparison compareAnalyses(const MatchAnalysis& reference, const MatchAnalysis& candidate,
                                const Tolerances& tolerances) {
  Collector c(tolerances);
  c.add("alignment", "latency", "ms", reference.alignment.latencyMs, candidate.alignment.latencyMs,
        tolerances.latencyMs, 0.0, tolerances.judgeLatency);
  c.add("alignment", "clock drift", "ppm", reference.alignment.driftPpm, candidate.alignment.driftPpm,
        0.0, 0.0, false);
  compareChannel(c, "", reference.left, candidate.left);
  if (reference.right && candidate.right)
    compareChannel(c, "right ", *reference.right, *candidate.right);
  if (reference.stereo && candidate.stereo)
    c.add("stereo", "correlation", "", reference.stereo->correlation, candidate.stereo->correlation,
          tolerances.stereoCorrelation);
  else if (reference.stereo || candidate.stereo)
    c.add("stereo", "correlation", "",
          reference.stereo ? std::optional<double>(reference.stereo->correlation) : std::nullopt,
          candidate.stereo ? std::optional<double>(candidate.stereo->correlation) : std::nullopt,
          tolerances.stereoCorrelation);
  c.finish();
  return c.out;
}

juce::var toVar(const MatchComparison& comparison) {
  juce::Array<juce::var> metrics;
  for (const MetricComparison& metric : comparison.metrics)
    metrics.add(object({
        {"group", juce::String(metric.group)},
        {"name", juce::String(metric.name)},
        {"unit", juce::String(metric.unit)},
        {"reference", optionalVar(metric.reference, 1000.0)},
        {"candidate", optionalVar(metric.candidate, 1000.0)},
        {"difference", tidy(metric.difference, 1000.0)},
        {"allowed", tidy(metric.allowed, 1000.0)},
        {"judged", metric.judged},
        {"pass", metric.pass},
        {"note", juce::String(metric.note)},
    }));
  return object({
      {"score", comparison.score},
      {"allPass", comparison.allPass},
      {"judged", comparison.judged},
      {"passed", comparison.passed},
      {"metrics", metrics},
  });
}

juce::String toMarkdown(const MatchComparison& comparison) {
  juce::String text;
  text << "# Effect match\n\n";
  text << "Score " << juce::String(comparison.score, 1) << " (" << comparison.passed << " of "
       << comparison.judged << " metrics within tolerance).\n\n";

  const auto row = [](const MetricComparison& metric, const juce::String& label) {
    juce::String line = "| " + label + " | " +
                        formatValue(metric.reference, metric.unit, 2) + " | " +
                        formatValue(metric.candidate, metric.unit, 2) + " | ";
    if (metric.reference && metric.candidate)
      line << (metric.difference > 0.0 ? "+" : "") << juce::String(metric.difference, 2) << " | "
           << juce::String(metric.allowed, 2) << " | ";
    else
      line << "n/a | n/a | ";
    line << (!metric.judged ? "not judged" : metric.pass ? "ok" : "FAIL")
         << (metric.note.empty() || !metric.judged ? juce::String() : " (" + juce::String(metric.note) + ")")
         << " |\n";
    return line;
  };
  const juce::String header =
      "| Metric | Reference | Candidate | Difference | Allowed | Result |\n"
      "|---|---|---|---|---|---|\n";

  bool anyFailure = false;
  for (const MetricComparison& metric : comparison.metrics)
    anyFailure = anyFailure || (metric.judged && !metric.pass);
  if (anyFailure) {
    text << "## Failures\n\n" << header;
    for (const MetricComparison& metric : comparison.metrics)
      if (metric.judged && !metric.pass)
        text << row(metric, juce::String(metric.group) + ": " + juce::String(metric.name));
    text << "\n";
  }

  std::vector<std::string> groups;
  for (const MetricComparison& metric : comparison.metrics)
    if (std::find(groups.begin(), groups.end(), metric.group) == groups.end())
      groups.push_back(metric.group);
  for (const std::string& group : groups) {
    text << "## " << juce::String(group) << "\n\n" << header;
    for (const MetricComparison& metric : comparison.metrics)
      if (metric.group == group)
        text << row(metric, juce::String(metric.name));
    text << "\n";
  }
  return text;
}

}  // namespace EffectMatch
