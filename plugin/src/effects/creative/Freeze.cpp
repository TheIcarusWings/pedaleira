// filter.freeze: holds a moment of the input as a smooth, endless pad.
//
// The input is always being written to a ring of the last second. When the
// `freeze` switch goes on, the latest `grainMs` of it is copied into one of
// two capture buffers (a switch turned on again before the last pad has
// faded away captures into the other one, so the pad that is dying keeps its
// own audio). From then on the pad is made of overlapping grains: every
// quarter of a grain length a new grain starts, at a random place in the
// captured window, under a raised-cosine (Hann) window. Four Hann windows a
// quarter apart add up their squares to a constant 1.5 (the cosine terms
// cancel), so grains that are not in step with each other, as these are not,
// make a pad whose power does not wander; the pad is scaled by 1/sqrt(1.5)
// to come out at the level of the captured audio. Grains of a tone are in
// step by chance only, so a pad of tonal material beats; a slow leveller
// (about 150 ms) holds its power at the captured audio's. Because the offsets are
// random, the grain rate is not a pitch and the pad does not buzz.
//
// The random offsets come from a hash of a grain counter that restarts with
// every capture, so a render repeats exactly. In a true stereo image the right
// side draws its own offsets (a wider pad); in mono and in dual mono every
// channel draws the same ones, so a channel is what a mono rig makes of it.
//
// Engaging crossfades the live signal into the pad over `fadeInMs`, equal
// power; releasing fades the pad out over `fadeOutMs` and the live signal
// back in. While frozen, `dry` keeps that share of the live signal under the
// pad. `level` trims the pad only. The wet path is the input itself whenever
// the pad is not sounding, so the block's mix behaves as usual and the block
// adds no latency.
#include <algorithm>
#include <cmath>
#include <vector>
#include "effects/EffectDsp.h"

namespace {

class FreezeProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "filter.freeze";

  enum Param { kFreeze = 0, kGrainMs, kFadeInMs, kFadeOutMs, kLevelDb, kDryPercent };

