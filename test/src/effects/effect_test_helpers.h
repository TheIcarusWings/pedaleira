#pragma once
// The test harness every built-in effect is tested with (docs/effects.md):
//
//   EffectRig            the real processor holding exactly one block of a
//                        kind, built and driven through the command layer
//   measurements         impulse response, delay time, RT60, magnitude
//                        response, gain, THD, modulation rate, signal scans
//   DirectKind           one processor of a kind run on its own, no chain
//   measureKindCost      the CPU budget hook: nanoseconds per 128-sample block
//   expect...            the checks one effect at a time can reuse
//   expectKindConforms   the conformance test every effect test file calls
//
// What a rig renders has been through the whole processor, not the block
// alone: the input stage and a 5 Hz DC blocker sit around the chain. They are
// transparent for these measurements (a reference rig with an empty chain
// reproduces an impulse at 0.9997, followed by the DC blocker's -66 dB,
// 32 ms tail), and exact comparisons are made against such a reference rig
// rather than against the input. The DC blocker has memory: after the block
// under test stops adding to the signal, the rig and the reference still
// differ by that decaying residue (around 1e-4 of what was playing), so
// "equal to dry" right after a transition is a tolerance, never bit-exact.
// A block that is off from the start is bit-exact (expectBypassNull).
//
// What a block is handed depends on the rig's layout (RigOptions::layout):
//
//   Mono      a one-channel host bus: the lane buffer has ONE channel.
//   Stereo    a two-channel bus, the default: the lane buffer has TWO
//             channels and ctx.dualMono is false, which is a true stereo
//             signal. The processor folds its input to mono before the
//             chain, so a single block always RECEIVES the same audio on
//             both channels; what it writes may differ (a ping-pong, a
//             stereo reverb) and reaches the two outputs as written. A block
//             only receives channels that differ when a block before it made
//             them differ, which a one-block rig cannot show: DirectKind can.
//   DualMono  a two-channel bus in the dual input mode: TWO channels and
//             ctx.dualMono true, two independent signals that must never
//             be mixed.
//
// (The fourth case, the processor's two-lane stereo mode, runs each lane on a
// one-channel buffer: for a block it is Mono.)

#include "BlockKinds.h"
#include "EngineCommands.h"
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fxtest {

// Parameter values by id, in real units ("timeMs", 250.0). The common block
// settings (mix, inputGain, outputGain, enabled) go through the same list.
using Params = std::vector<std::pair<juce::String, double>>;

struct Stereo {
  std::vector<float> left, right;
  size_t size() const { return left.size(); }
};

// What the block under test is handed; see the notes at the top.
enum class Layout { Mono, Stereo, DualMono };

inline const char* layoutName(Layout layout) {
  return layout == Layout::Mono ? "mono" : layout == Layout::DualMono ? "dual mono" : "stereo";
}

struct RigOptions {
  // What the host runs at. The chain itself always runs at 48 kHz (times the
  // oversampling factor); another host rate is resampled around it.
  double sampleRate{48000.0};
  int blockSize{256};
  // The rig's own tempo (there is no host playhead in these tests).
  double bpm{120.0};
  Layout layout{Layout::Stereo};
};

inline RigOptions rigFor(Layout layout) {
  RigOptions options;
  options.layout = layout;
  return options;
}

// ---------------------------------------------------------------------------
// The rig
// ---------------------------------------------------------------------------

/**
 * A prepared processor with one block of `kindId` in an otherwise empty
 * chain, at the given parameter values. An empty kind id gives the reference
 * rig: the same processor with nothing in the chain.
 *
 * Everything that changes the rig goes through the command layer, as a
 * client's would. After construction the rig is settled and freshly
 * prepared: smoothers sit on their targets and nothing is fading in, so two
 * rigs built alike render alike from the first sample.
 */
class EffectRig {
public:
  explicit EffectRig(const juce::String& kindId, const Params& params = {},
                     const RigOptions& rigOptions = {})
      : options(rigOptions) {
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(busChannels(), busChannels(), options.sampleRate,
                              options.blockSize);
    // The noise gate sits before the chain and is not what is measured.
    proc.parameters.getParameter("gateEnabled")->setValueNotifyingHost(0.0f);
    proc.prepareToPlay(options.sampleRate, options.blockSize);
    must("tempo.set", "{\"bpm\":" + juce::String(options.bpm) + "}");
    if (options.layout == Layout::DualMono)
      must("chain.setInputMode", "{\"mode\":\"dual\"}");

    if (kindId.isNotEmpty()) {
      const juce::var added =
          must("chain.addBlock", "{\"kind\":" + juce::JSON::toString(juce::var(kindId)) + "}");
      id = added["blockId"].toString().toStdString();
      EXPECT_FALSE(id.empty()) << "chain.addBlock returned no block id for " << kindId;
      for (const auto& [param, value] : params)
        set(param, value);
    }
    restart();
  }

  /** Runs a command and expects it to succeed. */
  juce::var must(const char* name, const juce::String& argsJson = "{}") {
    const CommandResult result = commands.registry().run(name, juce::JSON::parse(argsJson));
    EXPECT_TRUE(result.ok) << name << " " << argsJson.toStdString() << ": "
                           << result.errorCode.toStdString() << ": "
                           << result.errorMessage.toStdString();
    return result.value;
  }

  /** block.setParam on the rig's block. The value lands on the next audio
      block; nothing is re-prepared. */
  void set(const juce::String& param, double value) {
    must("block.setParam", "{\"blockId\":" + quotedId() + ",\"param\":" +
                               juce::JSON::toString(juce::var(param)) + ",\"value\":" +
                               juce::String(value, 9) + "}");
  }

  /** The block's power switch (block.setEnabled). */
  void setEnabled(bool enabled) {
    must("block.setEnabled", "{\"blockId\":" + quotedId() + ",\"enabled\":" +
                                 juce::String(enabled ? "true" : "false") + "}");
  }

  void setTempo(double bpm) { must("tempo.set", "{\"bpm\":" + juce::String(bpm) + "}"); }

  /** Delivers what a parameter move leaves for the message thread (the
      reported tail and latency), which a test has no timer for. */
  void settle() {
    EXPECT_TRUE(waitForChainLoaded(proc));
    proc.flushDeferredBlockRefresh();
  }

  /** Like a host restarting playback: settles, then prepares again, so every
      engine and smoother starts from rest at the current settings. */
  void restart() {
    settle();
    proc.prepareToPlay(options.sampleRate, options.blockSize);
  }

  /** Renders `in` block by block. A length that is not a whole number of
      blocks ends on a shorter block (as a host's last block may), so two
      calls in a row are one continuous stream. A mono rig plays the left
      input and returns its one output on both sides. */
  Stereo render(const Stereo& in) {
    Stereo out;
    out.left.assign(in.left.size(), 0.0f);
    out.right.assign(in.left.size(), 0.0f);
    const int total = static_cast<int>(in.left.size());
    const int block = options.blockSize;
    const int bus = busChannels();
    juce::AudioBuffer<float> buffer(bus, block);
    juce::MidiBuffer midi;
    for (int offset = 0; offset < total; offset += block) {
      const int n = std::min(block, total - offset);
      buffer.copyFrom(0, 0, in.left.data() + offset, n);
      if (bus > 1)
        buffer.copyFrom(1, 0, in.right.data() + offset, n);
      juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), bus, n);
      proc.processBlock(view, midi);
      std::copy_n(view.getReadPointer(0), n, out.left.begin() + offset);
      std::copy_n(view.getReadPointer(bus > 1 ? 1 : 0), n, out.right.begin() + offset);
    }
    return out;
  }

  /** The same signal on both inputs; returns the left output. Enough for a
      kind that treats its channels alike; one with stereo behaviour is
      measured with renderBoth. */
  std::vector<float> render(const std::vector<float>& in) { return render(Stereo{in, in}).left; }

  /** The same signal on both inputs; both outputs. */
  Stereo renderBoth(const std::vector<float>& in) { return render(Stereo{in, in}); }

  /** `frames` of silence in. */
  std::vector<float> renderSilence(int frames) {
    return render(std::vector<float>(static_cast<size_t>(frames), 0.0f));
  }

  /** The block's reported latency in host samples at 48 kHz, and its tail. */
  int latencySamples() const { return proc.blockLatencySamples(id); }
  double tailSeconds() const { return proc.blockTailSeconds(id); }

  /** The block's row of the chain state (state.get), or void. */
  juce::var blockState() {
    const juce::var state = proc.getChainState(-1);
    for (const char* key : {"chain", "chainRight"})
      if (const auto* lane = state[key].getArray())
        for (const auto& item : *lane)
          if (item["blockId"].toString() == juce::String(id))
            return item;
    return {};
  }

  const std::string& blockId() const { return id; }
  double sampleRate() const { return options.sampleRate; }
  int blockSize() const { return options.blockSize; }
  Layout layout() const { return options.layout; }
  int busChannels() const { return options.layout == Layout::Mono ? 1 : 2; }

  ChainTestProcessor proc;
  EngineCommands commands{proc};

