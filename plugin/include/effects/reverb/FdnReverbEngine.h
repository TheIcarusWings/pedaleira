#pragma once
// What the room, hall and shimmer reverbs share around an FdnTank: the
// pre-delay, the input diffusion, optional early reflections, the wet low
// and high cut, the level law and the channel layouts. Each kind is then a
// Design (its sizes), a parameter table and the mapping from one to the
// other. Private to the reverb family.
//
// Signal path of one core:
//
//   in -> pre-delay -+-> early reflection taps -------------------+
//                    +-> allpass diffusion -> FdnTank -> level ----+-> low/high cut -> out
//
// Layouts (docs/effects.md, Channels):
//   mono          one core; its two tank outputs summed
//   true stereo   one core fed the sum of the two inputs; the tank's two
//                 outputs (which share no delay line) go left and right, and
//                 the early reflections use a different tap pattern per side
//   dual mono     two cores, one per channel, each exactly the mono reverb;
//                 nothing crosses (also what runs in front of an amp model)
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>

#include "effects/dsp/DelayLine.h"
#include "effects/dsp/Smoothing.h"
#include "effects/reverb/FdnTank.h"
#include "effects/reverb/ReverbParts.h"

namespace fx::reverb {

class FdnReverbEngine {
public:
  static constexpr int kMaxEarlyTaps = 8;
  static constexpr int kMaxChannels = 2;

  struct Design {
    FdnTank::Design tank;
    int diffuserStages{0};
    std::array<double, AllpassChain::kMaxStages> diffuserSeconds{};
    std::array<float, AllpassChain::kMaxStages> diffuserCoeffs{};
    // Early reflections: tap times at scale 1 and gains, per side. The gains
    // are rescaled so each side's taps carry `earlyEnergy` in total.
    int earlyTaps{0};
    std::array<double, kMaxEarlyTaps> earlyLeftSeconds{};
    std::array<double, kMaxEarlyTaps> earlyRightSeconds{};
    std::array<float, kMaxEarlyTaps> earlyLeftGains{};
    std::array<float, kMaxEarlyTaps> earlyRightGains{};
    double earlyEnergy{0.0};
    double maxPreDelaySeconds{0.2};
  };

  /** The parameter values, in engineering units, read at each control tick. */
  struct Controls {
    double rt60{1.0};
    double highRatio{1.0};
    double scale{1.0};
    float preDelayMs{0.0f};
    float modDepthMs{0.0f};
    float lowCutHz{20.0f};
    float highCutHz{20000.0f};
    // 0 to 1: how much of the tail's energy the shifters lift per trip, as a
    // share of what all of them fully on would.
    float shimmerAmount{0.0f};
    double shimmerInterval{2.0};
  };

  // The read position of the pre-delay glides to a new time (a short pitch
  // bend of what enters the tank, as on a tape pre-delay) instead of jumping.
  static constexpr double kPreDelayGlideSeconds = 0.05;

  void prepare(const Design& newDesign, double newSampleRate) {
    design = newDesign;
    sampleRate = newSampleRate;
    for (int side = 0; side < 2; ++side) {
      auto& gains = side == 0 ? design.earlyLeftGains : design.earlyRightGains;
      double energy = 0.0;
      for (int k = 0; k < design.earlyTaps; ++k)
        energy += static_cast<double>(gains[static_cast<size_t>(k)]) * gains[static_cast<size_t>(k)];
      const float scale = energy > 0.0 ? static_cast<float>(std::sqrt(design.earlyEnergy / energy)) : 0.0f;
      for (int k = 0; k < design.earlyTaps; ++k)
        gains[static_cast<size_t>(k)] *= scale;
    }
    rt60.prepare(sampleRate, 0.06);
    highRatio.prepare(sampleRate, 0.06);
    modDepth.prepare(sampleRate, 0.1);
    shimmerAmount.prepare(sampleRate, 0.06);
    lowCut.prepare(sampleRate, 0.03);
    highCut.prepare(sampleRate, 0.03);
    for (auto& core : cores)
      core.prepare(design, sampleRate);
    reset();
  }

  void reset() noexcept {
    for (auto& core : cores)
      core.reset();
    rt60.snapOnNextTarget();
    highRatio.snapOnNextTarget();
    modDepth.snapOnNextTarget();
    shimmerAmount.snapOnNextTarget();
    lowCut.snapOnNextTarget();
    highCut.snapOnNextTarget();
    untilControl = 0;
    lastCores = 0;
  }

