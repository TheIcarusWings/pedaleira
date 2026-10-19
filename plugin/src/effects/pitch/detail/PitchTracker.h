#pragma once
// Private to the pitch family: a monophonic pitch tracker for the audio
// thread, shared by the kinds that need to know the note being played
// (pitch.harmony, pitch.subsynth, pitch.correct).
//
// Method: the McLeod Pitch Method (P. McLeod and G. Wyvill, "A Smarter Way to
// Find Pitch", ICMC 2005). For a window of the signal it computes the
// normalised square difference function
//
//     n(tau) = 2 * sum_j x[j] x[j + tau] / sum_j (x[j]^2 + x[j + tau]^2)
//
// which is 1 for a perfectly periodic signal at a lag of one period, and
// reads the pitch off its "key maxima": the highest point of each positive
// lobe. The first key maximum above k times the largest one (k = 0.93, the
// paper's choice) is the period; its height is the clarity of the pitch, a
// number between 0 and 1 that is used here as the confidence. A note's period
// is thereby picked over its multiples (the octave errors of a plain
// autocorrelation) without the threshold tuning YIN needs.
//
// What is added around the method, for a guitar on the audio thread:
//  - the input is high-passed (40 Hz), low-passed (two sections at 2 kHz) and
//    decimated to about 12 kHz, so the lag search is short;
//  - the search is a two-tier one. A short window (13 ms, lags down to 150 Hz)
//    is tried first and is accepted for notes above 250 Hz, where it holds at
//    least three periods; everything else (and everything the short window is
//    unsure of) goes to the long window (29 ms, lags down to 70 Hz). High
//    notes are therefore recognised in about 15 ms, the low E in about 30;
//  - the coarse result (12 kHz, a lag of 9 to 170 samples) is refined with the
//    same function at the full rate over nine lags around it, with a window of
//    two periods, which brings the frequency to well under a cent;
//  - the work of a frame is cut into slices, one per control step (16
//    samples), so no block carries a burst: nothing here allocates or locks
//    after prepare();
//  - a quiet frame (below about -60 dBFS) is silence, not a note;
//  - the last good note is held when confidence drops (a chord, noise, the gap
//    between two notes): frequencyHz() keeps its value, and only reset() or a
//    new confident note changes it. isVoiced() and confidence() say what the
//    latest frame found;
//  - the note a block acts on (note()) is the stable one: a new note has to be
//    heard in two frames in a row to take over, and a bend of less than 65
//    cents does not change it.
//
// The figures above are the guitar range (Range::guitar(), what prepare(rate)
// uses). A kind may prepare the tracker for another range: the bass range
// reaches down to 28 Hz (a five-string's low B is 30.9 Hz) with a longer
// window (72 ms), so its low notes are known later; the voice range accepts a
// less periodic signal (a lower clarity bar), as a sung note with breath and
// jitter is. With Range::guitar() every number is the one the tracker always
// had, so the kinds that do not ask for a range are unchanged to the bit.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "effects/EffectDsp.h"

namespace pitchfx {

class PitchTracker {
public:
  static constexpr double kMinHz = 70.0;     // below a guitar's low E (82 Hz) with room
  static constexpr double kMaxHz = 1400.0;   // above the highest guitar fret
  static constexpr float kClarityThreshold = 0.82f;
  static constexpr float kSilenceRms = 0.001f;  // -60 dBFS

  /** What the tracker listens for. */
  struct Range {
    double lowestHz;        // the lowest note it can read (sets the long window)
    double highestHz;       // the highest
    double highpassHz;      // the input's high-pass, under the lowest note
    float clarity;          // the clarity a frame needs to be a note
    int lagsPerStep;        // coarse lags per control step (the frame's pace)
    bool fineWindowFollows; // the refinement window may grow to two of its periods

    /** The guitar: 70 Hz to 1.4 kHz, the tracker's original setting. */
    static constexpr Range guitar() noexcept { return {kMinHz, kMaxHz, kHighpassHz, kClarityThreshold, kLagsPerStep, false}; }
    /** Bass: down to 28 Hz (a five-string's low B), up to 1 kHz. */
    static constexpr Range bass() noexcept { return {28.0, 1000.0, 20.0, kClarityThreshold, 2 * kLagsPerStep, true}; }
    /** Voice and other less periodic monophonic sources: 60 Hz to 1.4 kHz
        with a lower clarity bar. */
    static constexpr Range voice() noexcept { return {60.0, kMaxHz, kHighpassHz, 0.75f, kLagsPerStep, false}; }
  };