private:
  juce::String quotedId() const { return juce::JSON::toString(juce::var(juce::String(id))); }

  RigOptions options;
  std::string id;
};

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------

inline std::vector<float> impulse(int frames, int at = 0, float amplitude = 1.0f) {
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  if (at >= 0 && at < frames)
    x[static_cast<size_t>(at)] = amplitude;
  return x;
}

inline std::vector<float> sine(double seconds, double freq, float amplitude, double fs) {
  return makeSine(static_cast<int>(std::lround(seconds * fs)), freq, amplitude, fs);
}

/** Uniform white noise, the same for the same seed. */
inline std::vector<float> noise(double seconds, unsigned seed, float amplitude, double fs) {
  return makeNoise(static_cast<int>(std::lround(seconds * fs)), seed, amplitude);
}

/** `a` followed by `frames` of silence. */
inline std::vector<float> withSilence(std::vector<float> a, int frames) {
  a.resize(a.size() + static_cast<size_t>(frames), 0.0f);
  return a;
}

/** A test signal defined in seconds rather than samples, so it is the same
    sound at any host rate: five sines from 110 Hz to 3 kHz under a slow
    swell. */
inline std::vector<float> musicalSignal(double seconds, double fs, float amplitude = 0.2f) {
  const int frames = static_cast<int>(std::lround(seconds * fs));
  std::vector<float> x(static_cast<size_t>(frames));
  const double freqs[] = {110.0, 220.0, 440.0, 1000.0, 3000.0};
  for (int i = 0; i < frames; ++i) {
    const double t = i / fs;
    double v = 0.0;
    for (const double f : freqs)
      v += std::sin(2.0 * kPi * f * t);
    const double swell = 0.5 - 0.5 * std::cos(2.0 * kPi * 1.5 * t);
    x[static_cast<size_t>(i)] = amplitude * static_cast<float>(0.2 * v * (0.25 + 0.75 * swell));
  }
  return x;
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

inline float peakOf(const std::vector<float>& x, size_t begin = 0,
                    size_t end = std::numeric_limits<size_t>::max()) {
  float peak = 0.0f;
  for (size_t i = begin; i < std::min(end, x.size()); ++i)
    peak = std::max(peak, std::abs(x[i]));
  return peak;
}

inline double rmsOf(const std::vector<float>& x, size_t begin = 0,
                    size_t end = std::numeric_limits<size_t>::max()) {
  end = std::min(end, x.size());
  if (end <= begin)
    return 0.0;
  double sum = 0.0;
  for (size_t i = begin; i < end; ++i)
    sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
  return std::sqrt(sum / static_cast<double>(end - begin));
}

inline double toDb(double linear) { return 20.0 * std::log10(std::max(linear, 1.0e-15)); }

inline int peakIndexOf(const std::vector<float>& x) {
  size_t best = 0;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::abs(x[i]) > std::abs(x[best]))
      best = i;
  return static_cast<int>(best);
}

/** The rig's response to a unit impulse at sample 0, `seconds` long, from a
    fresh start. Set mix to 1 first to capture the wet path alone. */
inline std::vector<float> captureImpulseResponse(EffectRig& rig, double seconds) {
  rig.restart();
  return rig.render(impulse(static_cast<int>(std::lround(seconds * rig.sampleRate()))));
}

/** First sample of `ir` that rises above `relativeThreshold` of its peak
    (-1 for silence): where a response starts, as opposed to where it is
    loudest. */
inline int onsetIndex(const std::vector<float>& ir, float relativeThreshold = 0.01f) {
  const float peak = peakOf(ir);
  if (peak <= 0.0f)
    return -1;
  for (size_t i = 0; i < ir.size(); ++i)
    if (std::abs(ir[i]) >= relativeThreshold * peak)
      return static_cast<int>(i);
  return -1;
}

/** Delay of `out` against `in` in samples: the lag of the largest
    cross-correlation magnitude within [minLag, maxLag]. Works on any
    broadband signal (noise, a burst), for any length. */
inline int estimateDelaySamples(const std::vector<float>& out, const std::vector<float>& in,
                                int minLag, int maxLag) {
  const size_t n = std::max(out.size(), in.size()) + static_cast<size_t>(std::max(1, maxLag));
  const int order = static_cast<int>(std::ceil(std::log2(static_cast<double>(n))));
  const size_t size = size_t{1} << order;
  juce::dsp::FFT fft(order);
  using Complex = juce::dsp::Complex<float>;
  std::vector<Complex> a(size), b(size), fa(size), fb(size), corr(size);
  for (size_t i = 0; i < out.size(); ++i)
    a[i] = Complex(out[i], 0.0f);
  for (size_t i = 0; i < in.size(); ++i)
    b[i] = Complex(in[i], 0.0f);
  fft.perform(a.data(), fa.data(), false);
  fft.perform(b.data(), fb.data(), false);
  for (size_t i = 0; i < size; ++i)
    fa[i] *= std::conj(fb[i]);
  fft.perform(fa.data(), corr.data(), true);

  int bestLag = minLag;
  float best = -1.0f;
  for (int lag = std::max(0, minLag); lag <= maxLag && static_cast<size_t>(lag) < size; ++lag) {
    const float magnitude = std::abs(corr[static_cast<size_t>(lag)].real());
    if (magnitude > best) {
      best = magnitude;
      bestLag = lag;
    }
  }
  return bestLag;
}

/** RT60 in seconds from an impulse response: Schroeder backward integration,
    a line fitted to the decay between -5 and -35 dB, extrapolated to -60.
    0 when the response does not decay that far within the capture. */
