#pragma once
// Measuring an amp description the way a reference amp is measured for
// fitting (docs/amp-framework.md, "Measuring an amp ... and fitting the
// description"), through the real amp.custom block at 48 kHz:
//
//   gain sweep   a 1 kHz staircase from -96 to -36 dBFS in 3 dB steps of
//                0.3 s (the test signal's staircase, -60 to 0 dBFS, played 36
//                dB down); the tone level of each step is read 0.19 to 0.29 s
//                into it. Gs is the gain of the two lowest steps, the bend the
//                input where the gain is 1 dB under Gs (interpolated), the
//                ceiling the highest output level over the staircase.
//   THD          a 1 kHz sine at -54 dBFS (-18 dBFS played 36 dB down),
//                harmonics 2 to 10 over the fundamental, read from 0.5 to 1.5 s.
//   level        the 1 kHz output level of a -18 dBFS sine.
//   response     the test signal (EffectMatch) at line level, analysed as a
//                recording: the low sweep's 1/3-octave response relative to
//                1 kHz, and the 1 kHz gain of both sweeps.
//
// The gain sweep, the THD and the response take how far below its nominal
// level the signal was played (`inputOffsetDb`): 36 dB for the gain sweep
// and the THD by default, 0 (line level) for the response.
//
// Used by amp_fitted_tests.cpp, and by its AmpFitHarness test, which
// measures any description file for a fitting loop.
#include "effects/effect_test_helpers.h"

#include "AudioMeasure.h"
#include "EffectMatch.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ampfit {

constexpr double kRate = 48000.0;
constexpr double kStaircaseNominalStartDb = -60.0;
constexpr double kStaircaseStepDb = 3.0;
constexpr int kStaircaseSteps = 21;
constexpr double kStepSeconds = 0.3;
constexpr double kThdNominalDb = -18.0;
constexpr double kDefaultOffsetDb = 36.0;

struct GainPoint {
  double smallSignalGainDb{0.0};
  std::optional<double> bendDb;
  double ceilingDb{-200.0};
  double thdPercent{0.0};
  std::vector<double> curveDb;  // output level of each staircase step
};

/** A fresh amp.custom block with `descriptionText` loaded and `knobs` set (0 to
    100 percent; anything not named stays at 50). Null when it does not load. */
inline std::unique_ptr<BlockProcessor> makeAmp(const std::string& descriptionText, const fxtest::Params& knobs,
                                               std::string& error) {
  auto amp = BlockKindRegistry::instance().create("amp.custom");
  if (amp == nullptr) {
    error = "no amp.custom kind";
    return nullptr;
  }
  juce::String why;
  if (!amp->loadDescription(juce::String(descriptionText), why)) {
    error = why.toStdString();
    return nullptr;
  }
  for (const char* id : {"gain", "bass", "middle", "treble", "presence", "depth", "master"})
    amp->setParameter(amp->findParameter(id), 50.0f);
  for (const auto& [id, value] : knobs) {
    const int index = amp->findParameter(id);
    if (index < 0) {
      error = "no parameter " + id.toStdString();
      return nullptr;
    }
    amp->setParameter(index, static_cast<float>(value));
  }
  BlockPrepareSpec spec;
  spec.chainSampleRate = kRate;
  spec.oversampleFactor = 1;
  spec.baseBlockSize = 128;
  spec.maxBlockSize = 128;
  spec.numChannels = 2;
  amp->prepareForChain(spec);
  return amp;
}

/** Mono render, block by block, as the chain would. */
inline std::vector<float> renderMono(BlockProcessor& amp, const std::vector<float>& in) {
  const juce::ScopedNoDenormals noDenormals;
  BlockProcessContext ctx;
  std::vector<float> out(in.size(), 0.0f);
  juce::AudioBuffer<float> buffer(1, 128);
  for (size_t offset = 0; offset < in.size(); offset += 128) {
    const int n = static_cast<int>(std::min<size_t>(128, in.size() - offset));
    buffer.copyFrom(0, 0, in.data() + offset, n);
    juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 1, n);
    amp.processInChain(view, ctx);
    std::copy_n(view.getReadPointer(0), n, out.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return out;
}

inline std::vector<float> sineAt(double db, double seconds) {
  const double amplitude = std::pow(10.0, db / 20.0);
  std::vector<float> x(static_cast<size_t>(seconds * kRate));
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(amplitude * std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 *
                                                   static_cast<double>(i) / kRate));
  return x;
}

/** Harmonics 2 to 10 of 1 kHz over the fundamental, percent. */
inline double thdOf(const std::vector<float>& out, size_t from, size_t count) {
  const std::vector<float> part(out.begin() + static_cast<std::ptrdiff_t>(from),
                                out.begin() + static_cast<std::ptrdiff_t>(from + count));
  return fxtest::thdPercent(part, 1000.0, kRate, 0, 10);
}