  void prepare(double sampleRate) { prepare(sampleRate, Range::guitar()); }

  void prepare(double sampleRate, const Range& range) {
    rate = sampleRate;
    clarityBar = range.clarity;
    lagsPerStep = std::max(1, range.lagsPerStep);
    decimation = std::max(1, static_cast<int>(std::lround(sampleRate / kCoarseRate)));
    coarseRate = sampleRate / decimation;
    lagMin = std::max(3, static_cast<int>(std::floor(coarseRate / range.highestHz)));
    shortLagMax = static_cast<int>(std::lround(coarseRate / kShortMinHz));
    longLagMax = static_cast<int>(std::ceil(coarseRate / range.lowestHz));
    shortWindow = 2 * shortLagMax;
    longWindow = 2 * longLagMax;
    hop = std::max(1, static_cast<int>(std::lround(kHopSeconds * coarseRate)));
    maxFineWindow = kMaxFineWindow;
    if (range.fineWindowFollows)
      maxFineWindow = std::max(kMaxFineWindow, 2 * longLagMax * decimation + 8);
    coarseRingSize = juce::nextPowerOfTwo(longWindow + hop + 16);
    coarseRing.assign(static_cast<size_t>(coarseRingSize), 0.0f);
    fineRingSize = juce::nextPowerOfTwo(std::max(static_cast<int>(sampleRate * 0.17),
                                                 maxFineWindow + (longLagMax + 2) * decimation + 4 * kRefineSpan + 64));
    fineRing.assign(static_cast<size_t>(fineRingSize), 0.0f);
    frame.assign(static_cast<size_t>(longWindow), 0.0f);
    prefix.assign(static_cast<size_t>(longWindow) + 1, 0.0);
    nsdf.assign(static_cast<size_t>(longLagMax) + 3, 0.0f);
    fineNsdf.assign(static_cast<size_t>(2 * kRefineSpan) + 3, 0.0f);
    fineSegment.assign(static_cast<size_t>(maxFineWindow + longLagMax * decimation + 4 * kRefineSpan + 16), 0.0f);
    highpass.setCutoff(range.highpassHz, sampleRate);
    const fx::BiquadCoeffs low = fx::BiquadCoeffs::lowpass(kLowpassHz, 0.7071, sampleRate);
    lowpass[0].setCoeffs(low);
    lowpass[1].setCoeffs(low);
    reset();
  }

  void reset() noexcept {
    std::fill(coarseRing.begin(), coarseRing.end(), 0.0f);
    std::fill(fineRing.begin(), fineRing.end(), 0.0f);
    highpass.reset();
    lowpass[0].reset();
    lowpass[1].reset();
    phase = 0;
    coarseWritten = 0;
    fineWritten = 0;
    sinceFrame = 0;
    stage = Stage::Idle;
    voiced = false;
    haveNote = false;
    frequency = 0.0f;
    clarity = 0.0f;
    level = 0.0f;
    stableNote = -1;
    candidate = -1;
    candidateCount = 0;
    framesDone = 0;
  }

  /** One input sample, every sample. */
  void push(float x) noexcept {
    const float y = lowpass[1].process(lowpass[0].process(highpass.highpass(x)));
    fineRing[static_cast<size_t>(fineWritten) & static_cast<size_t>(fineRingSize - 1)] = y;
    ++fineWritten;
    if (++phase >= decimation) {
      phase = 0;
      coarseRing[static_cast<size_t>(coarseWritten) & static_cast<size_t>(coarseRingSize - 1)] = y;
      ++coarseWritten;
      ++sinceFrame;
    }
  }

  /** One slice of the analysis; call it once per control step. */
  void step() noexcept {
    if (stage == Stage::Idle) {
      if (coarseWritten < longWindow || sinceFrame < hop)
        return;
      beginFrame();
      if (stage == Stage::Idle)
        return;
    }
    switch (stage) {
      case Stage::Short: runCoarse(shortWindow, shortLagMax, Stage::Short); break;
      case Stage::Long: runCoarse(longWindow, longLagMax, Stage::Long); break;
      case Stage::Refine: runRefine(); break;
      case Stage::Idle: break;
    }
  }

  // ---- What the tracker knows -------------------------------------------

