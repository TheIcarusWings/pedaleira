// pitch.transpose "Transpose" (plugin/src/effects/pitch/Transpose.cpp): the
// conformance test, then what a transpose is for: the interval within a cent
// on sines at the whole-semitone steps of its range, polyphony, a latency that
// is exactly what it reports at every interval and lower than the wide
// shifter's, an exact delay at 0, and a stepped transpose that does not click.
#include "effects/pitch_test_signals.h"

#include <gtest/gtest.h>

using namespace pitchtest;

namespace {

constexpr const char* kKind = "pitch.transpose";

const BlockParamSpec& specOf(const BlockKindInfo& info, const char* id) {
  for (const auto& spec : info.params)
    if (spec.id == id)
      return spec;
  ADD_FAILURE() << "no parameter " << id;
  return info.params.front();
}

}  // namespace

TEST(TransposeTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"semitones", -5.0}, {"cents", 10.0}};
  options.stateParams = {{"semitones", 7.0}, {"cents", -23.0}, {"quality", 2.0}};
  // The 1 % onset of an impulse read between samples (cubic interpolation) comes
  // a sample or two before its peak, which is exactly on the reported latency.
  options.latencyToleranceSamples = 4;
  options.cpuAverageBudgetNs = 25000.0;
  options.cpuWorstBudgetNs = 250000.0;
  expectKindConforms("pitch.transpose", options);
}

TEST(TransposeTest, TableMatchesTheBrief) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kKind);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, "Transpose");
  EXPECT_EQ(info->category, BlockCategory::Pitch);
  // Always fully wet: a block starts at 100 %, the wet replacing the dry.
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);

  const BlockParamSpec& transpose = specOf(*info, "semitones");
  EXPECT_FLOAT_EQ(transpose.min, -12.0f);
  EXPECT_FLOAT_EQ(transpose.max, 12.0f);
  EXPECT_FLOAT_EQ(transpose.step, 1.0f);  // semitone steps
  EXPECT_FLOAT_EQ(transpose.defaultValue, 0.0f);
  const BlockParamSpec& fine = specOf(*info, "cents");
  EXPECT_EQ(fine.unit, "ct");
  EXPECT_FLOAT_EQ(fine.min, -50.0f);
  EXPECT_FLOAT_EQ(fine.max, 50.0f);
  const BlockParamSpec& quality = specOf(*info, "quality");
  EXPECT_EQ(quality.type, BlockParamType::Choice);
  EXPECT_EQ(quality.choices.size(), 4);
  EXPECT_TRUE(quality.affectsLatency);
  EXPECT_FALSE(quality.automatable);
  EXPECT_EQ(info->params.size(), 3u);
}

TEST(TransposeTest, EveryWholeSemitoneOfTheRangeIsWithinACent) {
  // Sines at three notes, every transpose from -12 to +12 (the zero is an
  // exact delay, tested below): the measured frequency against the exact
  // ratio.
  double worst = 0.0;
  std::string worstCase;
  for (int semitones = -12; semitones <= 12; ++semitones) {
    if (semitones == 0)
      continue;
    for (const double hz : {110.0, 196.0, 330.0}) {
      EffectRig rig(kKind, {{"semitones", static_cast<double>(semitones)}, {"mix", 1.0}});
      const std::vector<float> out = rig.render(sine(2.5, hz, 0.3f, kRate));
      const double target = shifted(hz, semitones);
      const double error = centsBetween(peakFrequencyHz(out, 48000, out.size(), target), target);
      if (std::abs(error) > worst) {
        worst = std::abs(error);
        worstCase = std::to_string(semitones) + " st at " + std::to_string(hz) + " Hz";
      }
      EXPECT_NEAR(error, 0.0, 1.0) << semitones << " st at " << hz << " Hz";
    }
  }
  std::cout << "[ TRANSPOSE] worst interval error over 24 transposes x 3 notes: " << juce::String(worst, 3)
            << " ct (" << worstCase << ")" << std::endl;
}

TEST(TransposeTest, FineCentsAddToTheTransposeWithinACent) {
  double worst = 0.0;
  for (const double cents : {-50.0, -23.0, -1.0, 1.0, 17.0, 50.0}) {
    for (const double semitones : {-5.0, 0.0, 3.0}) {
      EffectRig rig(kKind, {{"semitones", semitones}, {"cents", cents}, {"mix", 1.0}});
      const std::vector<float> out = rig.render(sine(2.5, 220.0, 0.3f, kRate));
      const double target = shifted(220.0, semitones + cents * 0.01);
      const double error = centsBetween(peakFrequencyHz(out, 48000, out.size(), target), target);
      worst = std::max(worst, std::abs(error));
      EXPECT_NEAR(error, 0.0, 1.0) << semitones << " st " << cents << " ct";
    }
  }
  std::cout << "[ TRANSPOSE] worst fine-setting error: " << juce::String(worst, 3) << " ct" << std::endl;
}

