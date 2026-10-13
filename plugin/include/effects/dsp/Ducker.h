#pragma once
// Ducking, shared by the delays and the reverbs: the wet output is lowered
// while the player is playing and lets go when the playing stops, so the
// echoes and the tail stay out of the way of the notes. The two parameters are
// the same in every kind (ids `duckDb` and `duckReleaseMs`), and so is the
// code: one Ducker per channel (or one shared by a linked pair), fed the dry
// input, whose gain multiplies the wet output only. The loop of the effect is
// untouched, so the repeats or the tail keep circulating at their full level
// underneath and come back when the ducking lets go.
//
// duckDb = 0 is off: next() returns exactly 1.0f without touching any state,
// and x * 1.0f is x, so an effect with ducking off is bit-identical to one
// without the feature.
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

#include "BlockParams.h"
#include "effects/dsp/EnvelopeFollower.h"

namespace fx {

/** The two ducking parameters, in table order. Put them before spilloverParam().
    `defaultDepthDb` is where the depth starts: 0 (off) for everything that had
    no ducking before. */
inline void appendDuckParams(BlockParamTable& table, float defaultDepthDb = 0.0f) {
  table.push_back(BlockParamSpec::floatParam("duckDb", "Duck", "dB", 0.0f, 24.0f, defaultDepthDb));
  table.push_back(
      BlockParamSpec::floatParam("duckReleaseMs", "Duck Release", "ms", 50.0f, 1000.0f, 250.0f)
          .withSkew(0.5f));
}

class Ducker {
public:
  void prepare(double newSampleRate) noexcept {
    sampleRate = newSampleRate;
    // The detector is fast both ways (1 ms up, 20 ms down): it only has to
    // tell "playing" from "not playing"; the musical release is the gain's.
    detector.prepare(sampleRate);
    detector.setTimes(1.0f, 20.0f);
    attackAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (0.003 * sampleRate)));
    reset();
  }

  void reset() noexcept {
    detector.reset();
    gain = 1.0f;
    running = false;
  }

  /** Once per block. `depthDb` 0 turns it off (the gain then glides back to
      1 if it was down); `releaseMs` is the time the gain takes to come back
      (it is within 2% of unity by then). */
  void setTarget(float depthDb, float releaseMs) noexcept {
    floorGain = depthDb > 0.0f ? std::pow(10.0f, -depthDb * 0.05f) : 1.0f;
    // Four time constants of the one-pole make "back to unity in releaseMs".
    releaseAlpha = static_cast<float>(
        1.0 - std::exp(-4.0 / (0.001 * static_cast<double>(std::max(releaseMs, 1.0f)) * sampleRate)));
    running = depthDb > 0.0f || gain < 1.0f;
  }

  /** False while next() is exactly 1.0f and costs nothing: the depth is 0 and
      the gain has come back to unity. */
  bool active() const noexcept { return running; }

  /** The gain for this sample, from the dry input (`a`, `b`: the two
      channels, or the same value twice). Exactly 1.0f while off. */
  float next(float a, float b) noexcept {
    if (!running)
      return 1.0f;
    const float level = detector.process(std::max(std::abs(a), std::abs(b)));
    // Full depth from -26 dBFS up, none below -40 dBFS, linear in between.
    const float amount = juce::jlimit(0.0f, 1.0f, (level - kQuiet) * kInverseSpan);
    const float target = 1.0f - amount * (1.0f - floorGain);
    gain += (target < gain ? attackAlpha : releaseAlpha) * (target - gain);
    if (floorGain >= 1.0f && gain > 0.99999f) {
      gain = 1.0f;
      running = false;
    }
    return gain;
  }

private:
  static constexpr float kQuiet = 0.01f;  // -40 dBFS
  static constexpr float kLoud = 0.05f;   // -26 dBFS
  static constexpr float kInverseSpan = 1.0f / (kLoud - kQuiet);

  fx::EnvelopeFollower detector;
  double sampleRate{48000.0};
  float attackAlpha{0.1f};
  float releaseAlpha{0.001f};
  float floorGain{1.0f};
  float gain{1.0f};
  bool running{false};
};

}  // namespace fx