  /** The latest frame found a confident pitch. */
  bool isVoiced() const noexcept { return voiced; }
  /** Clarity of the latest frame's best pitch, 0 to 1; 0 for silence. */
  float confidence() const noexcept { return clarity; }
  /** A good note has been heard since the last reset (and is being held). */
  bool hasNote() const noexcept { return haveNote; }
  /** Frequency of the last confident frame, held while confidence is low. 0
      before the first. */
  float frequencyHz() const noexcept { return frequency; }
  /** The same as a continuous MIDI note number (A4 = 69), 0 before the first. */
  float midiNote() const noexcept {
    return frequency > 0.0f ? 69.0f + 12.0f * std::log2(frequency * (1.0f / 440.0f)) : 0.0f;
  }
  /** The stable note, rounded to a MIDI note number; -1 before the first. */
  int note() const noexcept { return stableNote; }
  /** RMS of the latest analysed window. */
  float rms() const noexcept { return level; }
  /** Frames analysed since the last reset (tests, latency measurement). */
  int frames() const noexcept { return framesDone; }

  /** Time between two frames. */
  double hopSeconds() const noexcept { return static_cast<double>(hop) / coarseRate; }

private:
  // Eight independent partial sums: float additions cannot be reordered by the
  // compiler, so this is what lets the loop run as SIMD lanes.
  static float dot(const float* a, const float* b, int n) noexcept {
    float acc[8] = {};
    int i = 0;
    for (; i + 8 <= n; i += 8)
      for (int k = 0; k < 8; ++k)
        acc[k] += a[i + k] * b[i + k];
    float sum = 0.0f;
    for (; i < n; ++i)
      sum += a[i] * b[i];
    for (int k = 0; k < 8; ++k)
      sum += acc[k];
    return sum;
  }

  static constexpr double kCoarseRate = 12000.0;
  static constexpr double kHopSeconds = 0.005;
  static constexpr double kShortMinHz = 150.0;
  static constexpr double kHighpassHz = 40.0;
  static constexpr double kLowpassHz = 2000.0;
  static constexpr float kKeyMaximum = 0.93f;       // McLeod's k
  static constexpr double kShortAcceptShare = 0.6;  // a short-window lag must be under this of its range
  static constexpr int kLagsPerStep = 24;
  static constexpr int kRefineLagsPerStep = 3;
  static constexpr int kRefineSpan = 4;
  static constexpr int kMaxFineWindow = 1500;

  enum class Stage { Idle, Short, Long, Refine };

  struct Peak {
    bool found{false};
    double lag{0.0};  // fractional, in samples of the stage's rate
    float clarity{0.0f};
  };

  void beginFrame() noexcept {
    // The newest longWindow coarse samples, oldest first.
    const size_t mask = static_cast<size_t>(coarseRingSize - 1);
    const int64_t first = coarseWritten - longWindow;
    double energy = 0.0;
    prefix[0] = 0.0;
    for (int i = 0; i < longWindow; ++i) {
      const float v = coarseRing[static_cast<size_t>(first + i) & mask];
      frame[static_cast<size_t>(i)] = v;
      prefix[static_cast<size_t>(i) + 1] = prefix[static_cast<size_t>(i)] + static_cast<double>(v) * v;
    }
    sinceFrame = 0;
    fineSnapshot = fineWritten;
    // Level over the short window: silence is decided on the newest 13 ms, so a
    // note that has just stopped is not held up by the long window.
    energy = prefix[static_cast<size_t>(longWindow)] - prefix[static_cast<size_t>(longWindow - shortWindow)];
    level = static_cast<float>(std::sqrt(energy / shortWindow));
    if (level < kSilenceRms) {
      finishFrame(false, 0.0, 0.0f);
      return;
    }
    nextLag = 1;
    stage = Stage::Short;
  }

  // Lags of `window` computed so far; at the end the peak is picked.
  void runCoarse(int window, int lagMax, Stage current) noexcept {
    const float* base = frame.data() + (longWindow - window);
    const double* sums = prefix.data() + (longWindow - window);
    for (int n = 0; n < lagsPerStep && nextLag <= lagMax + 1; ++n, ++nextLag) {
      const int lag = nextLag;
      const int count = window - lag;
      const float r = dot(base, base + lag, count);
      // sum of x[j]^2 over [0, count) plus over [lag, window)
      const double m = (sums[count] - sums[0]) + (sums[window] - sums[lag]);
      nsdf[static_cast<size_t>(lag)] = m > 1.0e-12 ? static_cast<float>(2.0 * static_cast<double>(r) / m) : 0.0f;
    }
    if (nextLag <= lagMax + 1)
      return;

    const Peak peak = pickPeak(lagMax);
    if (current == Stage::Short) {
      if (peak.found && peak.clarity >= clarityBar &&
          peak.lag <= kShortAcceptShare * shortLagMax) {
        startRefine(peak);
      } else {
        // Not a note the short window can vouch for: the long one decides.
        nextLag = 1;
        stage = Stage::Long;
      }
      return;
    }
    if (peak.found && peak.clarity >= clarityBar)
      startRefine(peak);
    else
      finishFrame(false, 0.0, peak.found ? peak.clarity : bestLobe(lagMax));
  }

