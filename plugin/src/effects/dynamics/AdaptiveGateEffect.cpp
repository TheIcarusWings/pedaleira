// dynamics.adaptivegate: a noise gate whose threshold follows the noise floor,
// so the same setting works on a quiet rig and on a noisy one.
//
// - The gate listens to a band-limited copy of the input (high pass 100 Hz,
//   low pass 5 kHz, both second order), so hum and fizz do not hold it open.
// - A floor tracker learns the RMS level of that sidechain while the player
//   is not playing: 50 ms RMS frames, kept as the minimum of each half second
//   over the last 2.5 seconds (minimum statistics), then smoothed on the way
//   up. The floor is clamped to -100 .. -30 dBFS.
// - What counts as "not playing" is the heart of it, because a held note
//   after a high-gain amp or a compressor (a chord, a clipped tone, feedback)
//   is as steady in level as a noise floor. Each frame is judged on a copy of
//   the sidechain decimated to about 9.6 kHz, by two tests; it is PLAYED when
//   either says so:
//     * periodicity: the peak of the normalised autocorrelation over lags of
//       50 Hz to 1.3 kHz is 0.8 or more. Any single note, even with a wide
//       vibrato, sits near 1; noise of any colour stays under 0.65.
//     * spectral stability: the power spectrum (100 ms Hann windows, 9.4 Hz
//       bins, 100 Hz to 4.5 kHz) hardly changes from one frame to the next.
//       Every bin of a noise spectrum is a fresh random draw each frame
//       (whatever the colour of the noise), which puts this measure near
//       0.33; the lines of a chord stay put, clean, compressed or distorted,
//       and it stays under 0.06 (0.13 with a 30 cent vibrato). Played under
//       0.15.
//   A frame feeds the floor when:
//     * it is under the open threshold, and noise-like or under the close
//       threshold (so the slow tail of a note in the hysteresis band is not
//       learned), or
//     * the level has been steady (within 4 dB) for a second AND every frame
//       of that second was noise-like: a noise floor that rose, which would
//       otherwise never be learned. A held note never passes, so the floor
//       cannot climb up under it and cut the player off mid-note.
//   The prices: a floor that rises and is mostly hum is not learned through
//   the steady path (it is learned again once under the threshold), and a
//   chord held with a whammy vibrato of a semitone or more can still look
//   like noise.
// - Open threshold = floor + margin; the gate closes `hysteresis` dB lower.
// - Until a floor has been learned (and with tracking off and nothing
//   learned) the manual threshold is the open threshold.
// - Like dynamics.gate, there is no look-ahead (zero latency): a smooth gain
//   follows the decision, so the attack time is the price of that.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <juce_dsp/juce_dsp.h>
#include "effects/EffectDsp.h"

namespace {

class AdaptiveGateProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "dynamics.adaptivegate";