inline double estimateRt60Seconds(const std::vector<float>& ir, double fs) {
  const int start = peakIndexOf(ir);
  const size_t n = ir.size();
  std::vector<double> energy(n + 1, 0.0);
  for (size_t i = n; i-- > static_cast<size_t>(start);)
    energy[i] = energy[i + 1] + static_cast<double>(ir[i]) * static_cast<double>(ir[i]);
  const double total = energy[static_cast<size_t>(start)];
  if (total <= 0.0)
    return 0.0;

  // Least-squares slope of the decay curve in dB over the fit range.
  double sumT = 0.0, sumL = 0.0, sumTT = 0.0, sumTL = 0.0;
  int count = 0;
  bool reachedEnd = false;
  for (size_t i = static_cast<size_t>(start); i < n; ++i) {
    const double level = 10.0 * std::log10(std::max(energy[i] / total, 1.0e-30));
    if (level > -5.0)
      continue;
    if (level < -35.0) {
      reachedEnd = true;
      break;
    }
    const double t = static_cast<double>(i - static_cast<size_t>(start)) / fs;
    sumT += t;
    sumL += level;
    sumTT += t * t;
    sumTL += t * level;
    ++count;
  }
  if (!reachedEnd || count < 2)
    return 0.0;
  const double slope = (count * sumTL - sumT * sumL) / (count * sumTT - sumT * sumT);
  return slope < 0.0 ? -60.0 / slope : 0.0;
}

/** Seconds after `fromSample` until `x` has dropped for good below
    `relativeDb` of `referencePeak` (the last sample still above it). */
inline double decayTimeSeconds(const std::vector<float>& x, size_t fromSample, float referencePeak,
                               double fs, double relativeDb = -60.0) {
  const float threshold = referencePeak * static_cast<float>(std::pow(10.0, relativeDb / 20.0));
  for (size_t i = x.size(); i-- > fromSample;)
    if (std::abs(x[i]) > threshold)
      return static_cast<double>(i + 1 - fromSample) / fs;
  return 0.0;
}

/** Magnitude in dB of an impulse response at each frequency (a direct DFT
    bin per frequency, so any frequency, not only FFT bins). */