inline std::optional<GainPoint> measureGainPoint(const std::string& text, const fxtest::Params& knobs,
                                                 std::string& error, double inputOffsetDb = kDefaultOffsetDb) {
  auto amp = makeAmp(text, knobs, error);
  if (amp == nullptr)
    return std::nullopt;
  const double startDb = kStaircaseNominalStartDb - inputOffsetDb;
  // The staircase, with 2 ms ramps between the steps and a continuous phase.
  const size_t hold = static_cast<size_t>(kStepSeconds * kRate);
  const size_t ramp = static_cast<size_t>(0.002 * kRate);
  std::vector<float> in(hold * kStaircaseSteps);
  for (int step = 0; step < kStaircaseSteps; ++step) {
    const double to = std::pow(10.0, (startDb + kStaircaseStepDb * step) / 20.0);
    const double from = step == 0 ? to : std::pow(10.0, (startDb + kStaircaseStepDb * (step - 1)) / 20.0);
    for (size_t i = 0; i < hold; ++i) {
      const size_t n = static_cast<size_t>(step) * hold + i;
      const double a = i < ramp ? from + (to - from) * static_cast<double>(i) / static_cast<double>(ramp) : to;
      in[n] = static_cast<float>(a * std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 *
                                              static_cast<double>(n) / kRate));
    }
  }
  const std::vector<float> out = renderMono(*amp, in);
  GainPoint point;
  std::vector<double> gain;
  for (int step = 0; step < kStaircaseSteps; ++step) {
    const size_t at = static_cast<size_t>(step) * hold + static_cast<size_t>(0.19 * kRate);
    const double level = AudioMeasure::toneLevelDb(out.data() + at, static_cast<int>(0.1 * kRate), 1000.0, kRate);
    point.curveDb.push_back(level);
    gain.push_back(level - (startDb + kStaircaseStepDb * step));
    point.ceilingDb = std::max(point.ceilingDb, level);
  }
  point.smallSignalGainDb = 0.5 * (gain[0] + gain[1]);
  for (int step = 1; step < kStaircaseSteps; ++step)
    if (gain[static_cast<size_t>(step)] < point.smallSignalGainDb - 1.0) {
      const double above = gain[static_cast<size_t>(step - 1)] - (point.smallSignalGainDb - 1.0);
      const double drop = gain[static_cast<size_t>(step - 1)] - gain[static_cast<size_t>(step)];
      point.bendDb = startDb + kStaircaseStepDb * (step - 1 + above / drop);
      break;
    }
  // THD on a fresh amp.
  auto fresh = makeAmp(text, knobs, error);
  const std::vector<float> sine = renderMono(*fresh, sineAt(kThdNominalDb - inputOffsetDb, 1.5));
  point.thdPercent = thdOf(sine, static_cast<size_t>(0.5 * kRate), static_cast<size_t>(kRate));
  return point;
}

/** The 1 kHz output level, dBFS peak, of a sine at `inDb`; and its THD. */
inline std::optional<std::pair<double, double>> measureLevel(const std::string& text, const fxtest::Params& knobs,
                                                             double inDb, std::string& error) {
  auto amp = makeAmp(text, knobs, error);
  if (amp == nullptr)
    return std::nullopt;
  const std::vector<float> out = renderMono(*amp, sineAt(inDb, 1.0));
  const size_t from = static_cast<size_t>(0.5 * kRate), count = static_cast<size_t>(0.5 * kRate);
  const double level = AudioMeasure::toneLevelDb(out.data() + from, static_cast<int>(count), 1000.0, kRate);
  return std::make_pair(level, thdOf(out, from, count));
}

struct Sweeps {
  std::array<double, EffectMatch::kNumBands> responseLow{};  // per band, relative to 1 kHz
  double gainLowDb{0.0};   // 1 kHz gain of the low sweep (-18 dBFS nominal)
  double gainHighDb{0.0};  // and of the high sweep (-6 dBFS nominal)
};

/** The test signal played `inputOffsetDb` under its nominal level, analysed
    as PedaleiraMatch analyse reads a recording of it: the low sweep's
    response per 1/3-octave band relative to 1 kHz, and the 1 kHz gain of both
    sweeps against the nominal signal. */
inline std::optional<Sweeps> measureSweeps(const std::string& text, const fxtest::Params& knobs, std::string& error,
                                           double inputOffsetDb = 0.0) {
  auto amp = makeAmp(text, knobs, error);
  if (amp == nullptr)
    return std::nullopt;
  const EffectMatch::TestSignal signal = EffectMatch::makeTestSignal(kRate);
  std::vector<float> in = signal.samples;
  const float scale = static_cast<float>(std::pow(10.0, -inputOffsetDb / 20.0));
  for (float& v : in)
    v *= scale;
  in.resize(in.size() + static_cast<size_t>(kRate), 0.0f);
  const std::vector<float> out = renderMono(*amp, in);
  juce::AudioBuffer<float> recording(1, static_cast<int>(out.size()));
  recording.copyFrom(0, 0, out.data(), static_cast<int>(out.size()));
  try {
    const EffectMatch::MatchAnalysis analysis = EffectMatch::analyseRecording(recording, kRate, signal);
    if (!analysis.left.sweepLow || !analysis.left.sweepHigh) {
      error = "no sweeps in the analysis";
      return std::nullopt;
    }
    Sweeps sweeps;
    sweeps.responseLow = analysis.left.sweepLow->responseDb;
    sweeps.gainLowDb = analysis.left.sweepLow->levelGainDb;
    sweeps.gainHighDb = analysis.left.sweepHigh->levelGainDb;
    return sweeps;
  } catch (const EffectMatch::MatchError& e) {
    error = e.what();
    return std::nullopt;
  }
}

/** The low sweep's response at line level, per 1/3-octave band relative to
    1 kHz. */
inline std::optional<std::array<double, EffectMatch::kNumBands>> measureResponse(const std::string& text,
                                                                                  const fxtest::Params& knobs,
                                                                                  std::string& error) {
  const auto sweeps = measureSweeps(text, knobs, error);
  if (!sweeps)
    return std::nullopt;
  return sweeps->responseLow;
}

}  // namespace ampfit