TEST(TransposeTest, AChordComesOutAsTheTransposedChordNotAsOneNote) {
  // C, E and G (131, 165, 196 Hz) a whole tone down. The engine splices the
  // signal and never listens for a pitch, so all three move; pure sines are its
  // hardest chord (every splice is a compromise between three periods), so the
  // bound is loose: each shifted line is within 25 cents of the exact one and
  // at least 10 dB over what is left at the original pitch.
  std::vector<float> chord(static_cast<size_t>(3.0 * kRate), 0.0f);
  const double notes[] = {130.81, 164.81, 196.0};
  for (const double hz : notes) {
    const std::vector<float> one = sine(3.0, hz, 0.1f, kRate);
    for (size_t i = 0; i < chord.size(); ++i)
      chord[i] += one[i];
  }
  EffectRig rig(kKind, {{"semitones", -2.0}, {"mix", 1.0}});
  const std::vector<float> out = rig.render(chord);
  for (const double hz : notes) {
    const double target = shifted(hz, -2.0);
    const double error = centsBetween(peakFrequencyHz(out, 48000, out.size(), target), target);
    std::cout << "[ TRANSPOSE] chord note " << hz << " Hz: " << juce::String(error, 1) << " ct, shifted line "
              << juce::String(lineDb(out, 48000, out.size(), target) - lineDb(chord, 48000, 144000, hz), 1)
              << " dB, left at the old pitch "
              << juce::String(lineDb(out, 48000, out.size(), hz) - lineDb(chord, 48000, 144000, hz), 1) << " dB" << std::endl;
    EXPECT_NEAR(error, 0.0, 25.0) << hz;
    EXPECT_GT(lineDb(out, 48000, out.size(), target), lineDb(out, 48000, out.size(), hz) + 10.0) << hz;
  }
}

TEST(TransposeTest, MonoStereoAndDualMonoAllTranspose) {
  const std::vector<float> in = harmonicTone(2.0, 196.0, 0.3f);
  for (const Layout layout : {Layout::Mono, Layout::Stereo, Layout::DualMono}) {
    SCOPED_TRACE(layoutName(layout));
    EffectRig rig(kKind, {{"semitones", -7.0}, {"mix", 1.0}}, rigFor(layout));
    const Stereo out = rig.renderBoth(in);
    EXPECT_NEAR(centsBetween(peakFrequencyHz(out.left, 48000, out.left.size(), shifted(196.0, -7.0)), shifted(196.0, -7.0)),
                0.0, 3.0);
    if (layout != Layout::Mono)
      EXPECT_NEAR(centsBetween(peakFrequencyHz(out.right, 48000, out.right.size(), shifted(196.0, -7.0)),
                               shifted(196.0, -7.0)),
                  0.0, 3.0);
  }
}

TEST(TransposeTest, ItIsFullyWetByDefault) {
  EffectRig rig(kKind, {{"semitones", 7.0}});
  EXPECT_NEAR(static_cast<double>(rig.blockState()["params"]["mix"]), 1.0, 1e-6);
  const std::vector<float> in = sine(2.0, 220.0, 0.3f, kRate);
  const std::vector<float> out = rig.render(in);
  EXPECT_LT(lineDb(out, 48000, out.size(), 220.0), lineDb(in, 48000, 96000, 220.0) - 35.0);
}

TEST(TransposeTest, ZeroIsAnExactDelayByTheReportedLatency) {
  for (int quality = 0; quality < 4; ++quality) {
    SCOPED_TRACE("quality " + std::to_string(quality));
    EffectRig rig(kKind, {{"quality", static_cast<double>(quality)}, {"mix", 1.0}});
    rig.settle();
    rig.restart();
    EffectRig reference{juce::String()};
    const std::vector<float> in = noise(1.0, 31u, 0.25f, kRate);
    const std::vector<float> out = rig.render(in);
    const std::vector<float> dry = reference.render(in);
    const int latency = rig.latencySamples();
    double worst = 0.0;
    for (size_t i = static_cast<size_t>(latency); i < in.size(); ++i)
      worst = std::max(worst, static_cast<double>(std::abs(out[i] - dry[i - static_cast<size_t>(latency)])));
    EXPECT_LT(worst, 1.0e-6);
  }
}