  // The key maxima of the NSDF and the first one above k times the highest.
  Peak pickPeak(int lagMax) const noexcept {
    struct Maximum {
      int lag;
      float value;
    };
    constexpr int kMaxMaxima = 24;
    Maximum maxima[kMaxMaxima];
    int count = 0;
    int pos = 1;
    // Skip the lobe around lag 0.
    while (pos <= lagMax && nsdf[static_cast<size_t>(pos)] > 0.0f)
      ++pos;
    float highest = 0.0f;
    while (pos <= lagMax) {
      while (pos <= lagMax && nsdf[static_cast<size_t>(pos)] <= 0.0f)
        ++pos;
      if (pos > lagMax)
        break;
      float lobe = -1.0f;
      int where = pos;
      while (pos <= lagMax && nsdf[static_cast<size_t>(pos)] > 0.0f) {
        if (nsdf[static_cast<size_t>(pos)] > lobe) {
          lobe = nsdf[static_cast<size_t>(pos)];
          where = pos;
        }
        ++pos;
      }
      if (where >= lagMin && where < lagMax && count < kMaxMaxima) {
        maxima[count++] = {where, lobe};
        highest = std::max(highest, lobe);
      }
    }
    Peak result;
    const float bar = kKeyMaximum * highest;
    for (int i = 0; i < count; ++i) {
      if (maxima[i].value >= bar) {
        const int lag = maxima[i].lag;
        const float y0 = nsdf[static_cast<size_t>(lag - 1)];
        const float y1 = nsdf[static_cast<size_t>(lag)];
        const float y2 = nsdf[static_cast<size_t>(lag + 1)];
        const float denominator = y0 - 2.0f * y1 + y2;
        const float shift = std::abs(denominator) > 1.0e-9f ? 0.5f * (y0 - y2) / denominator : 0.0f;
        const float clamped = juce::jlimit(-1.0f, 1.0f, shift);
        result.found = true;
        result.lag = static_cast<double>(lag) + static_cast<double>(clamped);
        result.clarity = std::min(1.0f, y1 - 0.25f * (y0 - y2) * clamped);
        break;
      }
    }
    return result;
  }

  // The highest point of the NSDF past its first lobe, as a confidence for a
  // frame that found no pitch.
  float bestLobe(int lagMax) const noexcept {
    float best = 0.0f;
    int pos = 1;
    while (pos <= lagMax && nsdf[static_cast<size_t>(pos)] > 0.0f)
      ++pos;
    for (; pos <= lagMax; ++pos)
      best = std::max(best, nsdf[static_cast<size_t>(pos)]);
    return juce::jlimit(0.0f, 1.0f, best);
  }

  void startRefine(const Peak& coarse) noexcept {
    coarseClarity = coarse.clarity;
    coarseLag = coarse.lag * decimation;
    fineWindow = juce::jlimit(160, maxFineWindow, 2 * static_cast<int>(std::lround(coarseLag)));
    fineFirstLag = std::max(lagMin * decimation, static_cast<int>(std::lround(coarseLag)) - kRefineSpan);
    fineLagCount = 2 * kRefineSpan + 1;
    fineNext = 0;
    // The newest fineWindow + the longest lag samples, linear, oldest first:
    // fineSegment[fineBack + j] is the sample j into the newest window, and
    // lag L looks fineBack - L + j.
    const int farthest = fineFirstLag + fineLagCount;
    fineBack = farthest;
    const size_t mask = static_cast<size_t>(fineRingSize - 1);
    const int64_t first = fineSnapshot - fineWindow - farthest;
    for (int i = 0; i < fineWindow + farthest; ++i)
      fineSegment[static_cast<size_t>(i)] = fineRing[static_cast<size_t>(first + i) & mask];
    const float* newest = fineSegment.data() + fineBack;
    fineEnergy = static_cast<double>(dot(newest, newest, fineWindow));
    // The energy of the older window of the first lag; later lags slide it.
    const float* older = fineSegment.data() + fineBack - fineFirstLag;
    fineOlderEnergy = static_cast<double>(dot(older, older, fineWindow));
    stage = Stage::Refine;
  }