inline std::vector<double> magnitudeResponseDb(const std::vector<float>& ir,
                                               const std::vector<double>& freqsHz, double fs) {
  std::vector<double> out;
  for (const double f : freqsHz) {
    std::complex<double> acc{0.0, 0.0};
    for (size_t i = 0; i < ir.size(); ++i) {
      const double phase = -2.0 * kPi * f * static_cast<double>(i) / fs;
      acc += static_cast<double>(ir[i]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    out.push_back(toDb(std::abs(acc)));
  }
  return out;
}

/** Gain in dB at one frequency measured with a sine probe: for effects whose
    response depends on level (so an impulse would not tell). Skips the first
    `settleSeconds` of the render. */
inline double sineProbeGainDb(EffectRig& rig, double freqHz, float amplitude = 0.25f,
                              double seconds = 1.0, double settleSeconds = 0.5) {
  rig.restart();
  const double fs = rig.sampleRate();
  const std::vector<float> in = sine(seconds, freqHz, amplitude, fs);
  const std::vector<float> out = rig.render(in);
  const size_t skip = static_cast<size_t>(settleSeconds * fs);
  const double inPower = goertzelPower(in.data() + skip, in.size() - skip, freqHz, fs);
  const double outPower = goertzelPower(out.data() + skip, out.size() - skip, freqHz, fs);
  return db(outPower) - db(inPower);
}

/** Broadband gain in dB: RMS of `out` over RMS of `in`, after `skip`. */
inline double steadyStateGainDb(const std::vector<float>& out, const std::vector<float>& in,
                                size_t skip) {
  return toDb(rmsOf(out, skip)) - toDb(rmsOf(in, skip));
}

/** Total harmonic distortion of a rendered sine in percent: harmonics 2 to
    `harmonics` (below Nyquist) against the fundamental. */
inline double thdPercent(const std::vector<float>& out, double fundamentalHz, double fs,
                         size_t skip = 0, int harmonics = 8) {
  const float* data = out.data() + std::min(skip, out.size());
  const size_t n = out.size() - std::min(skip, out.size());
  const double fundamental = goertzelPower(data, n, fundamentalHz, fs);
  double distortion = 0.0;
  for (int h = 2; h <= harmonics && h * fundamentalHz < 0.5 * fs; ++h)
    distortion += goertzelPower(data, n, h * fundamentalHz, fs);
  return fundamental > 0.0 ? 100.0 * std::sqrt(distortion / fundamental) : 0.0;
}

/** How often a slowly varying series swings around its mean, in Hz: upward
    crossings of the mean with hysteresis, timed from the first to the last.
    0 when it swings fewer than twice, or by less than `minRelativeSwing` of
    its mean from trough to peak (ripple, not modulation). */
inline double oscillationRateHz(const std::vector<double>& series, double seriesRate,
                                double minRelativeSwing) {
  if (series.size() < 4)
    return 0.0;
  double mean = 0.0, lo = series[0], hi = series[0];
  for (const double v : series) {
    mean += v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  mean /= static_cast<double>(series.size());
  const double band = 0.2 * (hi - lo) * 0.5;
  if (band <= 0.0 || (hi - lo) < minRelativeSwing * std::abs(mean))
    return 0.0;

  bool below = series[0] < mean;
  int crossings = 0;
  size_t first = 0, last = 0;
  for (size_t i = 0; i < series.size(); ++i) {
    if (below && series[i] > mean + band) {
      below = false;
      if (crossings++ == 0)
        first = i;
      last = i;
    } else if (!below && series[i] < mean - band) {
      below = true;
    }
  }
  if (crossings < 2)
    return 0.0;
  return static_cast<double>(crossings - 1) * seriesRate / static_cast<double>(last - first);
}

/** Rate in Hz of an amplitude modulation (tremolo, auto-pan, a swept
    filter on a sine): the swing of the signal's envelope. Feed a steady
    sine of 400 Hz or more and pass the rendered output; `skip` leaves out
    the start. Swings under `minDepth` (trough to peak over mean, 5%) count
    as no modulation. */
inline double amplitudeModulationRateHz(const std::vector<float>& out, double fs, size_t skip = 0,
                                        double minDepth = 0.05) {
  // Rectify and smooth twice at 40 Hz (the carrier's ripple ends up far
  // under the depth floor), then keep one value per millisecond.
  const double coeff = 1.0 - std::exp(-2.0 * kPi * 40.0 / fs);
  const size_t hop = static_cast<size_t>(std::max(1.0, fs / 1000.0));
  std::vector<double> envelope;
  double first = 0.0, second = 0.0;
  for (size_t i = 0; i < out.size(); ++i) {
    first += coeff * (std::abs(static_cast<double>(out[i])) - first);
    second += coeff * (first - second);
    if (i >= skip && i % hop == 0)
      envelope.push_back(second);
  }
  return oscillationRateHz(envelope, fs / static_cast<double>(hop), minDepth);
}

/** Rate in Hz of a pitch modulation (vibrato, chorus, flanger at full wet):
    the swing of the instantaneous frequency, read off the spacing of the
    rendered sine's upward zero crossings. Feed a steady sine of a few
    hundred Hz. Deviations under `minDepth` of the carrier (trough to peak,
    0.1%) count as no modulation. */
inline double frequencyModulationRateHz(const std::vector<float>& out, double fs, size_t skip = 0,
                                        double minDepth = 0.001) {
  std::vector<double> crossingTimes;
  for (size_t i = std::max<size_t>(skip, 1); i < out.size(); ++i)
    if (out[i - 1] < 0.0f && out[i] >= 0.0f) {
      const double frac = static_cast<double>(-out[i - 1]) /
                          (static_cast<double>(out[i]) - static_cast<double>(out[i - 1]));
      crossingTimes.push_back((static_cast<double>(i - 1) + frac) / fs);
    }
  if (crossingTimes.size() < 8)
    return 0.0;
  // One frequency value per carrier cycle; the carrier is nearly steady, so
  // the series is close to evenly spaced at the carrier rate.
  std::vector<double> frequency;
  for (size_t i = 1; i < crossingTimes.size(); ++i)
    frequency.push_back(1.0 / (crossingTimes[i] - crossingTimes[i - 1]));
  const double carrier = static_cast<double>(crossingTimes.size() - 1) /
                         (crossingTimes.back() - crossingTimes.front());
  return oscillationRateHz(frequency, carrier, minDepth);
}

struct SignalScan {
  bool hasNaN{false};
  bool hasInf{false};
  // Subnormal values: a filter or feedback path decaying without a floor.
  int denormals{0};
  float peak{0.0f};
  bool finite() const { return !hasNaN && !hasInf; }
};

inline SignalScan scanSignal(const std::vector<float>& x) {
  SignalScan scan;
  for (const float v : x) {
    if (std::isnan(v))
      scan.hasNaN = true;
    else if (std::isinf(v))
      scan.hasInf = true;
    else {
      if (v != 0.0f && std::abs(v) < std::numeric_limits<float>::min())
        ++scan.denormals;
      scan.peak = std::max(scan.peak, std::abs(v));
    }
  }
  return scan;
}

/** The output is finite, free of denormals and no louder than `bound`. */
inline void expectCleanSignal(const std::vector<float>& x, float bound, const char* what) {
  const SignalScan scan = scanSignal(x);
  EXPECT_FALSE(scan.hasNaN) << what << ": NaN in the output";
  EXPECT_FALSE(scan.hasInf) << what << ": inf in the output";
  EXPECT_EQ(scan.denormals, 0) << what << ": denormal samples in the output";
  EXPECT_LE(scan.peak, bound) << what << ": output peak out of bounds";
}

/** Largest step between neighbouring samples in [begin, end): a click shows
    up as a step far above what the signal's own slope explains. */
inline float maxStep(const std::vector<float>& x, size_t begin, size_t end) {
  float step = 0.0f;
  for (size_t i = std::max<size_t>(begin, 1); i < std::min(end, x.size()); ++i)
    step = std::max(step, std::abs(x[i] - x[i - 1]));
  return step;
}

// ---------------------------------------------------------------------------
// The kind alone
// ---------------------------------------------------------------------------

/**
 * One processor of a kind, built by the registry and run directly: prepared
 * the way the chain prepares it (48 kHz, no oversampling) and handed its wet
 * path one block at a time, with nothing around it. No mix, no gains, no
 * bypass, no DC blocker: what comes out is exactly what processEffect wrote.
 * For measurements the rig's own stages would blur (a tail down to -60 dB,
 * the CPU cost), and for the one case a one-block rig cannot make: a true
 * stereo buffer whose two channels DIFFER on the way in (Layout::Stereo with
 * separate left and right inputs). Everything about behaviour IN the chain
 * needs an EffectRig.
 */
class DirectKind {
public:
  explicit DirectKind(const juce::String& kindId, const Params& params = {},
                      Layout layout = Layout::Mono, int blockSize = 256, double bpm = 120.0)
      : processor(BlockKindRegistry::instance().create(kindId)), block(blockSize),
        channels(layout == Layout::Mono ? 1 : 2) {
    if (processor == nullptr) {
      ADD_FAILURE() << "cannot create kind " << kindId;
      return;
    }
    for (const auto& [param, value] : params) {
      const int index = processor->findParameter(param);
      EXPECT_GE(index, 0) << kindId << " has no parameter " << param;
      if (index >= 0)
        processor->setParameter(index, static_cast<float>(value));
    }
    ctx.transport.bpm = bpm;
    ctx.dualMono = layout == Layout::DualMono;
    prepare();
  }

  bool valid() const { return processor != nullptr; }

  /** (Re)prepare: every buffer sized, all audio state cleared. */
  void prepare() {
    BlockPrepareSpec spec;
    spec.chainSampleRate = 48000.0;
    spec.oversampleFactor = 1;
    spec.baseBlockSize = block;
    spec.maxBlockSize = block;
    spec.numChannels = 2;
    processor->prepareForChain(spec);
  }

  /** The wet output for `in`. With one channel the left input is played
      and its output returned on both sides. */
  Stereo render(const Stereo& in) {
    const juce::ScopedNoDenormals noDenormals;
    Stereo out;
    out.left.assign(in.left.size(), 0.0f);
    out.right.assign(in.left.size(), 0.0f);
    juce::AudioBuffer<float> buffer(channels, block);
    for (size_t offset = 0; offset < in.left.size(); offset += static_cast<size_t>(block)) {
      const int n = static_cast<int>(std::min(static_cast<size_t>(block), in.left.size() - offset));
      buffer.copyFrom(0, 0, in.left.data() + offset, n);
      if (channels > 1)
        buffer.copyFrom(1, 0, in.right.data() + offset, n);
      juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), channels, n);
      processor->processInChain(view, ctx);
      const auto at = static_cast<std::ptrdiff_t>(offset);
      std::copy_n(view.getReadPointer(0), n, out.left.begin() + at);
      std::copy_n(view.getReadPointer(channels > 1 ? 1 : 0), n, out.right.begin() + at);
    }
    return out;
  }

  /** The same signal on every channel; channel 0 is returned. */
  std::vector<float> render(const std::vector<float>& in) { return render(Stereo{in, in}).left; }

  /** The tail and latency as the chain reads them (the tail capped at
      kMaxBlockTailSeconds). */
  double tailSeconds() const { return processor->reportedTailSeconds(); }
  int latencySamples() const { return processor->chainLatencySamples(); }
  BlockProcessor& engine() { return *processor; }

  std::unique_ptr<BlockProcessor> processor;
  BlockProcessContext ctx;

private:
  int block;
  int channels;
};

// ---------------------------------------------------------------------------
// CPU budget
// ---------------------------------------------------------------------------

struct KindCost {
  double averageNs{0.0};
  double worstNs{0.0};
  int blocks{0};
};

/**
 * What a kind costs: nanoseconds per 128-sample stereo block at 48 kHz,
 * averaged over `blocks` blocks of noise, and the worst single block. The
 * processor is run directly (built by the registry, prepared like the chain
 * prepares it), so the number is the kind's own and not the rig's. The run
 * is repeated three times and the best pass is reported, which keeps a
 * scheduler hiccup out of the worst case. Test builds only: nothing in the
 * shipped audio path is timed.
 */
inline KindCost measureKindCost(const juce::String& kindId, const Params& params = {},
                                int blocks = 2000) {
  constexpr int kBlockSize = 128;
  KindCost best;
  best.blocks = blocks;
  std::unique_ptr<BlockProcessor> processor = BlockKindRegistry::instance().create(kindId);
  if (processor == nullptr) {
    ADD_FAILURE() << "measureKindCost: cannot create kind " << kindId;
    return best;
  }
  for (const auto& [param, value] : params) {
    const int index = processor->findParameter(param);
    if (index >= 0)
      processor->setParameter(index, static_cast<float>(value));
  }

  BlockPrepareSpec spec;
  spec.chainSampleRate = 48000.0;
  spec.oversampleFactor = 1;
  spec.baseBlockSize = kBlockSize;
  spec.maxBlockSize = kBlockSize;
  spec.numChannels = 2;

  const std::vector<float> source = makeNoise(kBlockSize * 64, 7u, 0.25f);
  juce::AudioBuffer<float> buffer(2, kBlockSize);
  BlockProcessContext ctx;
  const juce::ScopedNoDenormals noDenormals;

  best.averageNs = std::numeric_limits<double>::max();
  best.worstNs = std::numeric_limits<double>::max();
  for (int pass = 0; pass < 3; ++pass) {
    processor->prepareForChain(spec);
    double total = 0.0, worst = 0.0;
    for (int b = 0; b < blocks; ++b) {
      const float* chunk = source.data() + static_cast<size_t>(b % 64) * kBlockSize;
      buffer.copyFrom(0, 0, chunk, kBlockSize);
      buffer.copyFrom(1, 0, chunk, kBlockSize);
      const auto begin = std::chrono::steady_clock::now();
      processor->processInChain(buffer, ctx);
      const auto end = std::chrono::steady_clock::now();
      const double ns =
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
      total += ns;
      worst = std::max(worst, ns);
    }
    best.averageNs = std::min(best.averageNs, total / blocks);
    best.worstNs = std::min(best.worstNs, worst);
  }
  return best;
}

/** Measures the kind, prints the numbers and expects the average under
    `averageBudgetNs` and the worst block under `worstBudgetNs`. One 128-sample
    block at 48 kHz lasts 2,666,667 ns, so 26,667 ns is 1% of a core. */
inline KindCost expectCpuUnderBudget(const juce::String& kindId, const Params& params,
                                     double averageBudgetNs, double worstBudgetNs) {
  const KindCost cost = measureKindCost(kindId, params);
  std::cout << "[   CPU    ] " << kindId << ": average " << static_cast<long long>(cost.averageNs)
            << " ns, worst " << static_cast<long long>(cost.worstNs)
            << " ns per 128-sample block at 48 kHz ("
            << juce::String(100.0 * cost.averageNs / 2666667.0, 2) << "% of a core)" << std::endl;
  // The budgets are calibrated on the development Mac. Shared CI runners are
  // slower and noisier by an unknown factor, so there the numbers are only
  // printed: a timing assertion that fails on a busy runner tells us nothing
  // about the code. PEDALEIRA_ENFORCE_CPU_BUDGETS=1 turns them back on anywhere.
  const bool onCi = juce::SystemStats::getEnvironmentVariable("CI", {}).isNotEmpty();
  const bool enforce =
      juce::SystemStats::getEnvironmentVariable("PEDALEIRA_ENFORCE_CPU_BUDGETS", {}) == "1";
  if (onCi && !enforce) {
    std::cout << "[   CPU    ] " << kindId << ": budget not enforced on CI" << std::endl;
    return cost;
  }
  EXPECT_LE(cost.averageNs, averageBudgetNs) << kindId << ": average cost over budget";
  EXPECT_LE(cost.worstNs, worstBudgetNs) << kindId << ": worst-case block over budget";
  return cost;
}

// ---------------------------------------------------------------------------
// Reusable checks
// ---------------------------------------------------------------------------
// Each takes the rig options, so it can be run per layout; both outputs are
// checked wherever audio is.

/** With the block switched off the rig is the empty rig: bit for bit, apart
    from the latency a latent kind keeps adding while bypassed. For a kind
    with spillover this holds because nothing is ringing: the block is off
    from the start. */
inline void expectBypassNull(const juce::String& kindId, const Params& params = {},
                             const RigOptions& options = {}) {
  EffectRig rig(kindId, params, options);
  rig.setEnabled(false);
  rig.restart();
  EffectRig reference(juce::String(), {}, options);
  const std::vector<float> in = noise(1.0, 11u, 0.25f, options.sampleRate);
  const std::vector<float> other = noise(1.0, 12u, 0.25f, options.sampleRate);
  const Stereo out = rig.render(Stereo{in, other});
  const Stereo dry = reference.render(Stereo{in, other});

  const int latency = std::max(0, rig.latencySamples());
  size_t mismatches = 0;
  for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i) {
    const size_t j = i - static_cast<size_t>(latency);
    if (std::memcmp(&out.left[i], &dry.left[j], sizeof(float)) != 0 ||
        std::memcmp(&out.right[i], &dry.right[j], sizeof(float)) != 0)
      ++mismatches;
  }
  EXPECT_EQ(mismatches, 0u) << kindId << ": a bypassed block must pass the dry signal bit-exact";
}