  enum Param {
    kThresholdDb = 0,
    kMargin,
    kHysteresis,
    kAttackMs,
    kHoldMs,
    kReleaseMs,
    kRangeDb,
    kTrack
  };
  enum Reading { kFloorDb = 0, kOpenThresholdDb, kGateOpen };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      // The open threshold before a floor is learned, or with tracking off
      // and nothing learned.
      t.push_back(BlockParamSpec::floatParam("thresholdDb", "Manual Threshold", "dB", -80.0f, 0.0f, -50.0f));
      // How far above the noise floor the gate opens.
      t.push_back(BlockParamSpec::floatParam("margin", "Margin", "dB", 3.0f, 24.0f, 10.0f));
      t.push_back(BlockParamSpec::floatParam("hysteresis", "Hysteresis", "dB", 0.0f, 12.0f, 4.0f));
      t.push_back(BlockParamSpec::floatParam("attackMs", "Attack", "ms", 0.1f, 20.0f, 1.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("holdMs", "Hold", "ms", 0.0f, 500.0f, 50.0f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("releaseMs", "Release", "ms", 5.0f, 1000.0f, 120.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("rangeDb", "Range", "dB", -90.0f, 0.0f, -60.0f));
      // Off freezes the learned floor.
      t.push_back(BlockParamSpec::boolParam("track", "Track", true));
      return t;
    }();
    return table;
  }

  // For the UI: the learned floor, the open threshold in force, and whether
  // the gate is open (1) or closed (0).
  static const BlockReadingTable& readingSpecs() {
    static const BlockReadingTable table = {{"floorDb", "Noise floor", "dB", -100.0f, -30.0f},
                                            {"openThresholdDb", "Open threshold", "dB", -100.0f, 0.0f},
                                            {"gateOpen", "Gate open", "", 0.0f, 1.0f}};
    return table;
  }

  AdaptiveGateProcessor()
      : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    detectorStep = stepCoefficient(kDetectorMs);
    frameLength = juce::jmax(1, static_cast<int>(std::lround(kFrameSeconds * sampleRate)));
    decimation = juce::jmax(1, static_cast<int>(std::lround(sampleRate / kDecimatedRateHz)));
    const double decimatedRate = sampleRate / decimation;
    lagLowest = juce::jlimit(1, kMaxLag, static_cast<int>(std::floor(decimatedRate / kPitchHighestHz)));
    lagHighest = juce::jlimit(lagLowest, kMaxLag, static_cast<int>(std::ceil(decimatedRate / kPitchLowestHz)));
    windowLength = juce::jlimit(16, kFftSize, 2 * (frameLength / decimation));
    for (int i = 0; i < windowLength; ++i)
      window[static_cast<size_t>(i)] =
          static_cast<float>(0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * (i + 0.5) / windowLength));
    const double binHz = decimatedRate / kFftSize;
    binLowest = juce::jlimit(1, kSpectrumBins - 1, static_cast<int>(std::ceil(kSpectrumLowestHz / binHz)));
    binHighest = juce::jlimit(binLowest, kSpectrumBins - 1,
                              static_cast<int>(std::floor(juce::jmin(kSpectrumHighestHz, 0.45 * decimatedRate) / binHz)));
    if (fft == nullptr)
      fft = std::make_unique<juce::dsp::FFT>(kFftOrder);
    const fx::BiquadCoeffs hp = fx::BiquadCoeffs::highpass(kSidechainHighPassHz, 0.7071, sampleRate);
    const fx::BiquadCoeffs lp = fx::BiquadCoeffs::lowpass(kSidechainLowPassHz, 0.7071, sampleRate);
    for (auto& lane : lanes)
      for (int c = 0; c < kMaxChannels; ++c) {
        lane.hp[c].setCoeffs(hp);
        lane.lp[c].setCoeffs(lp);
      }
    resetEffect();
  }

  void resetEffect() noexcept override {
    for (auto& lane : lanes)
      lane.clear();
    fresh = true;
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    // One stereo signal is gated as one; dual mono is two guitars, each with
    // its own floor.
    const bool linked = numChannels == 2 && !ctx.dualMono;
    const int numLanes = linked ? 1 : numChannels;
    const int sideChannels = linked ? 2 : 1;

    const Settings settings{param(kThresholdDb),
                            param(kMargin),
                            param(kHysteresis),
                            param(kTrack) >= 0.5f};
    const float rangeGain = fx::dbToGain(param(kRangeDb));
    const int holdSamples = static_cast<int>(std::lround(param(kHoldMs) * 0.001 * sampleRate));
    const float attack = stepCoefficient(param(kAttackMs));
    const float release = stepCoefficient(param(kReleaseMs));

    if (fresh) {
      for (auto& lane : lanes)
        lane.gain = rangeGain;
      fresh = false;
    }

    float* channels[kMaxChannels] = {buffer.getWritePointer(0),
                                     numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    for (int l = 0; l < numLanes; ++l) {
      Lane& s = lanes[l];
      updateThresholds(s, settings);
      for (int i = 0; i < numSamples; ++i) {
        // The sidechain: band-limited power (the larger side when linked).
        float power = 0.0f;
        float sum = 0.0f;
        for (int c = 0; c < sideChannels; ++c) {
          const int src = linked ? c : l;
          const float v = s.lp[c].process(s.hp[c].process(channels[src][i]));
          power = juce::jmax(power, v * v);
          sum += v;
        }
        power += 1.0e-18f;  // keeps the smoother off denormals

        if (settings.track) {
          s.decimatedSum += sum;
          if (++s.decimatedCount >= decimation) {
            pushDecimated(s, s.decimatedSum);
            s.decimatedSum = 0.0f;
            s.decimatedCount = 0;
          }
        }

        s.power += detectorStep * (power - s.power);
        s.frameSum += power;
        if (++s.frameCount >= frameLength) {
          endFrame(s, settings);
          updateThresholds(s, settings);
        }

        if (s.power > s.openPower) {
          s.open = true;
          s.hold = holdSamples;
        } else if (s.open) {
          if (s.power > s.closePower)
            s.hold = holdSamples;
          else if (s.hold > 0)
            --s.hold;
          else
            s.open = false;
        }

        const float target = s.open ? 1.0f : rangeGain;
        s.gain += (target > s.gain ? attack : release) * (target - s.gain);

        channels[l][i] *= s.gain;
        if (linked)
          channels[1][i] *= s.gain;
      }
    }

    bool anyOpen = false;
    for (int l = 0; l < numLanes; ++l)
      anyOpen = anyOpen || lanes[l].open;
    setReading(kFloorDb, lanes[0].floorDb);
    setReading(kOpenThresholdDb, lanes[0].openDb);
    setReading(kGateOpen, anyOpen ? 1.0f : 0.0f);
  }

private:
  static constexpr int kMaxChannels = 2;
  static constexpr double kDetectorMs = 4.0;
  static constexpr double kSidechainHighPassHz = 100.0;
  static constexpr double kSidechainLowPassHz = 5000.0;
  static constexpr double kFrameSeconds = 0.05;
  static constexpr int kFramesPerBucket = 10;  // half a second
  static constexpr int kBuckets = 5;           // 2.5 s of minimum statistics
  static constexpr int kSteadyFrames = 20;     // one second
  static constexpr float kSteadyRangeDb = 4.0f;
  static constexpr float kFloorRiseStep = 0.15f;  // per frame, ~0.3 s
  // The minimum of many frames sits a little under the mean level of noise.
  static constexpr float kMinimumBiasDb = 0.6f;
  static constexpr float kFloorLowestDb = -100.0f;
  static constexpr float kFloorHighestDb = -30.0f;
  static constexpr float kNoFrame = 1.0e9f;
  // The played-or-noise detector: the sidechain decimated to about this
  // rate; autocorrelation lags from 1.3 kHz down to 50 Hz (drop tunings) and
  // the peak from which a frame is periodic; the spectrum's window (two
  // frames, at most kFftSize samples), band, and the stability under which a
  // frame is a held note or chord.
  static constexpr double kDecimatedRateHz = 9600.0;
  static constexpr double kPitchHighestHz = 1300.0;
  static constexpr double kPitchLowestHz = 50.0;
  static constexpr float kPeriodicPeak = 0.8f;
  static constexpr int kFftOrder = 10;
  static constexpr int kFftSize = 1 << kFftOrder;
  static constexpr int kSpectrumBins = kFftSize / 2;
  static constexpr double kSpectrumLowestHz = 100.0;
  static constexpr double kSpectrumHighestHz = 4500.0;
  static constexpr float kStableSpectrum = 0.15f;
  static constexpr int kMaxLag = 256;
  static constexpr int kHistory = kFftSize;    // decimated samples kept before a frame
  static constexpr int kMaxDecimatedFrame = 1024;
  static constexpr int kDecimatedSize = kHistory + kMaxDecimatedFrame;

  struct Settings {
    float manualDb;
    float marginDb;
    float hysteresisDb;
    bool track;
  };

  struct Lane {
    fx::Biquad hp[kMaxChannels];
    fx::Biquad lp[kMaxChannels];
    float power{0.0f};
    float gain{0.0f};
    int hold{0};
    bool open{false};

    // Floor tracker.
    double frameSum{0.0};
    int frameCount{0};
    float recent[kSteadyFrames]{};
    int recentFill{0};
    int recentPos{0};
    int noiseLikeRun{0};  // consecutive noise-like frames

    // Played-or-noise detector: the decimated sidechain (kHistory samples
    // of history, then the current frame), prefix sums of its squares, the
    // running products per lag, and the last frame's spectrum and stability
    // terms.
    float decimatedSum{0.0f};
    int decimatedCount{0};
    int decimatedPos{kHistory};
    float decimated[kDecimatedSize];
    double decimatedEnergy[kDecimatedSize + 1];
    float lagProducts[kMaxLag];
    float lastSpectrum[kSpectrumBins];
    double lastChange{0.0};
    double lastSpread{0.0};
    float bucketMin[kBuckets];
    int bucket{0};
    int bucketFrames{0};
    int bucketsDone{0};
    bool learned{false};
    float floorDb{kFloorLowestDb};

    float openDb{-50.0f};
    float openPower{0.0f};
    float closePower{0.0f};

    Lane() { clear(); }

    void clear() noexcept {
      for (auto& f : hp)
        f.reset();
      for (auto& f : lp)
        f.reset();
      power = gain = 0.0f;
      hold = 0;
      open = false;
      frameSum = 0.0;
      frameCount = 0;
      recentFill = recentPos = noiseLikeRun = 0;
      decimatedSum = 0.0f;
      decimatedCount = 0;
      decimatedPos = kHistory;
      for (auto& v : decimated)
        v = 0.0f;
      for (auto& v : decimatedEnergy)
        v = 0.0;
      for (auto& v : lagProducts)
        v = 0.0f;
      for (auto& v : lastSpectrum)
        v = 0.0f;
      lastChange = lastSpread = 0.0;
      for (auto& m : bucketMin)
        m = kNoFrame;
      bucket = bucketFrames = bucketsDone = 0;
      learned = false;
      floorDb = kFloorLowestDb;
      openDb = -50.0f;
      openPower = closePower = 0.0f;
    }
  };

  static float powerFromDb(float db) noexcept { return std::pow(10.0f, 0.1f * db); }

  // The open threshold in force: floor + margin once a floor is known,
  // otherwise the manual threshold.
  static void updateThresholds(Lane& s, const Settings& st) noexcept {
    s.openDb = s.learned ? s.floorDb + st.marginDb : st.manualDb;
    s.openPower = powerFromDb(s.openDb);
    s.closePower = powerFromDb(s.openDb - st.hysteresisDb);
  }

  // One decimated sidechain sample: its product with every lagged sample,
  // and its square into the prefix sums.
  void pushDecimated(Lane& s, float x) const noexcept {
    if (s.decimatedPos >= kDecimatedSize)
      return;
    const int p = s.decimatedPos;
    s.decimated[p] = x;
    s.decimatedEnergy[p + 1] = s.decimatedEnergy[p] + static_cast<double>(x) * x;
    const float* lagged = s.decimated + p - lagLowest;
    const int count = lagHighest - lagLowest + 1;
    for (int k = 0; k < count; ++k)
      s.lagProducts[k] += x * lagged[-k];
    s.decimatedPos = p + 1;
  }

  // Whether the frame just ended is played (periodic, or a spectrum that
  // holds still) rather than noise; then the last kHistory samples become
  // the history of the next frame.
  bool endDecimatedFrame(Lane& s) noexcept {
    const int end = s.decimatedPos;
    const double frameEnergy = s.decimatedEnergy[end] - s.decimatedEnergy[kHistory];
    bool played = false;
    if (end > kHistory && frameEnergy > 1.0e-14) {
      // Periodicity.
      float peak = 0.0f;
      const int count = lagHighest - lagLowest + 1;
      for (int k = 0; k < count; ++k) {
        const int lag = lagLowest + k;
        const double laggedEnergy = s.decimatedEnergy[end - lag] - s.decimatedEnergy[kHistory - lag];
        const double r = s.lagProducts[k] / std::sqrt(frameEnergy * laggedEnergy + 1.0e-30);
        peak = juce::jmax(peak, static_cast<float>(r));
      }

      // Spectral stability: sum (P - Plast)^2 / sum (P + Plast)^2 over the
      // band, pooled over this frame and the one before.
      const float* x = s.decimated + end - windowLength;
      for (int i = 0; i < windowLength; ++i)
        fftData[static_cast<size_t>(i)] = x[i] * window[static_cast<size_t>(i)];
      std::fill(fftData.begin() + windowLength, fftData.end(), 0.0f);
      fft->performRealOnlyForwardTransform(fftData.data(), true);
      double change = 0.0, spread = 0.0;
      for (int k = binLowest; k <= binHighest; ++k) {
        const float re = fftData[static_cast<size_t>(2 * k)];
        const float im = fftData[static_cast<size_t>(2 * k + 1)];
        const float power = re * re + im * im;
        const double d = power - s.lastSpectrum[k];
        const double t = power + s.lastSpectrum[k];
        change += d * d;
        spread += t * t;
        s.lastSpectrum[k] = power;
      }
      const double stability = (change + s.lastChange) / (spread + s.lastSpread + 1.0e-60);
      s.lastChange = change;
      s.lastSpread = spread;
      played = peak >= kPeriodicPeak || stability < kStableSpectrum;
    } else {
      for (auto& v : s.lastSpectrum)
        v = 0.0f;
      s.lastChange = s.lastSpread = 0.0;
    }

    std::copy(s.decimated + end - kHistory, s.decimated + end, s.decimated);
    s.decimatedEnergy[0] = 0.0;
    for (int i = 0; i < kHistory; ++i)
      s.decimatedEnergy[i + 1] = s.decimatedEnergy[i] + static_cast<double>(s.decimated[i]) * s.decimated[i];
    std::fill(std::begin(s.lagProducts), std::end(s.lagProducts), 0.0f);
    s.decimatedPos = kHistory;
    return played;
  }

  // One 50 ms RMS frame of the sidechain: feeds the minimum statistics when
  // the player is judged not to be playing.
  void endFrame(Lane& s, const Settings& st) noexcept {
    const float meanSquare = static_cast<float>(s.frameSum / s.frameCount);
    s.frameSum = 0.0;
    s.frameCount = 0;
    if (!st.track)
      return;

    const bool noiseLike = !endDecimatedFrame(s);
    s.noiseLikeRun = noiseLike ? juce::jmin(s.noiseLikeRun + 1, kSteadyFrames) : 0;

    const float rmsDb = juce::jlimit(-120.0f, 6.0f, 10.0f * std::log10(meanSquare + 1.0e-13f));

    s.recent[s.recentPos] = rmsDb;
    s.recentPos = (s.recentPos + 1) % kSteadyFrames;
    s.recentFill = juce::jmin(s.recentFill + 1, kSteadyFrames);
    bool steady = false;
    if (s.recentFill == kSteadyFrames) {
      float lowest = s.recent[0], highest = s.recent[0];
      for (int i = 1; i < kSteadyFrames; ++i) {
        lowest = juce::jmin(lowest, s.recent[i]);
        highest = juce::jmax(highest, s.recent[i]);
      }
      steady = highest - lowest < kSteadyRangeDb;
    }

    const bool quiet = rmsDb < s.openDb && (noiseLike || rmsDb < s.openDb - st.hysteresisDb);
    const bool risenNoise = steady && s.noiseLikeRun >= kSteadyFrames;
    if (quiet || risenNoise)
      s.bucketMin[s.bucket] = juce::jmin(s.bucketMin[s.bucket], rmsDb);

    float lowestFrame = kNoFrame;
    for (const float m : s.bucketMin)
      lowestFrame = juce::jmin(lowestFrame, m);
    if (s.bucketsDone >= 1 && lowestFrame < kNoFrame) {
      const float candidate =
          juce::jlimit(kFloorLowestDb, kFloorHighestDb, lowestFrame + kMinimumBiasDb);
      if (!s.learned || candidate < s.floorDb)
        s.floorDb = candidate;  // down at once, up smoothly
      else
        s.floorDb += kFloorRiseStep * (candidate - s.floorDb);
      s.learned = true;
    }

    if (++s.bucketFrames >= kFramesPerBucket) {
      s.bucketFrames = 0;
      s.bucket = (s.bucket + 1) % kBuckets;
      s.bucketMin[s.bucket] = kNoFrame;
      s.bucketsDone = juce::jmin(s.bucketsDone + 1, 1000);
    }
  }

  float stepCoefficient(double ms) const noexcept {
    return ms > 0.0 ? static_cast<float>(-std::expm1(-1.0 / (0.001 * ms * sampleRate))) : 1.0f;
  }

  Lane lanes[kMaxChannels];
  double sampleRate{48000.0};
  float detectorStep{0.0f};
  int frameLength{2400};
  int decimation{5};
  int lagLowest{7};
  int lagHighest{192};
  int windowLength{960};
  int binLowest{11};
  int binHighest{480};
  std::array<float, kFftSize> window{};
  std::array<float, 2 * kFftSize> fftData{};
  std::unique_ptr<juce::dsp::FFT> fft;
  bool fresh{true};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(AdaptiveGateEffect) {
  addEffectKind<AdaptiveGateProcessor>(registry, "Adaptive Gate", BlockCategory::Dynamics);
}
