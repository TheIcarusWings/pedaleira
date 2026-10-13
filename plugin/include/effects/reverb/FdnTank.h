#pragma once
// A sixteen-line feedback delay network (Jot and Chaigne 1991; Schlecht and
// Habets 2017 for the choice of matrix): the late field of the reverbs of
// the family built on it (FdnReverbEngine.h). Private to the reverb family.
//
//   lines     sixteen delays of pairwise different prime lengths, so no two
//             share a period
//   mixing    a 16 x 16 Hadamard matrix scaled to be orthogonal: energy
//             preserving, every line feeds every line equally, and none
//             feeds itself more than the others (a Householder matrix of
//             this size sends 7/8 of a line's amplitude straight back into
//             it, which rings at that line's period)
//   decay     each line of n samples is scaled by gamma^n and followed by a
//             DampingShelf (see ReverbParts.h): the RT60 is set analytically,
//             below the shelf corner by `rt60` and above it by
//             `rt60 * highRatio`
//   size      a change of size crossfades every line's read tap from its old
//             length to its new one (see control())
//   chorus    four lines read at a slowly moving position
//   shimmer   up to four lines can read through a pitch shifter (see below)
//
// The loop is passive by construction: an orthogonal matrix, then per line a
// read whose gain is at most 1 at every frequency (a plain tap, a convex mix
// of taps, or readLagrange), a gain below 1 and a shelf whose gain never
// exceeds 1. So it is stable at every setting, including full shimmer at
// the longest decay. (The chorus stretches and squeezes what four lines hold
// by a few parts in ten thousand as it moves; over a cycle that cancels.)
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>

#include "effects/dsp/DelayLine.h"
#include "effects/dsp/Filters.h"
#include "effects/reverb/ReverbParts.h"

namespace fx::reverb {

class FdnTank {
public:
  static constexpr int kLines = 16;
  static constexpr int kModLines = 4;
  static constexpr int kMaxShifters = 4;

  struct Design {
    // Line delays in seconds at scale 1, ascending.
    std::array<double, kLines> lineSeconds{};
    // The largest scale control() will be given.
    double maxScale{1.0};
    // Largest excursion of the modulated lines, each way; 0 for none.
    double maxModSeconds{0.0};
    std::array<double, 2> modRatesHz{{0.61, 0.93}};
    // How many lines can read through a pitch shifter.
    int numShifters{0};
    double shifterGrainSeconds{0.06};
    double shifterLowpassHz{6500.0};
    // Corner of the damping shelves.
    double dampingCornerHz{4000.0};
    // A second shelf per line, below a corner given at run time, that sets
    // the RT60 of the bass as a multiple of the middle one (Control::
    // bassRatio). Off, the lines have only the damping shelf.
    bool bassBand{false};
    // Length of the crossfade a size change takes.
    double fadeSeconds{0.08};
  };

  struct Control {
    double rt60{1.0};
    // High-frequency RT60 over low-frequency RT60, 0 to 1.
    double highRatio{1.0};
    double scale{1.0};
    float modDepthSamples{0.0f};
    // Speed of the two oscillators of the chorus; 0 keeps the design's rates.
    double modRateHz{0.0};
    // With Design::bassBand: RT60 below bassCornerHz over the middle RT60
    // (above 1 the bass rings longer), and the corner of that shelf.
    double bassRatio{1.0};
    double bassCornerHz{200.0};
    // Corner of the damping shelves; 0 keeps the design's.
    double dampCornerHz{0.0};
    // The loops lossless: every band holds its level (a freeze). The caller
    // keeps the output gains and shuts the input.
    bool hold{false};
    // 0 to 1 per shifter: how much of its line's read is the shifted one.
    std::array<float, kMaxShifters> shimmerMix{};
    // Pitch ratio of the even and of the odd shifters.
    std::array<double, 2> shimmerRatio{{2.0, 2.0}};
  };