/** Random valid values on random parameters at random block boundaries for
    `seconds` of noise: the output stays finite, free of denormals and under
    `bound`. Every kind parameter takes part (not the common gains, which
    are the chain's). */
inline void expectStableUnderParameterSweep(const juce::String& kindId, const Params& params = {},
                                            double seconds = 6.0, float bound = 4.0f,
                                            const RigOptions& options = {}, unsigned seed = 1234u) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);
  ASSERT_NE(info, nullptr);
  EffectRig rig(kindId, params, options);
  rig.set("mix", 1.0);
  rig.restart();

  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const int block = rig.blockSize();
  const int blocks = static_cast<int>(seconds * rig.sampleRate()) / block;
  const std::vector<float> in = noise(seconds, seed + 1u, 0.25f, rig.sampleRate());
  const std::vector<float> other = noise(seconds, seed + 2u, 0.25f, rig.sampleRate());

  Stereo out;
  out.left.reserve(in.size());
  out.right.reserve(in.size());
  bool latencyMoved = false;
  for (int b = 0; b < blocks; ++b) {
    // A change on about one block in four, sometimes several at once.
    while (!info->params.empty() && unit(rng) < 0.25) {
      const BlockParamSpec& spec = info->params[static_cast<size_t>(
          unit(rng) * 0.999999 * static_cast<double>(info->params.size()))];
      // Extremes as often as anything in between: that is where it breaks.
      const double pick = unit(rng);
      const double position = pick < 0.15 ? 0.0 : pick > 0.85 ? 1.0 : unit(rng);
      rig.set(spec.id, static_cast<double>(spec.min) +
                           position * (static_cast<double>(spec.max) - static_cast<double>(spec.min)));
      latencyMoved = latencyMoved || spec.affectsLatency;
    }
    // A latency change waits for the message thread; deliver it as the
    // settle timer would.
    if (latencyMoved && b % 16 == 0) {
      rig.proc.flushDeferredBlockRefresh();
      latencyMoved = false;
    }
    const auto from = static_cast<std::ptrdiff_t>(b) * block;
    const Stereo rendered =
        rig.render(Stereo{std::vector<float>(in.begin() + from, in.begin() + from + block),
                          std::vector<float>(other.begin() + from, other.begin() + from + block)});
    out.left.insert(out.left.end(), rendered.left.begin(), rendered.left.end());
    out.right.insert(out.right.end(), rendered.right.begin(), rendered.right.end());
  }
  expectCleanSignal(out.left, bound, "parameter sweep, left");
  expectCleanSignal(out.right, bound, "parameter sweep, right");
}

struct TailMeasurement {
  // What the chain is told: the kind's tail, at most kMaxBlockTailSeconds.
  double reportedSeconds{0.0};
  // Time after the input stops until the output stays 60 dB under its
  // peak (0 when not measured, see `capped`).
  double measuredSeconds{0.0};
  int latencySamples{0};
  // The kind's own tail is longer than the cap, so the report is the cap.
  bool capped{false};
};

