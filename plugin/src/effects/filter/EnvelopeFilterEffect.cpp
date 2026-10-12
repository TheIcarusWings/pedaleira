// filter.envelope: a resonant filter whose cutoff follows the playing
// dynamics. A peak envelope follower (with its own attack and release) is
// scaled by the sensitivity and mapped onto a sweep of `rangeOct` octaves
// above `lowHz`; "Up" opens the filter as you play harder, "Down" closes it.
#include <cmath>
#include "effects/EffectDsp.h"
#include "effects/filter/SweepFilter.h"

namespace {

class EnvelopeFilterProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "filter.envelope";

  enum Param {
    kSensitivityPct = 0,
    kAttackMs,
    kReleaseMs,
    kLowHz,
    kRangeOct,
    kDirection,
    kResonancePct,
    kFilterType,
    kLevelDb
  };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::floatParam("sensitivityPct", "Sensitivity", "%", 0.0f, 100.0f, 60.0f));
      t.push_back(BlockParamSpec::floatParam("attackMs", "Attack", "ms", 1.0f, 200.0f, 10.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 10.0f, 2000.0f, 250.0f)
                      .withSkew(0.4f));
      // Where the sweep starts (its lowest cutoff) and how far it travels.
      t.push_back(BlockParamSpec::floatParam("lowHz", "Low Freq", "Hz", 60.0f, 1000.0f, 200.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("rangeOct", "Range", "oct", 0.5f, 6.0f, 3.0f));
      t.push_back(BlockParamSpec::choiceParam("direction", "Direction", {"Up", "Down"}, 0));
      t.push_back(BlockParamSpec::floatParam("resonancePct", "Resonance", "%", 0.0f, 100.0f, 55.0f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::choiceParam("filterType", "Filter", {"Low-pass", "Band-pass"}, 0)
                      .notAutomatable());
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -12.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  EnvelopeFilterProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // The lowest cutoff of the sweep is the slowest ring.
  double tailSeconds() const noexcept override {
    return fxfilter::resonanceDecaySeconds(param(kLowHz),
                                           fxfilter::resonanceToQ(param(kResonancePct))) +
           kSmoothSeconds;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    for (auto& follower : followers)
      follower.prepare(sampleRate);
    quality.prepare(sampleRate, kSmoothSeconds);
    level.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& filter : filters)
      filter.reset();
    for (auto& follower : followers)
      follower.reset();
    quality.snapOnNextTarget();
    level.snapOnNextTarget();
    ticksLeft = 0;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    // Two channels of one stereo signal share one envelope (and so one
    // cutoff), so the image holds; in dual mono each guitar has its own.
    const bool linked = numChannels == 2 && !ctx.dualMono;
    const int numLanes = linked ? 1 : numChannels;

    for (int lane = 0; lane < numLanes; ++lane)
      followers[lane].setTimes(param(kAttackMs), param(kReleaseMs));
    quality.setTarget(fxfilter::resonanceToQ(param(kResonancePct)));
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    // Sensitivity moves the input level that opens the filter fully: from
    // -2 dB at 0 % (it takes a hard pick) to -32 dB at 100 % (a whisper).
    // Below it the sweep covers 36 dB, so soft and hard notes land in
    // different places.
    const float ceilingDb = -2.0f - 0.3f * param(kSensitivityPct);
    const float floorDb = ceilingDb - kSpanDb;
    const float rangeOct = param(kRangeOct);
    const float lowLog2 = std::log2(param(kLowHz));
    const bool down = param(kDirection) >= 0.5f;
    const auto voice = param(kFilterType) >= 0.5f ? fxfilter::SweepFilter::Voice::Bandpass
                                                  : fxfilter::SweepFilter::Voice::Lowpass;

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int i = 0; i < numSamples; ++i) {
      float detected[kMaxChannels] = {0.0f, 0.0f};
      if (linked) {
        detected[0] = followers[0].process(
            juce::jmax(std::abs(channels[0][i]), std::abs(channels[1][i])));
      } else {
        for (int lane = 0; lane < numLanes; ++lane)
          detected[lane] = followers[lane].process(channels[lane][i]);
      }
      const float q = quality.next();
      const float gain = level.next();

      // The cutoff moves on a fixed grid of samples that runs across blocks.
      if (ticksLeft == 0) {
        for (int lane = 0; lane < numLanes; ++lane) {
          // Envelope in dB onto 0..1, then onto the sweep. The dB scale is
          // how loudness is heard, so equal steps in playing strength are
          // roughly equal steps in the filter.
          const float envDb = detected[lane] > 1.0e-6f ? 20.0f * std::log10(detected[lane]) : -120.0f;
          const float x = juce::jlimit(0.0f, 1.0f, (envDb - floorDb) / kSpanDb);
          const float octaves = (down ? 1.0f - x : x) * rangeOct;
          cutoffHz[lane] = std::exp2(static_cast<double>(lowLog2) + static_cast<double>(octaves));
        }
        for (int ch = 0; ch < numChannels; ++ch)
          filters[ch].set(cutoffHz[linked ? 0 : ch], q, sampleRate, voice);
        ticksLeft = kControlGrid;
      }
      --ticksLeft;

      for (int ch = 0; ch < numChannels; ++ch)
        channels[ch][i] = gain * filters[ch].process(channels[ch][i]);
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  static constexpr float kSpanDb = 36.0f;
  static constexpr double kSmoothSeconds = 0.02;

  fxfilter::SweepFilter filters[kMaxChannels];
  fx::EnvelopeFollower followers[kMaxChannels];
  fx::SmoothedParam quality, level;
  double cutoffHz[kMaxChannels] = {200.0, 200.0};
  double sampleRate{48000.0};
  int ticksLeft{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(EnvelopeFilterEffect) {
  addEffectKind<EnvelopeFilterProcessor>(registry, "Envelope Filter", BlockCategory::Filter);
}