TEST(TransposeTest, TheAttackArrivesAtTheReportedLatencyAtEveryTranspose) {
  // An impulse, at every whole transpose, on each quality: the peak and the
  // onset are exactly `latencySamples()` after the input.
  for (int quality = 0; quality < 4; ++quality) {
    EffectRig probe(kKind, {{"quality", static_cast<double>(quality)}});
    probe.settle();
    probe.restart();
    const int reported = probe.latencySamples();
    std::cout << "[ LATENCY  ] pitch.transpose quality " << quality << ": " << reported << " samples ("
              << juce::String(reported / 48.0, 2) << " ms)" << std::endl;
    for (int semitones = -12; semitones <= 12; ++semitones) {
      SCOPED_TRACE("quality " + std::to_string(quality) + ", " + std::to_string(semitones) + " st");
      EffectRig rig(kKind, {{"quality", static_cast<double>(quality)},
                            {"semitones", static_cast<double>(semitones)}, {"mix", 1.0}});
      rig.settle();
      EffectRig reference{juce::String()};
      const std::vector<float> ir = captureImpulseResponse(rig, 0.3);
      const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
      EXPECT_EQ(rig.latencySamples(), reported);
      EXPECT_EQ(peakIndexOf(ir) - origin, reported);
      EXPECT_LE(onsetIndex(ir) - origin, reported);
      EXPECT_GE(onsetIndex(ir) - origin, reported - 4);
    }
    // And with a fine setting on top, at the ends of the range.
    for (const double cents : {-50.0, 37.0}) {
      EffectRig rig(kKind, {{"quality", static_cast<double>(quality)}, {"semitones", 12.0}, {"cents", cents}, {"mix", 1.0}});
      rig.settle();
      EffectRig reference{juce::String()};
      const std::vector<float> ir = captureImpulseResponse(rig, 0.3);
      const int origin = onsetIndex(captureImpulseResponse(reference, 0.05));
      EXPECT_EQ(peakIndexOf(ir) - origin, rig.latencySamples()) << "quality " << quality << ", +12 st " << cents << " ct";
    }
  }
}

TEST(TransposeTest, TheLowLatencyQualityIsLowerThanTheWideShiftersAndTheQualityMovesIt) {
  // On the engine's 30 ms window the hardest landing is a small up-shift
  // (about +1 st), which both blocks have, so they report the same 446
  // samples; the 20 ms "Low latency" window is where the narrower range pays:
  // 299 samples against the shifter's 386 on the same window.
  EffectRig transpose(kKind);
  EffectRig shifter("pitch.shifter");
  EXPECT_EQ(transpose.latencySamples(), shifter.latencySamples());
  shifter.set("window", 0.0);
  shifter.settle();
  transpose.set("quality", 0.0);
  transpose.settle();
  EXPECT_LT(transpose.latencySamples(), shifter.latencySamples());
  transpose.set("quality", 1.0);
  transpose.settle();
  // The quality is latched until the framework commits it.
  const int standard = transpose.latencySamples();
  transpose.set("quality", 0.0);
  EXPECT_EQ(transpose.latencySamples(), standard);
  transpose.settle();
  const int low = transpose.latencySamples();
  EXPECT_LT(low, standard);
  transpose.set("quality", 3.0);
  transpose.settle();
  EXPECT_GT(transpose.latencySamples(), standard);
  std::cout << "[ LATENCY  ] transpose " << low << " / " << standard << " / ... / " << transpose.latencySamples()
            << " samples against the shifter's " << shifter.latencySamples() << std::endl;
}

TEST(TransposeTest, SteppingTheTransposeUnderAudioDoesNotClick) {
  // From -12 to +12 and back in single semitones, 120 ms apart, over a 220 Hz
  // sine at 0.25: the largest step between neighbouring samples is within what
  // the highest note involved (440 Hz at 0.25: 0.0144 per sample) explains, with
  // room for the engine's own splices.
  EffectRig rig(kKind, {{"semitones", -12.0}, {"mix", 1.0}});
  const std::vector<float> tone = sine(0.12 * 49.0, 220.0, 0.25f, kRate);
  std::vector<float> out;
  const size_t chunk = static_cast<size_t>(0.12 * kRate);
  for (int step = 0; step < 49; ++step) {
    const int semitones = step <= 24 ? -12 + step : 12 - (step - 24);
    rig.set("semitones", semitones);
    const std::vector<float> part =
        rig.render(std::vector<float>(tone.begin() + static_cast<std::ptrdiff_t>(step * chunk),
                                      tone.begin() + static_cast<std::ptrdiff_t>((step + 1) * chunk)));
    out.insert(out.end(), part.begin(), part.end());
  }
  expectCleanSignal(out, 0.25f * 1.2f, "stepped transpose");
  const float largest = maxStep(out, 9600, out.size());
  std::cout << "[ TRANSPOSE] largest step across 48 semitone changes: " << largest << std::endl;
  EXPECT_LT(largest, 0.04f);
}