  void prepare(const Design& newDesign, double newSampleRate) {
    design = newDesign;
    sampleRate = newSampleRate;
    numShifters = juce::jlimit(0, kMaxShifters, design.numShifters);
    grainSamples = std::max(64, static_cast<int>(std::lround(design.shifterGrainSeconds * sampleRate)));
    maxModSamples = static_cast<int>(std::ceil(design.maxModSeconds * sampleRate));

    const int longest = static_cast<int>(
        std::ceil(design.lineSeconds[kLines - 1] * design.maxScale * sampleRate));
    // Room for the prime search to move every line up a little.
    maxLength = longest + 16 * kLines;
    primes.prepare(maxLength);
    // A shifter reads up to one grain past the line's own length (ratio 2).
    lines.prepare(kLines, maxLength + std::max(maxModSamples, numShifters > 0 ? grainSamples : 0) + 4);

    // Whole control intervals, so a fade ends on a control tick.
    fadeSamples = kControlInterval *
                  std::max(1, static_cast<int>(std::lround(design.fadeSeconds * sampleRate /
                                                           kControlInterval)));
    corner = designCorner = shelfCorner(design.dampingCornerHz, sampleRate);
    for (int j = 0; j < 2; ++j)
      lfo[static_cast<size_t>(j)].setRate(design.modRatesHz[static_cast<size_t>(j)], sampleRate);
    appliedModRate = 0.0;
    const double grainAngle = juce::MathConstants<double>::twoPi / grainSamples;
    grainStepCos = static_cast<float>(std::cos(grainAngle));
    grainStepSin = static_cast<float>(std::sin(grainAngle));
    for (auto& filter : shifterLowpass)
      filter.setCoeffs(fx::BiquadCoeffs::lowpass(design.shifterLowpassHz, 0.7071, sampleRate));
    reset();
  }

  void reset() noexcept {
    lines.clear();
    shelfState.fill(0.0f);
    bassState.fill(0.0f);
    lfo[0].reset(0.0);
    lfo[1].reset(0.125);
    for (int k = 0; k < kMaxShifters; ++k) {
      // Grains of different shifters start apart, so their seams do not
      // coincide.
      shifterPhase[static_cast<size_t>(k)] = 0.25f * static_cast<float>(k) + 0.1f;
      syncGrainWindow(k);
      shifterRange[static_cast<size_t>(k)] = {0.0f, 0.0f};
      shifterLowpass[static_cast<size_t>(k)].reset();
    }
    fadeRemaining = 0;
    snapPending = true;
    gainsSettled = false;
  }