/** Plays a noise burst into the kind, then silence: the wet output must be
    silent (60 dB under its peak) once the reported tail has passed, and the
    reported tail must not be shorter than the decay actually measured, give
    or take `toleranceSeconds`. The one exception is a tail at the cap
    (kMaxBlockTailSeconds): the kind may ring past it, the chain fades it
    there, and only the render to the cap is checked for being clean.
    Measured on the kind alone (DirectKind), on every channel of `layout`:
    the rig's DC blocker has a -60 dB tail of its own that would be counted. */
inline TailMeasurement expectSilenceAfterReportedTail(const juce::String& kindId,
                                                      const Params& params = {},
                                                      double toleranceSeconds = 0.05,
                                                      Layout layout = Layout::Mono) {
  TailMeasurement result;
  DirectKind kind(kindId, params, layout);
  if (!kind.valid())
    return result;
  const double fs = 48000.0;
  result.reportedSeconds = kind.tailSeconds();
  result.latencySamples = std::max(0, kind.latencySamples());
  result.capped = kind.engine().tailSeconds() >= kMaxBlockTailSeconds;
  EXPECT_GE(kind.engine().tailSeconds(), 0.0) << kindId << ": a negative tail";
  EXPECT_LE(result.reportedSeconds, kMaxBlockTailSeconds);

  const double burstSeconds = 0.5;
  const double latencySeconds = result.latencySamples / fs;
  const double afterSeconds = result.reportedSeconds + latencySeconds + toleranceSeconds;
  const double watchSeconds = 0.5;
  const std::vector<float> burst = noise(burstSeconds, 21u, 0.25f, fs);
  const std::vector<float> in =
      withSilence(burst, static_cast<int>(std::lround((afterSeconds + watchSeconds) * fs)));
  // The same burst on both channels: in stereo and dual mono alike each
  // channel then has a tail of its own to measure.
  const Stereo out = kind.render(Stereo{in, in});

  for (const std::vector<float>* channel : {&out.left, &out.right}) {
    expectCleanSignal(*channel, 8.0f, "tail render");
    // Against the loudest the wet output ever was: a long delay says
    // nothing until the burst is over.
    const float peak = peakOf(*channel);
    EXPECT_GT(peak, 0.0f) << kindId << ": no output for a noise burst";
    if (result.capped)
      continue;
    const double measured = decayTimeSeconds(*channel, burst.size(), peak, fs);
    result.measuredSeconds = std::max(result.measuredSeconds, measured);
    // Reported, not shorter than measured.
    EXPECT_GE(result.reportedSeconds + latencySeconds + toleranceSeconds, measured)
        << kindId << ": the reported tail is shorter than the measured decay to -60 dB (only a "
           "tail at the " << kMaxBlockTailSeconds << " s cap may be)";
    // Silent once it has passed.
    const size_t silentFrom = burst.size() + static_cast<size_t>(std::lround(afterSeconds * fs));
    EXPECT_LE(peakOf(*channel, silentFrom), peak * 0.001f)
        << kindId << ": still sounding after the reported tail";
  }
  return result;
}

enum class LatencyCheck {
  // The wet path starts exactly `latencySamples()` after the input: right
  // for every kind whose wet output contains its input (dynamics, EQ,
  // filters, drive, pitch) and for look-ahead designs.
  MatchesOnset,
  // The wet path is itself a deliberate delay (an echo, a chorus voice
  // without the dry signal): it must not start BEFORE the reported latency,
  // and may start later.
  AtMostOnset,
  // Nothing to measure (the kind stays silent for an impulse).
  Skip
};

/** Reported latency against the wet impulse response (see LatencyCheck),
    on both outputs. */
inline void expectLatencyMatches(const juce::String& kindId, const Params& params,
                                 LatencyCheck check, int toleranceSamples = 1,
                                 const RigOptions& options = {}) {
  if (check == LatencyCheck::Skip)
    return;
  EffectRig rig(kindId, params, options);
  rig.set("mix", 1.0);
  rig.restart();
  const int frames = static_cast<int>(std::lround(2.0 * rig.sampleRate()));
  const Stereo ir = rig.renderBoth(impulse(frames));
  EffectRig reference(juce::String(), {}, options);
  const int origin = onsetIndex(reference.render(impulse(frames / 20)));
  const int reported = rig.latencySamples();
  ASSERT_GE(origin, 0);
  ASSERT_GE(reported, 0);
  for (const std::vector<float>* channel : {&ir.left, &ir.right}) {
    const int onset = onsetIndex(*channel);
    ASSERT_GE(onset, 0) << kindId << ": no response to an impulse; use LatencyCheck::Skip if "
                           "the kind is meant to stay silent for one";
    if (check == LatencyCheck::MatchesOnset)
      EXPECT_NEAR(onset - origin, reported, toleranceSamples)
          << kindId << ": the wet path starts " << (onset - origin)
          << " samples after the input, the kind reports " << reported;
    else
      EXPECT_GE(onset - origin, reported - toleranceSamples)
          << kindId << ": the wet path starts before the reported latency";
  }
}

/** Saves the rig's state, restores it into a fresh processor, and expects
    the same parameter values and the same audio. `changed` should move
    parameters off their defaults, or the round trip proves little. */
inline void expectStateRoundTrip(const juce::String& kindId, const Params& changed) {
  EffectRig original(kindId, changed);
  juce::MemoryBlock saved;
  original.proc.getStateInformation(saved);
  const juce::var before = original.blockState();
  ASSERT_TRUE(before.isObject());

  EffectRig restored{juce::String()};
  restored.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  restored.restart();

  // The restored block (same id) carries the same kind and values.
  juce::var after;
  {
    const juce::var state = restored.proc.getChainState(-1);
    if (const auto* lane = state["chain"].getArray())
      for (const auto& item : *lane)
        if (item["blockId"].toString() == juce::String(original.blockId()))
          after = item;
  }
  ASSERT_TRUE(after.isObject()) << kindId << ": the block did not come back from saved state";
  EXPECT_EQ(after["kindId"].toString(), kindId);
  EXPECT_EQ(juce::JSON::toString(after["kindParams"]), juce::JSON::toString(before["kindParams"]));
  EXPECT_EQ(juce::JSON::toString(after["params"]), juce::JSON::toString(before["params"]));

  // And sounds the same.
  original.restart();
  const std::vector<float> in = musicalSignal(1.0, 48000.0);
  const std::vector<float> a = original.render(in);
  const std::vector<float> b = restored.render(in);
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, std::abs(a[i] - b[i]));
  EXPECT_LE(worst, 1.0e-6f) << kindId << ": a restored block renders differently";
}

/** Two fresh rigs render the same input to the same bits, and so does one
    rig restarted: nothing is seeded from the clock, and a prepare resets
    everything. */
inline void expectDeterministic(const juce::String& kindId, const Params& params = {},
                                const RigOptions& options = {}) {
  const std::vector<float> in = musicalSignal(1.5, options.sampleRate);
  const auto same = [](const Stereo& a, const Stereo& b) {
    return std::memcmp(a.left.data(), b.left.data(), a.left.size() * sizeof(float)) == 0 &&
           std::memcmp(a.right.data(), b.right.data(), a.right.size() * sizeof(float)) == 0;
  };
  EffectRig first(kindId, params, options);
  EffectRig second(kindId, params, options);
  const Stereo a = first.renderBoth(in);
  const Stereo b = second.renderBoth(in);
  EXPECT_TRUE(same(a, b)) << kindId << ": two renders of the same rig differ";
  first.restart();
  const Stereo c = first.renderBoth(in);
  EXPECT_TRUE(same(a, c))
      << kindId << ": a restarted rig renders differently (state survives a prepare)";
}