  static constexpr int kMaxChannels = 2;
  static constexpr double kMaxGrainSeconds = 1.0;
  static constexpr int kVoices = 4;

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      BlockParamTable t;
      t.push_back(BlockParamSpec::boolParam("freeze", "Freeze", false).withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("grainMs", "Grain", "ms", 50.0f, 1000.0f, 300.0f)
                      .withSkew(0.5f));
      t.push_back(BlockParamSpec::floatParam("fadeInMs", "Fade In", "ms", 5.0f, 3000.0f, 150.0f)
                      .withSkew(0.4f));
      t.push_back(BlockParamSpec::floatParam("fadeOutMs", "Fade Out", "ms", 5.0f, 5000.0f,
                                             600.0f)
                      .withSkew(0.4f)
                      .withAffectsTail());
      t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -24.0f, 12.0f, 0.0f));
      t.push_back(BlockParamSpec::floatParam("dryPercent", "Dry", "%", 0.0f, 100.0f, 0.0f));
      return t;
    }();
    return table;
  }

  FreezeProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs()) {}

  // Off, only the fade-out of a pad that is still sounding; on, the pad has
  // no end (the chain caps what is reported at kMaxBlockTailSeconds).
  double tailSeconds() const noexcept override {
    if (param(kFreeze) >= 0.5f)
      return kMaxBlockTailSeconds;
    return static_cast<double>(param(kFadeOutMs)) * 0.001 + 0.01;
  }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    ringSize = static_cast<size_t>(
        juce::nextPowerOfTwo(static_cast<int>(std::ceil(kMaxGrainSeconds * sampleRate)) + 8));
    ringMask = ringSize - 1;
    captureLength = static_cast<size_t>(std::ceil(kMaxGrainSeconds * sampleRate)) + 8;
    ring.assign(static_cast<size_t>(kMaxChannels) * ringSize, 0.0f);
    for (auto& buffer : captured)
      buffer.assign(static_cast<size_t>(kMaxChannels) * captureLength, 0.0f);
    refBuffer.assign(static_cast<size_t>(0.01 * sampleRate), 0.0f);
    level.prepare(sampleRate, 0.02);
    dry.prepare(sampleRate, 0.02);
    resetEffect();
  }

  void resetEffect() noexcept override {
    std::fill(ring.begin(), ring.end(), 0.0f);
    for (auto& buffer : captured)
      std::fill(buffer.begin(), buffer.end(), 0.0f);
    writePos = 0;
    padAmount = 0.0;
    targetPower = 0.0f;
    smoothedPower = 0.0f;
    levelling = 1.0f;
    frozen = false;
    capturedBuffer = 1;
    window = 0;
    grainLength = 0;
    grainHop = 1;
    untilGrain = 0;
    grainCounter = 0;
    nextVoice = 0;
    for (auto& voice : voices)
      voice.active = false;
    level.snapOnNextTarget();
    dry.snapOnNextTarget();
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override {
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
    const bool stereoImage = blockHasStereoImage(numChannels, ctx);

    const bool wantFrozen = param(kFreeze) >= 0.5f;
    if (wantFrozen && !frozen)
      capture(stereoImage, numChannels);
    frozen = wantFrozen;

    // The pad's amount moves from 0 to 1 over the fade-in while frozen and
    // back over the fade-out otherwise.
    const double upStep = 1.0 / std::max(1.0, 0.001 * static_cast<double>(param(kFadeInMs)) * sampleRate);
    const double downStep =
        1.0 / std::max(1.0, 0.001 * static_cast<double>(param(kFadeOutMs)) * sampleRate);
    level.setTarget(juce::Decibels::decibelsToGain(param(kLevelDb)));
    dry.setTarget(param(kDryPercent) * 0.01f);

    float* data[kMaxChannels] = {buffer.getWritePointer(0),
                                 numChannels > 1 ? buffer.getWritePointer(1) : nullptr};
    constexpr float kHalfPi = 1.57079632679f;
    // Hann windows squared, kVoices of them a grain length / kVoices apart, add
    // up to kVoices * 3/8, whatever the position.
    const float kGrainNormal = 1.0f / std::sqrt(0.375f * static_cast<float>(kVoices));
    const float powerCoeff = 1.0f - std::exp(-1.0f / (0.15f * static_cast<float>(sampleRate)));
    const float gainCoeff = 1.0f - std::exp(-1.0f / (0.10f * static_cast<float>(sampleRate)));

    for (int i = 0; i < numSamples; ++i) {
      float live[kMaxChannels] = {0.0f, 0.0f};
      for (int ch = 0; ch < numChannels; ++ch)
        live[ch] = data[ch][i];

      // The ring always takes the input; a capture copies out of it.
      for (int ch = 0; ch < numChannels; ++ch)
        ring[static_cast<size_t>(ch) * ringSize + writePos] = live[ch];
      writePos = (writePos + 1) & ringMask;

      if (padAmount > 0.0 || frozen) {
        padAmount = frozen ? std::min(1.0, padAmount + upStep)
                           : std::max(0.0, padAmount - downStep);

        if (window > 0 && untilGrain == 0) {
          startGrain(stereoImage);
          untilGrain = grainHop;
        }
        if (untilGrain > 0)
          --untilGrain;

        float pad[kMaxChannels] = {0.0f, 0.0f};
        for (Voice& voice : voices) {
          if (!voice.active)
            continue;
          const float w = voice.weight();
          const std::vector<float>& source = captured[voice.buffer];
          for (int ch = 0; ch < numChannels; ++ch) {
            const size_t offset =
                voice.offset[stereoImage ? ch : 0] + static_cast<size_t>(voice.position);
            pad[ch] += w * source[static_cast<size_t>(ch) * captureLength + offset];
          }
          if (++voice.position >= voice.length)
            voice.active = false;
        }

        // The leveller: the pad's power, slowly averaged, against the power
        // of the audio it was made from.
        if (targetPower > 1.0e-12f) {
          float power = 0.0f;
          for (int ch = 0; ch < numChannels; ++ch)
            power += pad[ch] * pad[ch] * kGrainNormal * kGrainNormal;
          power /= static_cast<float>(numChannels);
          smoothedPower += powerCoeff * (power - smoothedPower);
          const float wanted =
              juce::jlimit(0.25f, 4.0f, std::sqrt(targetPower / std::max(smoothedPower, 1.0e-12f)));
          levelling += gainCoeff * (wanted - levelling);
        }
        const float amount = static_cast<float>(padAmount);
        const float padGain = std::sin(kHalfPi * amount) * kGrainNormal * levelling * level.next();
        const float liveShare = dry.next();
        const float liveGain = liveShare + (1.0f - liveShare) * std::cos(kHalfPi * amount);
        for (int ch = 0; ch < numChannels; ++ch)
          data[ch][i] = live[ch] * liveGain + pad[ch] * padGain;
      } else {
        level.skip(1);
        dry.skip(1);
      }
    }
  }

private:
  struct Voice {
    bool active{false};
    int buffer{0};
    int position{0};
    int length{1};
    // Start of the grain in the captured window, per channel.
    size_t offset[kMaxChannels] = {0, 0};

    // Hann window over the grain.
    float weight() const noexcept { return weightAt(position); }
    float weightAt(int at) const noexcept {
      const float x = (static_cast<float>(at) + 0.5f) / static_cast<float>(length);
      return 0.5f - 0.5f * std::cos(6.28318530718f * x);
    }
  };

  // Copies the latest `grainMs` of the ring into the capture buffer the pad
  // that is fading out is not reading, and restarts the grain stream.
  void capture(bool stereoImage, int activeChannels) noexcept {
    capturedBuffer ^= 1;
    window = juce::jlimit<size_t>(
        64, captureLength - 8,
        static_cast<size_t>(0.001 * static_cast<double>(param(kGrainMs)) * sampleRate));
    for (int ch = 0; ch < kMaxChannels; ++ch)
      for (size_t n = 0; n < window; ++n)
        captured[capturedBuffer][static_cast<size_t>(ch) * captureLength + n] =
            ring[static_cast<size_t>(ch) * ringSize + ((writePos + ringSize - window + n) & ringMask)];
    // What the leveller aims at: the power of the captured audio.
    double sum = 0.0;
    const int sides = std::max(1, activeChannels);
    for (int ch = 0; ch < sides; ++ch)
      for (size_t n = 0; n < window; ++n) {
        const double v = captured[capturedBuffer][static_cast<size_t>(ch) * captureLength + n];
        sum += v * v;
      }
    targetPower = static_cast<float>(sum / (static_cast<double>(window) * sides));
    smoothedPower = targetPower;
    levelling = 1.0f;
    // Half the window is a grain; the rest is where it may start.
    grainLength = static_cast<int>(window / 2);
    grainHop = std::max(1, grainLength / kVoices);
    untilGrain = 0;
    grainCounter = 0;
    // From silence, start with the four grains a running pad would have, a
    // hop apart, so it is at full level at once; over a pad that is still
    // fading out the new grains come in one by one instead.
    bool idle = true;
    for (const Voice& voice : voices)
      idle = idle && !voice.active;
    if (idle) {
      for (int k = 0; k < kVoices; ++k) {
        startGrain(stereoImage);
        voices[(nextVoice + kVoices - 1) % kVoices].position = (kVoices - 1 - k) * grainHop;
      }
      untilGrain = grainHop;
    }
  }

  void startGrain(bool stereoImage) noexcept {
    Voice& voice = voices[nextVoice];
    // A grain of an older pad that outlasts its slot is left to finish: the
    // next one waits for its turn rather than cut it.
    if (voice.active)
      return;
    nextVoice = (nextVoice + 1) % kVoices;
    voice.active = true;
    voice.buffer = capturedBuffer;
    voice.position = 0;
    voice.length = grainLength;
    const size_t room = window - static_cast<size_t>(grainLength);
    for (int ch = 0; ch < kMaxChannels; ++ch) {
      const std::uint32_t salt = (stereoImage && ch == 1) ? 0x51ed270bu : 0u;
      const float unit =
          static_cast<float>(fx::hash32(grainCounter * 0x9e3779b1u + salt) >> 8) * (1.0f / 16777216.0f);
      const size_t base = std::min(room, static_cast<size_t>(unit * static_cast<float>(room + 1)));
      voice.offset[ch] = alignedOffset(ch, base, room);
    }
    ++grainCounter;
  }

  // Moves a grain's random start by up to 5 ms either way to where its first
  // 10 ms best follow the grains already playing (the largest normalised
  // correlation with their sum, over the same stretch), so that a tone in the
  // captured audio does not meet itself in opposite phase. The place is still
  // random to within those 5 ms, which is under a cycle of anything below
  // 200 Hz and does not make the grain rate audible. Searches every second
  // shift, on every second sample.
  size_t alignedOffset(int ch, size_t base, size_t room) noexcept {
    const size_t length = refBuffer.size();
    if (length == 0)
      return base;
    float energy = 0.0f;
    for (size_t n = 0; n < length; ++n) {
      float sum = 0.0f;
      for (const Voice& other : voices) {
        const int at = other.position + static_cast<int>(n);
        if (!other.active || at >= other.length)
          continue;
        sum += other.weightAt(at) *
               captured[other.buffer][static_cast<size_t>(ch) * captureLength +
                                      other.offset[ch] + static_cast<size_t>(at)];
      }
      refBuffer[n] = sum;
      energy += sum * sum;
    }
    if (energy < 1.0e-12f)
      return base;
    const long reach = static_cast<long>(0.005 * sampleRate);
    const std::vector<float>& source = captured[capturedBuffer];
    size_t best = base;
    float bestScore = -1.0e30f;
    for (long shift = -reach; shift <= reach; shift += 2) {
      const long candidate = static_cast<long>(base) + shift;
      if (candidate < 0 || static_cast<size_t>(candidate) > room)
        continue;
      const float* cand = source.data() + static_cast<size_t>(ch) * captureLength +
                          static_cast<size_t>(candidate);
      float dot = 0.0f, power = 0.0f;
      for (size_t n = 0; n < length; n += 2) {
        dot += refBuffer[n] * cand[n];
        power += cand[n] * cand[n];
      }
      const float score = dot / std::sqrt(power + 1.0e-12f);
      if (score > bestScore) {
        bestScore = score;
        best = static_cast<size_t>(candidate);
      }
    }
    // Noise correlates with nothing: a best match that is not clearly a
    // match is a coincidence, and aligning to it would only make the grains
    // add up in amplitude. Only a tone is followed.
    return bestScore > 0.6f * std::sqrt(energy) ? best : base;
  }

  std::vector<float> ring;
  std::vector<float> refBuffer;
  std::vector<float> captured[2];
  size_t ringSize{0};
  size_t ringMask{0};
  size_t captureLength{0};
  size_t writePos{0};
  size_t window{0};

  Voice voices[kVoices];
  int nextVoice{0};
  int capturedBuffer{1};
  int grainLength{0};
  int grainHop{1};
  int untilGrain{0};
  std::uint32_t grainCounter{0};

  double padAmount{0.0};
  float targetPower{0.0f};
  float smoothedPower{0.0f};
  float levelling{1.0f};
  bool frozen{false};
  fx::SmoothedParam level;
  fx::SmoothedParam dry;
  double sampleRate{48000.0};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(Freeze) {
  // The wet path is the live signal until the switch is on; fully wet to
  // start, so the block's mix is only needed to blend the pad under a part.
  addEffectKind<FreezeProcessor>(registry, "Freeze", BlockCategory::Filter, 1.0f,
                                 BlockMixLaw::Crossfade);
}