  void runRefine() noexcept {
    const float* newest = fineSegment.data() + fineBack;
    for (int n = 0; n < kRefineLagsPerStep && fineNext < fineLagCount; ++n, ++fineNext) {
      const int lag = fineFirstLag + fineNext;
      const float* older = fineSegment.data() + fineBack - lag;
      const double r = static_cast<double>(dot(newest, older, fineWindow));
      const double m = fineEnergy + fineOlderEnergy;
      fineNsdf[static_cast<size_t>(fineNext)] = m > 1.0e-12 ? static_cast<float>(2.0 * r / m) : 0.0f;
      // The next lag's older window starts one sample earlier and ends one
      // sample earlier: add the sample that enters, drop the one that leaves.
      const double entering = older[-1];
      const double leaving = older[fineWindow - 1];
      fineOlderEnergy += entering * entering - leaving * leaving;
    }
    if (fineNext < fineLagCount)
      return;

    int best = 0;
    for (int i = 1; i < fineLagCount; ++i)
      if (fineNsdf[static_cast<size_t>(i)] > fineNsdf[static_cast<size_t>(best)])
        best = i;
    double lag = coarseLag;
    float found = coarseClarity;
    if (best > 0 && best < fineLagCount - 1) {
      const float y0 = fineNsdf[static_cast<size_t>(best) - 1];
      const float y1 = fineNsdf[static_cast<size_t>(best)];
      const float y2 = fineNsdf[static_cast<size_t>(best) + 1];
      const float denominator = y0 - 2.0f * y1 + y2;
      const float shift = std::abs(denominator) > 1.0e-12f ? 0.5f * (y0 - y2) / denominator : 0.0f;
      lag = static_cast<double>(fineFirstLag + best) + static_cast<double>(juce::jlimit(-1.0f, 1.0f, shift));
      found = std::min(1.0f, std::max(found, y1));
    }
    finishFrame(true, rate / lag, found);
  }

  void finishFrame(bool isVoiced, double hz, float found) noexcept {
    stage = Stage::Idle;
    ++framesDone;
    voiced = isVoiced;
    clarity = isVoiced ? found : (level < kSilenceRms ? 0.0f : found);
    if (!isVoiced) {
      candidateCount = 0;
      return;
    }
    frequency = static_cast<float>(hz);
    haveNote = true;
    const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    if (stableNote >= 0 && std::abs(midi - stableNote) <= 0.65) {
      candidateCount = 0;
      return;
    }
    const int nearest = static_cast<int>(std::lround(midi));
    if (candidateCount > 0 && nearest == candidate)
      ++candidateCount;
    else {
      candidate = nearest;
      candidateCount = 1;
    }
    if (candidateCount >= 2)
      stableNote = candidate;
  }

  double rate{48000.0};
  double coarseRate{12000.0};
  int decimation{4};
  int lagMin{8}, shortLagMax{80}, longLagMax{172}, shortWindow{160}, longWindow{344}, hop{60};
  int coarseRingSize{0}, fineRingSize{0};
  int maxFineWindow{kMaxFineWindow}, lagsPerStep{kLagsPerStep};
  float clarityBar{kClarityThreshold};
  std::vector<float> coarseRing, fineRing, frame, nsdf, fineNsdf, fineSegment;
  std::vector<double> prefix;
  fx::OnePole highpass;
  fx::Biquad lowpass[2];
  int phase{0};
  int64_t coarseWritten{0}, fineWritten{0}, fineSnapshot{0};
  int sinceFrame{0};
  Stage stage{Stage::Idle};
  int nextLag{1};

  double coarseLag{0.0};
  float coarseClarity{0.0f};
  int fineWindow{0}, fineFirstLag{0}, fineLagCount{0}, fineNext{0};
  double fineEnergy{0.0}, fineOlderEnergy{0.0};
  int fineBack{0};

  bool voiced{false}, haveNote{false};
  float frequency{0.0f}, clarity{0.0f}, level{0.0f};
  int stableNote{-1}, candidate{-1}, candidateCount{0};
  int framesDone{0};
};

}  // namespace pitchfx