/** The same sound whatever the host's block size and sample rate: the RMS
    difference between a 32-sample and a 512-sample render stays under
    `blockToleranceDb` of the signal, and the output level at 44.1 kHz is
    within `rateToleranceDb` of the one at 48 kHz (the chain runs at 48 kHz
    either way; the host rate is resampled around it). Both outputs. */
inline void expectRateAndBlockSizeIndependent(const juce::String& kindId, const Params& params = {},
                                              double blockToleranceDb = -60.0,
                                              double rateToleranceDb = 0.5,
                                              Layout layout = Layout::Stereo) {
  const double seconds = 2.0;
  const auto renderAt = [&](double fs, int block) {
    RigOptions options = rigFor(layout);
    options.sampleRate = fs;
    options.blockSize = block;
    EffectRig rig(kindId, params, options);
    return rig.renderBoth(musicalSignal(seconds, fs));
  };
  const Stereo smallBlocks = renderAt(48000.0, 32);
  const Stereo large = renderAt(48000.0, 512);
  const Stereo other = renderAt(44100.0, 256);

  const auto compare = [&](const std::vector<float>& a, const std::vector<float>& b,
                           const std::vector<float>& c, const char* side) {
    expectCleanSignal(a, 8.0f, "block 32");
    expectCleanSignal(b, 8.0f, "block 512");
    expectCleanSignal(c, 8.0f, "44.1 kHz");
    std::vector<float> difference(a.size());
    for (size_t i = 0; i < a.size(); ++i)
      difference[i] = a[i] - b[i];
    const double level = rmsOf(a);
    EXPECT_GT(level, 0.0) << side;
    EXPECT_LE(toDb(rmsOf(difference)) - toDb(level), blockToleranceDb)
        << kindId << " (" << side << "): the output depends on the host block size";
    EXPECT_NEAR(toDb(rmsOf(c)), toDb(level), rateToleranceDb)
        << kindId << " (" << side << "): the output level depends on the host sample rate";
  };
  compare(smallBlocks.left, large.left, other.left, "left");
  compare(smallBlocks.right, large.right, other.right, "right");
}

/** Dual mono is two instruments through one rig. Audio on the left only
    must leave the right output exactly as the empty rig has it, and with
    two different signals each output must be what a mono rig makes of that
    channel's signal alone (within `toleranceDb` of its level): no cross-feed
    and no stereo treatment, which belongs to a true stereo buffer only. */
inline void expectDualMonoIsTwoMonoRigs(const juce::String& kindId, const Params& params = {},
                                        double toleranceDb = -60.0) {
  const RigOptions dual = rigFor(Layout::DualMono);
  const std::vector<float> a = musicalSignal(1.0, 48000.0);
  const std::vector<float> b = noise(1.0, 77u, 0.2f, 48000.0);
  {
    EffectRig rig(kindId, params, dual);
    EffectRig reference(juce::String(), {}, dual);
    const std::vector<float> silence(a.size(), 0.0f);
    const Stereo out = rig.render(Stereo{a, silence});
    const Stereo dry = reference.render(Stereo{a, silence});
    expectCleanSignal(out.left, 8.0f, "dual mono left");
    EXPECT_GT(peakOf(out.left), 0.0f);
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
      worst = std::max(worst, std::abs(out.right[i] - dry.right[i]));
    EXPECT_LE(worst, 1.0e-7f) << kindId << ": the left channel leaks into the right in dual mono";
  }

  EffectRig rig(kindId, params, dual);
  EffectRig monoA(kindId, params, rigFor(Layout::Mono));
  EffectRig monoB(kindId, params, rigFor(Layout::Mono));
  const Stereo out = rig.render(Stereo{a, b});
  const std::vector<float> wantLeft = monoA.render(a);
  const std::vector<float> wantRight = monoB.render(b);
  const auto relativeDb = [](const std::vector<float>& got, const std::vector<float>& want) {
    std::vector<float> difference(got.size());
    for (size_t i = 0; i < got.size(); ++i)
      difference[i] = got[i] - want[i];
    return toDb(rmsOf(difference)) - toDb(rmsOf(want));
  };
  EXPECT_LE(relativeDb(out.left, wantLeft), toleranceDb)
      << kindId << ": the left channel in dual mono is not what a mono rig renders";
  EXPECT_LE(relativeDb(out.right, wantRight), toleranceDb)
      << kindId << ": the right channel in dual mono is not what a mono rig renders";
}

/** A true stereo buffer whose channels differ on the way in (what a block
    gets behind a stereo IR or another stereo effect), which no one-block rig
    produces: run on the kind alone. The output is clean on both channels
    and a second run after a prepare repeats it bit for bit. */
inline void expectHandlesTrueStereoInput(const juce::String& kindId, const Params& params = {}) {
  DirectKind kind(kindId, params, Layout::Stereo);
  if (!kind.valid())
    return;
  const Stereo in{musicalSignal(1.5, 48000.0), noise(1.5, 55u, 0.2f, 48000.0)};
  const Stereo first = kind.render(in);
  expectCleanSignal(first.left, 8.0f, "true stereo left");
  expectCleanSignal(first.right, 8.0f, "true stereo right");
  EXPECT_GT(peakOf(first.left) + peakOf(first.right), 0.0f);
  kind.prepare();
  const Stereo second = kind.render(in);
  EXPECT_EQ(first.left, second.left);
  EXPECT_EQ(first.right, second.right);
}

// ---------------------------------------------------------------------------
// Conformance
// ---------------------------------------------------------------------------

struct ConformanceOptions {
  // Values to run the audio checks at, on top of the kind's defaults: pick a
  // setting where the effect clearly does something.
  Params params;
  // Values for the state round trip; every one should differ from its
  // default. Empty: `params`.
  Params stateParams;

  // CPU ceilings per 128-sample block at 48 kHz, deliberately generous (the
  // suite runs in parallel on shared machines): 1.5% of a core on average,
  // 15% for the worst block. Tighten per effect once its cost is known.
  double cpuAverageBudgetNs{40000.0};
  double cpuWorstBudgetNs{400000.0};

  LatencyCheck latency{LatencyCheck::MatchesOnset};
  int latencyToleranceSamples{1};

  // How much shorter than the measured decay the reported tail may be.
  double tailToleranceSeconds{0.05};

  // Peak allowed during the parameter sweep, for noise at 0.25 in.
  float sweepBound{4.0f};
  double sweepSeconds{4.0};

  // See expectRateAndBlockSizeIndependent. Loosen blockToleranceDb for a
  // kind with random modulation; never to hide a per-block control update.
  double blockToleranceDb{-60.0};
  double rateToleranceDb{0.5};

  // See expectDualMonoIsTwoMonoRigs. Off only for a kind that makes sound
  // without input (none so far); loosen the tolerance only for a kind whose
  // channels carry independent random sources.
  bool checkDualMono{true};
  double dualMonoToleranceDb{-60.0};

  // The tail check (silent after the reported tail, and the reported tail not
  // shorter than the decay to -60 dB). Off only with a reason written at the
  // call: the amp.heavyhot description rings below -60 dB for over two seconds.
  bool checkTail{true};
};