  /** In place. `readControls()` returns the current Controls; it is called
      once per control interval, on a grid counted from the last reset. */
  template <typename ControlSource>
  void process(juce::AudioBuffer<float>& buffer, bool stereoImage, ControlSource&& readControls) noexcept {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    if (numChannels <= 0)
      return;
    const int activeCores = stereoImage ? 1 : numChannels;
    // The second core only runs in dual mono; coming back to it after
    // another layout it must not replay what it held.
    if (activeCores > lastCores && lastCores != 0)
      cores[1].reset();
    lastCores = activeCores;

    float* left = buffer.getWritePointer(0);
    float* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
    int done = 0;
    while (done < numSamples) {
      if (untilControl == 0) {
        updateControls(readControls(), activeCores);
        untilControl = kControlInterval;
      }
      const int n = juce::jmin(untilControl, numSamples - done);
      if (stereoImage) {
        for (int i = done; i < done + n; ++i)
          cores[0].processStereo(0.5f * (left[i] + right[i]), left[i], right[i]);
      } else {
        for (int i = done; i < done + n; ++i)
          left[i] = cores[0].processMono(left[i]);
        if (right != nullptr)
          for (int i = done; i < done + n; ++i)
            right[i] = cores[1].processMono(right[i]);
      }
      done += n;
      untilControl -= n;
    }
  }

private:
  struct Smoothed {
    double rt60, highRatio, scale;
    float preDelaySamples, modDepthSamples, lowCutHz, highCutHz;
    std::array<float, FdnTank::kMaxShifters> shimmerMix;
    double shimmerInterval;
    bool filtersMoved;
  };

  class Core {
  public:
    void prepare(const Design& d, double fs) {
      design = &d;
      sampleRate = fs;
      preDelay.prepare(1, static_cast<int>(std::ceil(d.maxPreDelaySeconds * fs)) + 4);
      preDelaySamples.prepare(fs, kPreDelayGlideSeconds);
      diffuser.prepare(d.diffuserSeconds.data(), d.diffuserCoeffs.data(), d.diffuserStages, fs);
      double longestTap = 0.0;
      for (int k = 0; k < d.earlyTaps; ++k)
        longestTap = std::max({longestTap, d.earlyLeftSeconds[static_cast<size_t>(k)],
                               d.earlyRightSeconds[static_cast<size_t>(k)]});
      early.prepare(1, static_cast<int>(std::ceil(longestTap * d.tank.maxScale * fs)) + 4);
      tank.prepare(d.tank, fs);
      reset();
    }

    void reset() noexcept {
      preDelay.clear();
      preDelaySamples.snapOnNextTarget();
      diffuser.clear();
      early.clear();
      tank.reset();
      for (auto& filter : wet)
        filter.reset();
      snapPending = true;
      tapsPending = false;
    }

    void control(const Smoothed& c) noexcept {
      preDelaySamples.setTarget(c.preDelaySamples);

      FdnTank::Control tc;
      tc.rt60 = c.rt60;
      tc.highRatio = c.highRatio;
      tc.scale = c.scale;
      tc.modDepthSamples = c.modDepthSamples;
      tc.shimmerMix = c.shimmerMix;
      tc.shimmerRatio = {2.0, c.shimmerInterval};
      const bool wasFading = tank.isFading();
      tank.control(tc);

      // The early taps follow the room size on the tank's own crossfade.
      if (snapPending) {
        computeTaps(c.scale, tapNow);
        tapNext = tapNow;
      } else {
        if (tapsPending && !wasFading) {
          tapNow = tapNext;
          tapsPending = false;
        }
        if (tank.isFading() && !wasFading) {
          computeTaps(c.scale, tapNext);
          tapsPending = true;
        }
      }

      float even = 0.0f, odd = 0.0f;
      tank.outputGainsFor(wetEnergyTarget(c.rt60), even, odd);
      if (snapPending) {
        evenGain.snap(even);
        oddGain.snap(odd);
      } else {
        evenGain.aim(even);
        oddGain.aim(odd);
      }
      if (snapPending || c.filtersMoved)
        for (auto& filter : wet)
          filter.set(c.lowCutHz, c.highCutHz, sampleRate);
      snapPending = false;
    }

    float processMono(float x) noexcept {
      float even = 0.0f, odd = 0.0f;
      front(x, even, odd);
      // Both outputs: they share no line, so their sum is the denser mono
      // reverb at the level of either side.
      const float late = (evenGain.step() * even + oddGain.step() * odd) * 0.70710678f;
      return wet[0].process(late + earlyTapsOf(0));
    }

    void processStereo(float x, float& left, float& right) noexcept {
      float even = 0.0f, odd = 0.0f;
      front(x, even, odd);
      left = wet[0].process(evenGain.step() * even + earlyTapsOf(0));
      right = wet[1].process(oddGain.step() * odd + earlyTapsOf(1));
    }

  private:
    // Pre-delay, early line, diffusion and tank.
    void front(float x, float& even, float& odd) noexcept {
      preDelay.write(0, x);
      // Linear, not Hermite: the times are whole samples at rest (exact
      // either way) and the read may sit at zero delay.
      const float delayed = preDelay.readLinear(0, 1.0f + preDelaySamples.next());
      if (design->earlyTaps > 0)
        early.write(0, delayed);
      tank.process(diffuser.process(delayed), even, odd);
    }