  /**
   * Once per control interval, before its samples.
   *
   * A size change does not move the taps (that would bend the pitch of the
   * whole tail, and an interpolated read in every line would dull it for
   * good): each line fades from a tap at its old length to one at its new
   * length over `fadeSeconds`, and the next change waits for that fade. The
   * fade is linear, a convex combination, so the loop stays passive while it
   * runs; the price is a tail a few dB quieter while the knob is moving.
   */
  void control(const Control& c) noexcept {
    if (snapPending) {
      computeLengths(c.scale, current);
      target = current;
      appliedScale = c.scale;
      fadeRemaining = 0;
    } else if (fadeRemaining == 0 && c.scale != appliedScale) {
      computeLengths(c.scale, target);
      appliedScale = c.scale;
      if (target != current) {
        fadeRemaining = fadeSamples;
        gainsSettled = false;
      }
    }

    // The shelves' corners and the oscillators' speed move freely (nothing
    // in them is a gain); with the defaults they are the design's.
    corner = c.dampCornerHz > 0.0 ? shelfCorner(c.dampCornerHz, sampleRate) : designCorner;
    if (design.bassBand)
      bassCorner = shelfCorner(c.bassCornerHz, sampleRate);
    if (c.modRateHz != appliedModRate) {
      const double ratio = design.modRatesHz[1] / design.modRatesHz[0];
      const double first = c.modRateHz > 0.0 ? c.modRateHz : design.modRatesHz[0];
      lfo[0].setRate(first, sampleRate);
      lfo[1].setRate(c.modRateHz > 0.0 ? first * ratio : design.modRatesHz[1], sampleRate);
      appliedModRate = c.modRateHz;
    }

    const bool moved = c.rt60 != appliedRt60 || c.highRatio != appliedHighRatio ||
                       c.bassRatio != appliedBassRatio || c.hold != appliedHold;
    if (snapPending || moved || fadeRemaining > 0 || !gainsSettled) {
      // Where the fade will be at the end of this interval: the gains follow
      // the line lengths across it.
      const double fade =
          fadeRemaining > 0
              ? 1.0 - static_cast<double>(std::max(0, fadeRemaining - kControlInterval)) / fadeSamples
              : 0.0;
      const double highRt60 = c.rt60 * juce::jlimit(0.01, 1.0, c.highRatio);
      double energy[2] = {0.0, 0.0};
      for (int i = 0; i < kLines; ++i) {
        const size_t n = static_cast<size_t>(i);
        const double length = current[n] + fade * (target[n] - current[n]);
        // Held, every band passes at exactly unity.
        const double lowGain = c.hold ? 1.0 : decayGain(length, c.rt60, sampleRate);
        energy[i & 1] += lowGain * lowGain;
        const float low = static_cast<float>(lowGain);
        const float high = c.hold ? 1.0f : static_cast<float>(decayGain(length, highRt60, sampleRate));
        if (snapPending) {
          gainHigh[n].snap(high);
          gainShelf[n].snap(low - high);
        } else {
          gainHigh[n].aim(high);
          gainShelf[n].aim(low - high);
        }
        if (design.bassBand) {
          // What the bass shelf adds on top of the middle gain: the bass band
          // as a whole passes at decayGain(length, rt60 * ratio), which is
          // never above 1, and the shelf's own gain never exceeds its low
          // end (see DampingShelf), so the loop stays passive.
          const double bassGain =
              c.hold ? 1.0
                     : decayGain(length, c.rt60 * juce::jlimit(0.05, 20.0, c.bassRatio), sampleRate);
          const float extra = static_cast<float>(bassGain / lowGain - 1.0);
          if (snapPending)
            gainBass[n].snap(extra);
          else
            gainBass[n].aim(extra);
        }
      }
      if (!c.hold)
        passEnergy = {energy[0], energy[1]};
      // Settled once a tick has re-aimed at unchanged targets: the ramps are
      // then exactly on them and need no more work.
      gainsSettled = !snapPending && !moved && fadeRemaining == 0 && rampsIdle();
      appliedRt60 = c.rt60;
      appliedHighRatio = c.highRatio;
      appliedBassRatio = c.bassRatio;
      appliedHold = c.hold;
    }

    const float depth = juce::jlimit(0.0f, static_cast<float>(maxModSamples), c.modDepthSamples);
    if (snapPending)
      modDepth.snap(depth);
    else
      modDepth.aim(depth);
    for (int k = 0; k < numShifters; ++k) {
      const float mix = juce::jlimit(0.0f, 1.0f, c.shimmerMix[static_cast<size_t>(k)]);
      if (snapPending)
        shimmerMix[static_cast<size_t>(k)].snap(mix);
      else
        shimmerMix[static_cast<size_t>(k)].aim(mix);
      const double ratio = juce::jlimit(1.0, 2.0, c.shimmerRatio[static_cast<size_t>(k & 1)]);
      shifterRangeTarget[static_cast<size_t>(k)] = static_cast<float>(grainSamples * (ratio - 1.0));
      if (snapPending)
        shifterRange[static_cast<size_t>(k)] = {shifterRangeTarget[static_cast<size_t>(k)],
                                                shifterRangeTarget[static_cast<size_t>(k)]};
    }
    lfo[0].renormalise();
    lfo[1].renormalise();
    snapPending = false;
  }

