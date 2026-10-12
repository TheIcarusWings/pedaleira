// eq.graphic10: a ten-band octave graphic equaliser, 31.5 Hz to 16 kHz, +-12
// dB per band plus an overall level.
//
// Each band is a peaking biquad at a fixed centre and a fixed Q, in series.
// With every slider at zero the peaking sections reduce to an exact identity
// (numerator and denominator are the same numbers), so a flat setting is
// unity. A slider move glides over 20 ms and the coefficients are rebuilt on
// a fixed 16-sample grid while it does, so the response never steps and the
// sound does not depend on the block size.
//
// The sections run in double precision. A 31.5 Hz resonance has its poles a
// thousandth from the unit circle, and a float biquad turns its rounding
// error into noise 60 to 70 dB under the signal there, which a flat EQ must
// not add.
#include <algorithm>
#include <cmath>
#include "effects/EffectDsp.h"

namespace {

class GraphicEq10Processor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "eq.graphic10";

  static constexpr int kNumBands = 10;
  enum Param {
    kBand31 = 0,
    kBand63,
    kBand125,
    kBand250,
    kBand500,
    kBand1k,
    kBand2k,
    kBand4k,
    kBand8k,
    kBand16k,
    kLevelDb
  };

  static constexpr double kCentreHz[kNumBands] = {31.5,   63.0,   125.0,  250.0,  500.0,
                                                  1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

  // Octave spacing with a little overlap: a band is about an octave wide
  // (Q 1.41 would touch its neighbours at -3 dB; a touch higher keeps the
  // sliders from piling up when several are raised together).
  static constexpr double kBandQ = 1.6;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      const char* ids[kNumBands] = {"band31",  "band63",  "band125", "band250", "band500",
                                    "band1k",  "band2k",  "band4k",  "band8k",  "band16k"};
      const char* names[kNumBands] = {"31.5 Hz", "63 Hz", "125 Hz", "250 Hz", "500 Hz",
                                      "1 kHz",   "2 kHz", "4 kHz",  "8 kHz",  "16 kHz"};
      for (int b = 0; b < kNumBands; ++b)
        t.push_back(BlockParamSpec::floatParam(ids[b], names[b], "dB", -12.0f, 12.0f, 0.0f)
                        .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -12.0f, 12.0f, 0.0f));
      return t;
    }();
    return table;
  }

  GraphicEq10Processor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // A boosted band is a resonance: its poles sit at quality Q * boost, and
  // the ring takes 6.9 time constants (q / (pi f)) to fall 60 dB. A cut has
  // broader poles and rings less, so it is taken at the flat Q. The slowest
  // band that is not flat decides.
  double tailSeconds() const noexcept override {
    double longest = 0.0;
    for (int b = 0; b < kNumBands; ++b) {
      const double gain = static_cast<double>(param(kBand31 + b));
      if (std::abs(gain) < kFlatDb)
        continue;
      const double poleQ = kBandQ * std::pow(10.0, std::max(gain, 0.0) / 40.0);
      longest = std::max(longest, 6.9078 * poleQ / (3.14159265358979 * kCentreHz[b]));
    }
    return longest > 0.0 ? longest + kSmoothSeconds : 0.0;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    for (auto& band : bandDb)
      band.prepare(sampleRate, kSmoothSeconds);
    level.prepare(sampleRate, kSmoothSeconds);
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& channel : sections)
      for (auto& section : channel)
        section.reset();
    for (auto& band : bandDb)
      band.snapOnNextTarget();
    level.snapOnNextTarget();
    // The first chunk rebuilds every section from the current gains.
    gridLeft = 0;
    for (auto& used : usedDb)
      used = kUnset;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);

    for (int b = 0; b < kNumBands; ++b)
      bandDb[b].setTarget(param(kBand31 + b));
    level.setTarget(fx::dbToGain(param(kLevelDb)));

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    float levels[kControlGrid];
    int done = 0;
    while (done < numSamples) {
      if (gridLeft == 0) {
        refreshSections();
        gridLeft = kControlGrid;
      }
      const int chunk = juce::jmin(numSamples - done, gridLeft);

      for (int i = 0; i < chunk; ++i)
        levels[i] = level.next();
      for (int ch = 0; ch < numChannels; ++ch) {
        float* x = channels[ch] + done;
        for (int i = 0; i < chunk; ++i) {
          float v = x[i];
          for (int b = 0; b < kNumBands; ++b)
            v = sections[ch][b].process(v);
          x[i] = v * levels[i];
        }
      }
      // The sliders glide on at the same pace whatever the chunking.
      for (auto& band : bandDb)
        band.skip(chunk);

      gridLeft -= chunk;
      done += chunk;
    }
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr int kControlGrid = 16;
  static constexpr double kSmoothSeconds = 0.02;
  // Under this a band is flat as far as anyone can hear; it adds no tail.
  static constexpr double kFlatDb = 0.05;
  static constexpr float kUnset = 1.0e9f;

  // One peaking section (the Audio EQ Cookbook form) in double precision,
  // transposed direct form II. The design is the one fx::BiquadCoeffs::peak
  // does, kept in double rather than rounded to float.
  struct Section {
    void design(double hz, double q, double gainDb, double sampleRate) noexcept {
      const double a = std::pow(10.0, gainDb / 40.0);
      const double w = 2.0 * 3.14159265358979323846 * hz / sampleRate;
      const double alpha = std::sin(w) / (2.0 * q);
      const double cosW = std::cos(w);
      const double a0 = 1.0 + alpha / a;
      b0 = (1.0 + alpha * a) / a0;
      b1 = (-2.0 * cosW) / a0;
      b2 = (1.0 - alpha * a) / a0;
      a1 = (-2.0 * cosW) / a0;
      a2 = (1.0 - alpha / a) / a0;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
    float process(float x) noexcept {
      const double in = static_cast<double>(x);
      const double out = b0 * in + z1;
      z1 = b1 * in - a1 * out + z2;
      z2 = b2 * in - a2 * out;
      return static_cast<float>(out);
    }

    double b0{1.0}, b1{0.0}, b2{0.0}, a1{0.0}, a2{0.0};
    double z1{0.0}, z2{0.0};
  };

  // Rebuild the sections whose slider moved since they were last built. Both
  // channels share the coefficients and are always kept current, so a
  // channel that appears later (mono to stereo) is already right.
  void refreshSections() noexcept {
    for (int b = 0; b < kNumBands; ++b) {
      const float db = bandDb[b].current();
      if (db == usedDb[b])
        continue;
      usedDb[b] = db;
      for (int ch = 0; ch < kMaxChannels; ++ch)
        sections[ch][b].design(kCentreHz[b], kBandQ, db, sampleRate);
    }
  }

  Section sections[kMaxChannels][kNumBands];
  fx::SmoothedParam bandDb[kNumBands];
  fx::SmoothedParam level;
  float usedDb[kNumBands] = {};
  double sampleRate{48000.0};
  int gridLeft{0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(GraphicEq10) {
  addEffectKind<GraphicEq10Processor>(registry, "Graphic EQ", BlockCategory::Eq);
}