    float earlyTapsOf(int side) const noexcept {
      const int count = design->earlyTaps;
      if (count == 0)
        return 0.0f;
      const auto& gains = side == 0 ? design->earlyLeftGains : design->earlyRightGains;
      const auto& now = tapNow[static_cast<size_t>(side)];
      float sum = 0.0f;
      if (!tapsPending || !tank.isFading()) {
        // After the last sample of a fade and before the next control tick
        // there is nothing to render: a fade ends on a tick.
        const auto& taps = tapsPending ? tapNext[static_cast<size_t>(side)] : now;
        for (int k = 0; k < count; ++k)
          sum += gains[static_cast<size_t>(k)] * early.read(0, 1 + taps[static_cast<size_t>(k)]);
        return sum;
      }
      const auto& next = tapNext[static_cast<size_t>(side)];
      float old = 0.0f;
      for (int k = 0; k < count; ++k) {
        old += gains[static_cast<size_t>(k)] * early.read(0, 1 + now[static_cast<size_t>(k)]);
        sum += gains[static_cast<size_t>(k)] * early.read(0, 1 + next[static_cast<size_t>(k)]);
      }
      return old + tank.fadeValue() * (sum - old);
    }

    using TapSet = std::array<std::array<int, kMaxEarlyTaps>, 2>;
    void computeTaps(double scale, TapSet& out) const noexcept {
      for (int k = 0; k < design->earlyTaps; ++k) {
        const size_t n = static_cast<size_t>(k);
        out[0][n] = static_cast<int>(std::lround(design->earlyLeftSeconds[n] * scale * sampleRate));
        out[1][n] = static_cast<int>(std::lround(design->earlyRightSeconds[n] * scale * sampleRate));
      }
    }

    const Design* design{nullptr};
    double sampleRate{48000.0};
    fx::DelayLine preDelay;
    fx::SmoothedParam preDelaySamples;
    AllpassChain diffuser;
    fx::DelayLine early;
    FdnTank tank;
    std::array<WetFilter, 2> wet{};
    Ramped evenGain, oddGain;
    TapSet tapNow{};
    TapSet tapNext{};
    bool snapPending{true};
    bool tapsPending{false};
  };

  void updateControls(const Controls& raw, int activeCores) noexcept {
    Smoothed c;
    // Decay, damping, modulation depth and shimmer are slewed here (60 to
    // 100 ms) and then ramped per sample inside the tank: a moving knob
    // changes the loop gains smoothly, never in steps.
    c.rt60 = rt60.advance(static_cast<float>(raw.rt60));
    c.highRatio = highRatio.advance(static_cast<float>(raw.highRatio));
    // Not smoothed: a size change is a crossfade inside the tank.
    c.scale = raw.scale;
    // Whole samples, so the pre-delay read is exact once the glide is over.
    c.preDelaySamples = std::round(
        juce::jlimit(0.0f, static_cast<float>(design.maxPreDelaySeconds * 1000.0), raw.preDelayMs) *
        0.001f * static_cast<float>(sampleRate));
    c.modDepthSamples = modDepth.advance(raw.modDepthMs) * 0.001f * static_cast<float>(sampleRate);
    // The shifters come in one after another, each all the way before the
    // next starts (see FdnTank::readShifted for why not all together). The
    // mix is an amplitude share and what is heard is its energy, hence the
    // square root: the amount is linear in how fast the tail climbs.
    const int shifters = std::max(1, design.tank.numShifters);
    const float amount = shimmerAmount.advance(juce::jlimit(0.0f, 1.0f, raw.shimmerAmount));
    for (int k = 0; k < FdnTank::kMaxShifters; ++k)
      c.shimmerMix[static_cast<size_t>(k)] = std::sqrt(
          juce::jlimit(0.0f, 1.0f, amount * static_cast<float>(shifters) - static_cast<float>(k)));
    c.shimmerInterval = raw.shimmerInterval;
    const float lowBefore = lowCut.current(), highBefore = highCut.current();
    c.lowCutHz = lowCut.advance(raw.lowCutHz);
    c.highCutHz = highCut.advance(raw.highCutHz);
    c.filtersMoved = c.lowCutHz != lowBefore || c.highCutHz != highBefore;
    for (int i = 0; i < activeCores; ++i)
      cores[static_cast<size_t>(i)].control(c);
  }

  Design design;
  double sampleRate{48000.0};
  std::array<Core, kMaxChannels> cores;
  ControlSmoother rt60, highRatio, modDepth, shimmerAmount, lowCut, highCut;
  int untilControl{0};
  int lastCores{0};
};

}  // namespace fx::reverb