  /** One sample in; the signed sums of the even and of the odd lines out
      (two outputs that share no line, hence uncorrelated). */
  void process(float in, float& even, float& odd) noexcept {
    float s[kLines];
    const bool fading = fadeRemaining > 0;
    const float fade =
        fading ? 1.0f - static_cast<float>(fadeRemaining) / static_cast<float>(fadeSamples) : 0.0f;
    if (!fading) {
      for (int i = 0; i < kLines; ++i)
        s[i] = lines.read(i, current[static_cast<size_t>(i)]);
    } else {
      for (int i = 0; i < kLines; ++i) {
        const float a = lines.read(i, current[static_cast<size_t>(i)]);
        const float b = lines.read(i, target[static_cast<size_t>(i)]);
        s[i] = a + fade * (b - a);
      }
    }

    // Chorus: four lines read at a slowly moving position, which keeps the
    // modes of a long tail from standing still and ringing. readLagrange,
    // because the read sits inside the loop (see ReverbParts.h).
    if (maxModSamples > 0) {
      lfo[0].advance();
      lfo[1].advance();
      const float depth = modDepth.step();
      if (depth > 0.0f) {
        const float osc[kModLines] = {lfo[0].sine, lfo[0].cosine, lfo[1].sine, lfo[1].cosine};
        for (int j = 0; j < kModLines; ++j) {
          const int i = kModLine[j];
          const float offset = depth * osc[j];
          float a = readLagrange(lines, i, static_cast<float>(current[static_cast<size_t>(i)]) + offset);
          if (fading)
            a += fade * (readLagrange(lines, i, static_cast<float>(target[static_cast<size_t>(i)]) + offset) - a);
          s[i] = a;
        }
      }
    }

    // Shimmer: see readShifted().
    for (int k = 0; k < numShifters; ++k) {
      const float mix = shimmerMix[static_cast<size_t>(k)].step();
      if (mix > 0.0f) {
        const int i = kShifterLine[k];
        s[i] += mix * (readShifted(k, i) - s[i]);
      }
    }

    // The bass shelf first (unity above its corner, `extra` more below it),
    // then the damping shelf, in series: each one's gain is bounded by its
    // own two ends, so their product is too.
    if (design.bassBand)
      for (int i = 0; i < kLines; ++i) {
        const size_t n = static_cast<size_t>(i);
        bassState[n] += bassCorner * (s[i] - bassState[n]);
        s[i] += gainBass[n].value * bassState[n];
      }
    for (int i = 0; i < kLines; ++i) {
      const size_t n = static_cast<size_t>(i);
      shelfState[n] += corner * (s[i] - shelfState[n]);
      s[i] = gainHigh[n].value * s[i] + gainShelf[n].value * shelfState[n];
    }
    if (!gainsSettled)
      for (int i = 0; i < kLines; ++i) {
        gainHigh[static_cast<size_t>(i)].step();
        gainShelf[static_cast<size_t>(i)].step();
        if (design.bassBand)
          gainBass[static_cast<size_t>(i)].step();
      }

    float sumEven = 0.0f, sumOdd = 0.0f;
    for (int i = 0; i < kLines; i += 2) {
      sumEven += kOutputSign[i] * s[i];
      sumOdd += kOutputSign[i + 1] * s[i + 1];
    }
    even = sumEven;
    odd = sumOdd;

    // Fast Walsh-Hadamard transform; 1/4 makes the 16 x 16 matrix orthogonal.
    for (int half = 1; half < kLines; half <<= 1)
      for (int i = 0; i < kLines; i += half << 1)
        for (int j = i; j < i + half; ++j) {
          const float a = s[j];
          const float b = s[j + half];
          s[j] = a + b;
          s[j + half] = a - b;
        }
    const float drive = 0.25f * in;
    for (int i = 0; i < kLines; ++i)
      lines.write(i, 0.25f * s[i] + kInputSign[i] * drive);

    if (fading && --fadeRemaining == 0)
      current = target;
  }