/**
 * What every built-in effect must satisfy; each effect's test file calls it
 * once (the registration test fails for a kind without the call) and then
 * adds what is specific to the effect.
 *
 * Once:
 *   registered      found in the registry, addable, named, its id is
 *                   <category>.<name> with its own category
 *   parameters      lowerCamel ids, defaults inside ranges and on the grid,
 *                   every float has a unit, every choice has labels, the
 *                   instance reports the registered table
 *   state           saves and restores to the same values and audio
 *   CPU             under budget; the numbers are printed
 *   dual mono       no cross-feed, and each channel is what a mono rig makes
 *                   of it
 *   stereo input    clean and repeatable on a true stereo buffer whose
 *                   channels differ
 *
 * In each layout a block can be handed (mono, true stereo, dual mono; see
 * the notes at the top), on both outputs:
 *   stability       finite, bounded, denormal-free under a parameter sweep
 *   tail            silent after the reported tail; the reported tail is not
 *                   shorter than the measured decay to -60 dB, unless it is
 *                   the 30 s cap
 *   latency         the reported latency is where the wet path starts
 *   bypass          switched off, the rig is bit-exact the empty rig
 *   determinism     two renders are bit-identical, also after a restart
 *   independence    same output at block 32 and 512, same level at 44.1 kHz
 */
inline void expectKindConforms(const juce::String& kindId, const ConformanceOptions& options = {}) {
  SCOPED_TRACE("expectKindConforms(" + kindId.toStdString() + ")");

  // ---- Registered ----------------------------------------------------------
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);
  ASSERT_NE(info, nullptr) << "not registered: is the file under plugin/src/effects and does it "
                              "define PEDALEIRA_EFFECT_KINDS(<FileStem>)?";
  EXPECT_TRUE(info->addable);
  EXPECT_TRUE(info->name.isNotEmpty());
  const juce::String category = blockCategoryToString(info->category);
  EXPECT_TRUE(kindId.startsWith(category + ".") && kindId.length() > category.length() + 1)
      << "the kind id must be <category>.<name>; category is '" << category << "'";
  EXPECT_TRUE(kindId.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789."))
      << "kind ids are lower case, digits and dots";
  EXPECT_GE(info->defaultMix, 0.0f);
  EXPECT_LE(info->defaultMix, 1.0f);

  // ---- Parameter table -----------------------------------------------------
  for (size_t i = 0; i < info->params.size(); ++i) {
    const BlockParamSpec& spec = info->params[i];
    SCOPED_TRACE("parameter " + spec.id.toStdString());
    EXPECT_TRUE(spec.id.isNotEmpty() && juce::CharacterFunctions::isLowerCase(spec.id[0]) &&
                spec.id.containsOnly(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"))
        << "parameter ids are lowerCamel";
    EXPECT_TRUE(spec.name.isNotEmpty());
    EXPECT_LT(spec.min, spec.max);
    EXPECT_GE(spec.defaultValue, spec.min);
    EXPECT_LE(spec.defaultValue, spec.max);
    EXPECT_EQ(spec.clamp(spec.defaultValue), spec.defaultValue) << "the default is off the grid";
    EXPECT_GT(spec.skew, 0.0f);
    if (spec.type == BlockParamType::Float)
      EXPECT_TRUE(spec.unit.isNotEmpty()) << "a float parameter needs a unit (dB, ms, Hz, %, ...)";
    if (spec.type == BlockParamType::Choice) {
      EXPECT_GE(spec.choices.size(), 2);
      EXPECT_EQ(static_cast<int>(spec.max), spec.choices.size() - 1);
    }
    for (size_t j = 0; j < i; ++j)
      EXPECT_NE(info->params[j].id, spec.id) << "duplicate parameter id";
    if (spec.id == kSpilloverParamId)
      EXPECT_EQ(i, info->params.size() - 1) << "spilloverParam() goes last in the table";
  }
  {
    const std::unique_ptr<BlockProcessor> instance = BlockKindRegistry::instance().create(kindId);
    ASSERT_NE(instance, nullptr);
    EXPECT_STREQ(instance->kindId(), kindId.toRawUTF8());
    EXPECT_EQ(instance->supportsSpillover(), info->spillover);
    const BlockParamTable& table = instance->parameterTable();
    ASSERT_EQ(table.size(), info->params.size())
        << "the processor's table is not the registered one";
    for (size_t i = 0; i < table.size(); ++i) {
      EXPECT_EQ(table[i].id, info->params[i].id);
      EXPECT_EQ(instance->getParameter(static_cast<int>(i)), table[i].defaultValue)
          << "a new processor starts at the defaults (" << table[i].id << ")";
    }
    // A kind that says its tail follows nothing must not have one that
    // moves with a flagged parameter missing; the reverse cannot be checked
    // generically, so this only catches a tail on a kind with no flag at all.
    bool anyTailFlag = false;
    for (const auto& spec : table)
      anyTailFlag = anyTailFlag || spec.affectsTail;
    if (instance->supportsSpillover())
      EXPECT_TRUE(anyTailFlag || instance->tailSeconds() > 0.0)
          << "a kind with spillover reports a tail";
  }
  if (::testing::Test::HasFatalFailure())
    return;

  // ---- Once ----------------------------------------------------------------
  {
    SCOPED_TRACE("state round trip");
    expectStateRoundTrip(kindId, options.stateParams.empty() ? options.params : options.stateParams);
  }
  {
    SCOPED_TRACE("cpu");
    expectCpuUnderBudget(kindId, options.params, options.cpuAverageBudgetNs,
                         options.cpuWorstBudgetNs);
  }
  if (options.checkDualMono) {
    SCOPED_TRACE("dual mono");
    expectDualMonoIsTwoMonoRigs(kindId, options.params, options.dualMonoToleranceDb);
  }
  {
    SCOPED_TRACE("true stereo input");
    expectHandlesTrueStereoInput(kindId, options.params);
  }

  // ---- In every layout -------------------------------------------------------
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(std::string("layout: ") + layoutName(layout));
    const RigOptions rig = rigFor(layout);
    {
      SCOPED_TRACE("parameter sweep");
      expectStableUnderParameterSweep(kindId, options.params, options.sweepSeconds,
                                      options.sweepBound, rig);
    }
    if (options.checkTail) {
      SCOPED_TRACE("tail");
      const TailMeasurement tail = expectSilenceAfterReportedTail(
          kindId, options.params, options.tailToleranceSeconds, layout);
      std::cout << "[   TAIL   ] " << kindId << " (" << layoutName(layout) << "): reported "
                << juce::String(tail.reportedSeconds, 3) << " s, "
                << (tail.capped ? juce::String("capped, not measured")
                                : "measured " + juce::String(tail.measuredSeconds, 3) +
                                      " s to -60 dB")
                << std::endl;
    }
    {
      SCOPED_TRACE("latency");
      expectLatencyMatches(kindId, options.params, options.latency,
                           options.latencyToleranceSamples, rig);
    }
    {
      SCOPED_TRACE("bypass null");
      expectBypassNull(kindId, options.params, rig);
    }
    {
      SCOPED_TRACE("determinism");
      expectDeterministic(kindId, options.params, rig);
    }
    {
      SCOPED_TRACE("rate and block size");
      expectRateAndBlockSizeIndependent(kindId, options.params, options.blockToleranceDb,
                                        options.rateToleranceDb, layout);
    }
  }
}

}  // namespace fxtest