  /**
   * Gains that bring the even and the odd output to `energy` (the energy of
   * the impulse response) each.
   *
   * Energy bookkeeping of the network: the input puts 1/16 into each line;
   * what leaves a line has its gain squared left, and the orthogonal matrix
   * hands it on in sixteen equal parts. With m the mean of the squared line
   * gains, everything that ever leaves the lines sums to m / (1 - m) per
   * unit of input, and line i's part of it is its squared gain over
   * 16 (1 - m). An output sums eight lines. Done per output because the two
   * hold different lines: without it one side would be louder at short
   * decays, where the lines' gains differ most.
   */
  void outputGainsFor(double energy, float& even, float& odd) const noexcept {
    const double mean = (passEnergy[0] + passEnergy[1]) / kLines;
    const double scale = energy * kLines * std::max(1.0e-9, 1.0 - mean);
    even = static_cast<float>(std::sqrt(scale / std::max(1.0e-12, passEnergy[0])));
    odd = static_cast<float>(std::sqrt(scale / std::max(1.0e-12, passEnergy[1])));
  }

  bool isFading() const noexcept { return fadeRemaining > 0; }
  /** 0 at the old lengths, 1 at the new ones. */
  float fadeValue() const noexcept {
    return fadeRemaining > 0
               ? 1.0f - static_cast<float>(fadeRemaining) / static_cast<float>(fadeSamples)
               : 0.0f;
  }

private:
  static constexpr int kModLine[kModLines] = {1, 4, 9, 12};
  static constexpr int kShifterLine[kMaxShifters] = {2, 7, 10, 15};
  // Fixed sign patterns. The input one spreads an impulse over the lines
  // with mixed polarity. The output one decides how much of a line's
  // recirculation comes straight back out one line length later, which is
  // what would be heard as that line's pitch: line i contributes the i-th
  // entry of the Hadamard transform of the pattern. A row of the matrix
  // would put all of it on one line; this is a bent sequence
  // ((-1)^(b0 b1 + b2 b3) over the bits of the index), whose transform has
  // the same magnitude everywhere, the smallest any pattern can reach.
  static constexpr float kInputSign[kLines] = {1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1};
  static constexpr float kOutputSign[kLines] = {1, 1, 1, -1, 1, 1, 1, -1, 1, 1, 1, -1, -1, -1, -1, 1};

  void computeLengths(double scale, std::array<int, kLines>& out) const noexcept {
    int previous = 1;
    for (int i = 0; i < kLines; ++i) {
      const int wanted = static_cast<int>(
          std::lround(design.lineSeconds[static_cast<size_t>(i)] * scale * sampleRate));
      // Ascending and distinct primes, so pairwise coprime; clear of the
      // modulation excursion at the short end.
      const int length = primes.atLeast(std::max({wanted, previous + 1, maxModSamples + 3}));
      out[static_cast<size_t>(i)] = std::min(length, maxLength);
      previous = out[static_cast<size_t>(i)];
    }
  }

  bool rampsIdle() const noexcept {
    for (int i = 0; i < kLines; ++i)
      if (gainHigh[static_cast<size_t>(i)].delta != 0.0f || gainShelf[static_cast<size_t>(i)].delta != 0.0f ||
          gainBass[static_cast<size_t>(i)].delta != 0.0f)
        return false;
    return true;
  }

  void syncGrainWindow(int k) noexcept {
    const double angle = juce::MathConstants<double>::twoPi * shifterPhase[static_cast<size_t>(k)];
    grainCos[static_cast<size_t>(k)] = static_cast<float>(std::cos(angle));
    grainSin[static_cast<size_t>(k)] = static_cast<float>(std::sin(angle));
  }

  /**
   * The pitch-shifted read of shifter `k` on line `line`: two taps whose
   * delay shrinks at (ratio - 1) samples per sample, so each plays the line
   * back `ratio` times too fast, half a grain apart and crossfaded with
   * raised-cosine windows that sum to 1. Each tap jumps back by one grain
   * while its window is at zero.
   *
   * Why this cannot run away: at every instant the output is a convex
   * combination of two samples of the line, and over time every stored
   * sample is read with a total weight of exactly 1 (the windows overlap to
   * a constant in the line's own time as well, for the ratios 2 and 3/2
   * used here). So the shifter never puts out more energy than it reads. In
   * the tank its output replaces a share `mix` of the line's plain read,
   * again a convex combination. Energy that moves up an octave is therefore
   * taken out of the note it came from: the base of the tail gets shorter as
   * the shimmer rises, and the total can only fall.
   *
   * The price of that guarantee is that a convex mix of two unrelated
   * signals (the plain read and the shifted one, or the two taps) loses
   * energy: up to half of the line's at an even mix. Callers therefore
   * bring the shifters in one after another, each all the way, rather than
   * all of them part of the way (FdnReverbEngine).
   *
   * A new ratio is taken up by each tap at its own seam, so changing the
   * interval does not click.
   */
  float readShifted(int k, int line) noexcept {
    const size_t n = static_cast<size_t>(k);
    float phase = shifterPhase[n];
    const float other = phase >= 0.5f ? phase - 0.5f : phase + 0.5f;
    const float windowA = 0.5f - 0.5f * grainCos[n];
    const float base = static_cast<float>(current[static_cast<size_t>(line)]);
    const float a = lines.readLinear(line, base + shifterRange[n][0] * (1.0f - phase));
    const float b = lines.readLinear(line, base + shifterRange[n][1] * (1.0f - other));
    const float shifted = b + windowA * (a - b);

    const float c = grainCos[n] * grainStepCos - grainSin[n] * grainStepSin;
    grainSin[n] = grainSin[n] * grainStepCos + grainCos[n] * grainStepSin;
    grainCos[n] = c;
    const float next = phase + 1.0f / static_cast<float>(grainSamples);
    if (phase < 0.5f && next >= 0.5f)
      shifterRange[n][1] = shifterRangeTarget[n];
    phase = next;
    if (phase >= 1.0f) {
      phase -= 1.0f;
      shifterRange[n][0] = shifterRangeTarget[n];
      shifterPhase[n] = phase;
      // The window is recomputed from the phase once a grain, so the
      // rotation's rounding never accumulates.
      syncGrainWindow(k);
    } else {
      shifterPhase[n] = phase;
    }
    // Above this the next pass would fold over; the shifted path is also
    // where the tail would otherwise climb to the top of the band and hiss.
    return shifterLowpass[n].process(shifted);
  }

  Design design;
  double sampleRate{48000.0};
  fx::DelayLine lines;
  PrimeTable primes;
  std::array<int, kLines> current{};
  std::array<int, kLines> target{};
  std::array<Ramped, kLines> gainHigh{};
  std::array<Ramped, kLines> gainShelf{};
  std::array<Ramped, kLines> gainBass{};
  std::array<float, kLines> shelfState{};
  std::array<float, kLines> bassState{};
  float corner{1.0f};
  float designCorner{1.0f};
  float bassCorner{1.0f};
  int maxLength{2};
  int maxModSamples{0};
  int fadeSamples{kControlInterval};
  int fadeRemaining{0};
  double appliedScale{1.0};
  double appliedRt60{0.0};
  double appliedHighRatio{0.0};
  double appliedBassRatio{1.0};
  double appliedModRate{0.0};
  bool appliedHold{false};
  // Sum of the squared low-frequency line gains, even and odd lines.
  std::array<double, 2> passEnergy{{1.0, 1.0}};
  bool snapPending{true};
  bool gainsSettled{false};

  std::array<QuadratureLfo, 2> lfo{};
  Ramped modDepth;

  int numShifters{0};
  int grainSamples{2880};
  float grainStepCos{1.0f};
  float grainStepSin{0.0f};
  std::array<Ramped, kMaxShifters> shimmerMix{};
  std::array<float, kMaxShifters> shifterPhase{};
  std::array<float, kMaxShifters> grainCos{};
  std::array<float, kMaxShifters> grainSin{};
  std::array<std::array<float, 2>, kMaxShifters> shifterRange{};
  std::array<float, kMaxShifters> shifterRangeTarget{};
  std::array<fx::Biquad, kMaxShifters> shifterLowpass{};
};

}  // namespace fx::reverb
