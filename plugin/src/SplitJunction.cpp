#include "SplitJunction.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

constexpr double kSqrt2 = 1.41421356237309504880;

// dB to gain with the junction's floor: at or below it the path is off.
float levelGain(float db) noexcept {
  return juce::Decibels::decibelsToGain(db, SplitJunction::kLevelFloorDb);
}

// The balance law utility.gain uses: unity on both sides at centre, the
// far side fading linearly to zero. Exact at centre and at both ends.
void balanceLaw(float pan, float& left, float& right) noexcept {
  left = pan > 0.0f ? 1.0f - pan : 1.0f;
  right = pan < 0.0f ? 1.0f + pan : 1.0f;
}

// Constant-power placement of a mono path in a stereo result, scaled by
// sqrt(2) so centre is unity on both sides: a mono row joined at the
// defaults then sums exactly like a stereo one, and the total power stays
// the same wherever the pan sits (a hard side is +3 dB on that side).
// Centre and the ends are written out so they are exact.
void constantPowerLaw(float pan, float& left, float& right) noexcept {
  const float root2 = static_cast<float>(kSqrt2);
  if (pan == 0.0f) {
    left = right = 1.0f;
  } else if (pan <= -1.0f) {
    left = root2;
    right = 0.0f;
  } else if (pan >= 1.0f) {
    left = 0.0f;
    right = root2;
  } else {
    const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    left = root2 * std::cos(angle);
    right = root2 * std::sin(angle);
  }
}

// a at t = 0, b at t = 1, and exactly those at the ends (the plain
// a + t * (b - a) is not exact at t = 1 in floating point).
template <typename T>
T blend(T a, T b, float t) noexcept {
  if (t <= 0.0f)
    return a;
  if (t >= 1.0f)
    return b;
  return a + static_cast<T>(t) * (b - a);
}

int liveChannels(int wanted, const juce::AudioBuffer<float>& buffer) noexcept {
  return juce::jlimit(1, 2, juce::jmin(wanted, buffer.getNumChannels()));
}

}  // namespace

juce::String splitterTypeName(SplitterType type) {
  switch (type) {
    case SplitterType::Balance:
      return "balance";
    case SplitterType::AB:
      return "ab";
    case SplitterType::Crossover:
      return "crossover";
  }
  return "balance";
}

bool splitterTypeFromName(const juce::String& name, SplitterType& type) {
  for (const SplitterType candidate :
       {SplitterType::Balance, SplitterType::AB, SplitterType::Crossover}) {
    if (name == splitterTypeName(candidate)) {
      type = candidate;
      return true;
    }
  }
  return false;
}

const BlockParamTable& SplitJunction::parameterSpecs() {
  static const BlockParamTable table = [] {
    BlockParamTable t;
    t.push_back(BlockParamSpec::floatParam("split.balance", "Balance", "", -1.0f, 1.0f, 0.0f));
    t.push_back(BlockParamSpec::floatParam("split.levelA", "A Level", "dB", kLevelFloorDb,
                                           kLevelMaxDb, 0.0f));
    t.push_back(BlockParamSpec::floatParam("split.levelB", "B Level", "dB", kLevelFloorDb,
                                           kLevelMaxDb, 0.0f));
    // The skew gives the low octaves their share of the knob; the value is
    // always in Hz.
    t.push_back(BlockParamSpec::floatParam("split.crossoverHz", "Crossover", "Hz",
                                           kCrossoverMinHz, kCrossoverMaxHz, 500.0f)
                    .withSkew(0.3f));
    t.push_back(BlockParamSpec::boolParam("split.swap", "Swap Bands", false));
    t.push_back(BlockParamSpec::boolParam("split.stereo", "Stereo Split", false));
    t.push_back(BlockParamSpec::floatParam("mix.levelA", "A Level", "dB", kLevelFloorDb,
                                           kLevelMaxDb, 0.0f));
    t.push_back(BlockParamSpec::floatParam("mix.panA", "A Pan", "", -1.0f, 1.0f, 0.0f));
    t.push_back(BlockParamSpec::floatParam("mix.levelB", "B Level", "dB", kLevelFloorDb,
                                           kLevelMaxDb, 0.0f));
    t.push_back(BlockParamSpec::floatParam("mix.panB", "B Pan", "", -1.0f, 1.0f, 0.0f));
    t.push_back(BlockParamSpec::boolParam("mix.invertB", "B Polarity", false));
    t.push_back(BlockParamSpec::floatParam("mix.level", "Level", "dB", kLevelFloorDb,
                                           kLevelMaxDb, 0.0f));
    t.push_back(BlockParamSpec::boolParam("mix.mute", "Mute", false));
    jassert(static_cast<int>(t.size()) == kNumParams);
    return t;
  }();
  return table;
}

SplitJunction::SplitJunction() {
  const BlockParamTable& table = parameterSpecs();
  for (size_t i = 0; i < values.size(); ++i)
    values[i].store(table[i].defaultValue, std::memory_order_relaxed);
  for (auto& meter : splitMeter)
    meter.store(kMeterFloorDb, std::memory_order_relaxed);
  for (auto& meter : mixMeter)
    meter.store(kMeterFloorDb, std::memory_order_relaxed);
  prepare(sampleRate, 0);
}

// ---------------------------------------------------------------------------
// Parameters and state
// ---------------------------------------------------------------------------

int SplitJunction::findParameter(const juce::String& paramId) {
  return findBlockParam(parameterSpecs(), paramId);
}

float SplitJunction::getParameter(int index) const noexcept {
  if (index < 0 || index >= kNumParams)
    return 0.0f;
  return values[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void SplitJunction::setParameter(int index, float value) noexcept {
  if (index < 0 || index >= kNumParams)
    return;
  // The table is a function-local static built before any junction exists
  // (the constructor reads it), so this is a plain read on any thread.
  const BlockParamSpec& spec = parameterSpecs()[static_cast<size_t>(index)];
  values[static_cast<size_t>(index)].store(spec.clamp(value), std::memory_order_relaxed);
}

bool SplitJunction::setParameter(const juce::String& paramId, float value) {
  const int index = findParameter(paramId);
  if (index < 0)
    return false;
  setParameter(index, value);
  return true;
}

float SplitJunction::getParameter(const juce::String& paramId) const {
  return getParameter(findParameter(paramId));
}

juce::ValueTree SplitJunction::getState() const {
  juce::ValueTree state("Split");
  state.setProperty("id", juce::String(id), nullptr);
  state.setProperty("upperRow", upperRow, nullptr);
  state.setProperty("splitCol", splitCol, nullptr);
  state.setProperty("mixCol", mixCol, nullptr);
  state.setProperty("type", splitterTypeName(type), nullptr);
  const BlockParamTable& table = parameterSpecs();
  for (size_t i = 0; i < table.size(); ++i) {
    juce::ValueTree param("Param");
    param.setProperty("id", table[i].id, nullptr);
    param.setProperty("value", static_cast<double>(getParameter(static_cast<int>(i))), nullptr);
    state.appendChild(param, nullptr);
  }
  return state;
}

bool SplitJunction::setState(const juce::ValueTree& state) {
  if (!state.hasType("Split"))
    return false;
  SplitterType loadedType = type;
  if (!splitterTypeFromName(state.getProperty("type", splitterTypeName(SplitterType::Balance))
                                .toString(),
                            loadedType))
    return false;

  type = loadedType;
  id = state.getProperty("id", juce::String(id)).toString().toStdString();
  // A NaN or an infinity cast to int is undefined, and a file can hold
  // either: what is not a finite number is the default.
  const auto finiteInt = [](const juce::var& value, int fallback) {
    if (value.isVoid())
      return fallback;
    const double number = static_cast<double>(value);
    return std::isfinite(number) && std::abs(number) < 1.0e9 ? static_cast<int>(number) : fallback;
  };
  upperRow = finiteInt(state.getProperty("upperRow"), upperRow);
  splitCol = finiteInt(state.getProperty("splitCol"), splitCol);
  mixCol = finiteInt(state.getProperty("mixCol"), -1);

  // Defaults first, so a parameter the writer did not know comes out at its
  // default instead of keeping whatever this junction held before.
  const BlockParamTable& table = parameterSpecs();
  for (size_t i = 0; i < table.size(); ++i)
    setParameter(static_cast<int>(i), table[i].defaultValue);
  for (int i = 0; i < state.getNumChildren(); ++i) {
    const juce::ValueTree param = state.getChild(i);
    if (!param.hasType("Param"))
      continue;
    const int index = findParameter(param.getProperty("id").toString());
    if (index >= 0)
      setParameter(index, static_cast<float>(static_cast<double>(param.getProperty("value"))));
  }
  return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SplitJunction::Ramp::prepare(double rate) noexcept {
  value.reset(rate, kRampSeconds);
  snapNext = true;
}

void SplitJunction::Ramp::setTarget(float target) noexcept {
  if (snapNext) {
    value.setCurrentAndTargetValue(target);
    snapNext = false;
  } else {
    value.setTargetValue(target);
  }
}

void SplitJunction::prepare(double chainSampleRate, int maxBlockSize) {
  sampleRate = chainSampleRate > 0.0 ? chainSampleRate : 48000.0;
  preparedBlockSize = juce::jmax(0, maxBlockSize);
  rampSamples = juce::jmax(1, juce::roundToInt(sampleRate * kRampSeconds));
  for (Ramp* ramp : {&splitGainA, &splitGainB, &stereoAmount, &swapAmount, &mixGainAL, &mixGainAR,
                     &mixGainBL, &mixGainBR})
    ramp->prepare(sampleRate);
  reset();
}

void SplitJunction::resetSplitterState() noexcept {
  for (Ramp* ramp : {&splitGainA, &splitGainB, &stereoAmount, &swapAmount})
    ramp->snapOnNextTarget();
  for (auto& channel : crossover)
    channel.reset();
  snapCrossover = true;
  logHzSamplesLeft = 0;
}

void SplitJunction::reset() {
  resetSplitterState();
  lastSplitType = -1;
  for (Ramp* ramp : {&mixGainAL, &mixGainAR, &mixGainBL, &mixGainBR})
    ramp->snapOnNextTarget();
  lastMixLayout = -1;
  alignA.clear();
  alignB.clear();
  for (auto& meter : splitMeter)
    meter.store(kMeterFloorDb, std::memory_order_relaxed);
  for (auto& meter : mixMeter)
    meter.store(kMeterFloorDb, std::memory_order_relaxed);
}

void SplitJunction::setAlignment(int samplesA, int samplesB) {
  alignA.configure(samplesA);
  alignB.configure(samplesB);
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------

void SplitJunction::storePeakDb(std::atomic<float>& meter, const juce::AudioBuffer<float>& buffer,
                                int channels, int numFrames) noexcept {
  float peak = 0.0f;
  for (int ch = 0; ch < channels; ++ch) {
    const auto range = juce::FloatVectorOperations::findMinAndMax(buffer.getReadPointer(ch),
                                                                  numFrames);
    peak = std::max(peak, std::max(-range.getStart(), range.getEnd()));
  }
  meter.store(std::max(kMeterFloorDb, juce::Decibels::gainToDecibels(peak, kMeterFloorDb)),
              std::memory_order_relaxed);
}

float SplitJunction::splitMeterDb(int side) const noexcept {
  return splitMeter[static_cast<size_t>(juce::jlimit(0, 1, side))].load(std::memory_order_relaxed);
}

float SplitJunction::mixMeterDb(int side) const noexcept {
  return mixMeter[static_cast<size_t>(juce::jlimit(0, 1, side))].load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Splitter
// ---------------------------------------------------------------------------

void SplitJunction::updateCrossoverCoeffs(double hz) noexcept {
  // Kept below Nyquist so tan() stays finite at any chain rate.
  const double fc = juce::jlimit(1.0, 0.49 * sampleRate, hz);
  const double g = std::tan(juce::MathConstants<double>::pi * fc / sampleRate);
  // Butterworth: damping 1/Q = sqrt(2).
  coeffs.a1 = 1.0 / (1.0 + g * (g + kSqrt2));
  coeffs.a2 = g * coeffs.a1;
  coeffs.a3 = g * coeffs.a2;
}

void SplitJunction::split(juce::AudioBuffer<float>& upper, juce::AudioBuffer<float>& lower,
                          int numFrames, const SplitFormat& format) noexcept {
  if (upper.getNumChannels() < 1 || lower.getNumChannels() < 1)
    return;
  numFrames = juce::jmin(numFrames, upper.getNumSamples(), lower.getNumSamples());
  if (numFrames <= 0)
    return;
  jassert(preparedBlockSize == 0 || numFrames <= preparedBlockSize);

  const int upperCh = liveChannels(format.upperChannels, upper);
  const int lowerCh = liveChannels(format.lowerChannels, lower);
  const bool stereoSource = upperCh == 2;

  // A change of type swaps one law for an unrelated one. The caller makes
  // it under the chain-edit fade, so start the new law clean instead of
  // ramping between the two.
  const SplitterType typeNow = type;
  if (static_cast<int>(typeNow) != lastSplitType) {
    resetSplitterState();
    lastSplitType = static_cast<int>(typeNow);
  }
  const bool isCrossover = typeNow == SplitterType::Crossover;

  float targetA = 1.0f, targetB = 1.0f;
  if (typeNow == SplitterType::Balance) {
    // The same shape as the pan balance: both at unity in the centre, the
    // other path fading out toward either end. Negative is toward A.
    float keepA = 1.0f, keepB = 1.0f;
    balanceLaw(getParameter(kSplitBalance), keepA, keepB);
    targetA = keepA;
    targetB = keepB;
  } else if (typeNow == SplitterType::AB) {
    targetA = levelGain(getParameter(kSplitLevelA));
    targetB = levelGain(getParameter(kSplitLevelB));
  }
  splitGainA.setTarget(targetA);
  splitGainB.setTarget(targetB);

  // Stereo split needs two channels that are one image: a mono source has
  // one signal, and dual mono channels must never be handed to the other
  // path as if they were its sides.
  const bool stereoSplit =
      stereoSource && !format.upperDualMono && getParameter(kSplitStereo) >= 0.5f;
  if (!stereoSource)
    stereoAmount.snapOnNextTarget();  // not advanced below: keep it settled
  stereoAmount.setTarget(stereoSplit ? 1.0f : 0.0f);

  if (!isCrossover)
    swapAmount.snapOnNextTarget();  // unused by the other types: keep it settled
  swapAmount.setTarget(getParameter(kSplitSwap) >= 0.5f ? 1.0f : 0.0f);

  if (isCrossover) {
    const double target = std::log(static_cast<double>(
        juce::jlimit(kCrossoverMinHz, kCrossoverMaxHz, getParameter(kSplitCrossoverHz))));
    if (snapCrossover) {
      logHz = logHzTarget = target;
      logHzSamplesLeft = 0;
      snapCrossover = false;
      updateCrossoverCoeffs(std::exp(logHz));
    } else if (target != logHzTarget) {
      logHzTarget = target;
      logHzStep = (logHzTarget - logHz) / static_cast<double>(rampSamples);
      logHzSamplesLeft = rampSamples;
    }
  }

  float* up0 = upper.getWritePointer(0);
  float* up1 = stereoSource ? upper.getWritePointer(1) : nullptr;
  float* lo0 = lower.getWritePointer(0);
  float* lo1 = lowerCh == 2 ? lower.getWritePointer(1) : nullptr;

  const bool settled = !splitGainA.isSmoothing() && !splitGainB.isSmoothing() &&
                       !stereoAmount.isSmoothing();
  if (!isCrossover && settled) {
    // Nothing is moving and there is no filter: copies and at most one gain
    // per path. B first, since it reads the channels A is about to change.
    const bool stereoOn = stereoSource && stereoAmount.current() >= 1.0f;
    const float gainA = splitGainA.current();
    const float gainB = splitGainB.current();

    if (gainB == 0.0f) {
      for (int ch = 0; ch < lowerCh; ++ch)
        juce::FloatVectorOperations::clear(lower.getWritePointer(ch), numFrames);
    } else {
      // The channels B is made of: the right one twice in a stereo split,
      // the only one twice from a mono source.
      const float* fromLeft = !stereoSource ? up0 : (stereoOn ? up1 : up0);
      const float* fromRight = !stereoSource ? up0 : up1;
      if (lo1 != nullptr) {
        juce::FloatVectorOperations::copy(lo0, fromLeft, numFrames);
        juce::FloatVectorOperations::copy(lo1, fromRight, numFrames);
      } else if (fromLeft == fromRight) {
        juce::FloatVectorOperations::copy(lo0, fromLeft, numFrames);
      } else {
        for (int i = 0; i < numFrames; ++i)
          lo0[i] = 0.5f * (fromLeft[i] + fromRight[i]);
      }
      if (gainB != 1.0f)
        for (int ch = 0; ch < lowerCh; ++ch)
          juce::FloatVectorOperations::multiply(lower.getWritePointer(ch), gainB, numFrames);
    }

    if (stereoOn)
      juce::FloatVectorOperations::copy(up1, up0, numFrames);
    if (gainA == 0.0f) {
      for (int ch = 0; ch < upperCh; ++ch)
        juce::FloatVectorOperations::clear(upper.getWritePointer(ch), numFrames);
    } else if (gainA != 1.0f) {
      for (int ch = 0; ch < upperCh; ++ch)
        juce::FloatVectorOperations::multiply(upper.getWritePointer(ch), gainA, numFrames);
    }
  } else {
    for (int i = 0; i < numFrames; ++i) {
      const float in0 = up0[i];
      const float in1 = stereoSource ? up1[i] : in0;
      // What each path takes from each input channel, before the stereo
      // routing: the whole signal, or its band.
      float a0 = in0, a1 = in1, b0 = in0, b1 = in1;

      if (isCrossover) {
        if (logHzSamplesLeft > 0) {
          logHz += logHzStep;
          if (--logHzSamplesLeft == 0)
            logHz = logHzTarget;
          updateCrossoverCoeffs(std::exp(logHz));
        }
        const float swap = swapAmount.next();
        const float in[2] = {in0, in1};
        float toA[2] = {0.0f, 0.0f}, toB[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < upperCh; ++ch) {
          Crossover& xo = crossover[static_cast<size_t>(ch)];
          const CrossoverCoeffs& c = coeffs;
          // One TPT state-variable step: returns the low output and leaves
          // the high one in `high`.
          const auto step = [&c](Section& s, double x, double& high) noexcept {
            const double v3 = x - s.ic2;
            const double v1 = c.a1 * s.ic1 + c.a2 * v3;
            const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
            s.ic1 = 2.0 * v1 - s.ic1;
            s.ic2 = 2.0 * v2 - s.ic2;
            high = x - kSqrt2 * v1 - v2;
            return v2;
          };
          double firstHigh = 0.0, unused = 0.0, highBand = 0.0;
          const double firstLow = step(xo.first, static_cast<double>(in[ch]), firstHigh);
          const double lowBand = step(xo.low, firstLow, unused);
          step(xo.high, firstHigh, highBand);
          toA[ch] = static_cast<float>(blend(lowBand, highBand, swap));
          toB[ch] = static_cast<float>(blend(highBand, lowBand, swap));
        }
        a0 = toA[0];
        b0 = toB[0];
        a1 = stereoSource ? toA[1] : a0;
        b1 = stereoSource ? toB[1] : b0;
      }

      // Stereo split: A's right channel becomes its left one, B's left
      // channel becomes its right one.
      const float stereo = stereoSource ? stereoAmount.next() : 0.0f;
      const float aRight = blend(a1, a0, stereo);
      const float bLeft = blend(b0, b1, stereo);

      const float gainA = splitGainA.next();
      const float gainB = splitGainB.next();
      up0[i] = a0 * gainA;
      if (stereoSource)
        up1[i] = aRight * gainA;
      if (lo1 != nullptr) {
        lo0[i] = bLeft * gainB;
        lo1[i] = b1 * gainB;
      } else {
        // The mono fold. From a mono source both terms are the same value,
        // so this is that value exactly.
        lo0[i] = 0.5f * (bLeft + b1) * gainB;
      }
    }
  }

  storePeakDb(splitMeter[0], upper, upperCh, numFrames);
  storePeakDb(splitMeter[1], lower, lowerCh, numFrames);
}

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------

void SplitJunction::mix(juce::AudioBuffer<float>& upper, juce::AudioBuffer<float>& lower,
                        int numFrames, const SplitFormat& format) noexcept {
  if (upper.getNumChannels() < 1 || lower.getNumChannels() < 1)
    return;
  numFrames = juce::jmin(numFrames, upper.getNumSamples(), lower.getNumSamples());
  if (numFrames <= 0)
    return;
  jassert(preparedBlockSize == 0 || numFrames <= preparedBlockSize);

  const int upperCh = liveChannels(format.upperChannels, upper);
  const int lowerCh = liveChannels(format.lowerChannels, lower);

  // The path that arrives early waits for the other one.
  alignA.process(upper.getArrayOfWritePointers(), upperCh, numFrames);
  alignB.process(lower.getArrayOfWritePointers(), lowerCh, numFrames);

  storePeakDb(mixMeter[0], upper, upperCh, numFrames);
  storePeakDb(mixMeter[1], lower, lowerCh, numFrames);

  // Everything the mixer does is one gain per path and side, so one set of
  // four ramps carries the levels, the pans, the polarity, the overall
  // level and the mute together.
  const float overall = getParameter(kMixMute) >= 0.5f ? 0.0f : levelGain(getParameter(kMixLevel));
  const float levelA = levelGain(getParameter(kMixLevelA)) * overall;
  float levelB = levelGain(getParameter(kMixLevelB)) * overall;
  if (getParameter(kMixInvertB) >= 0.5f)
    levelB = -levelB;

  const bool stereoResult = upperCh == 2;
  float panAL = 1.0f, panAR = 1.0f, panBL = 1.0f, panBR = 1.0f;
  if (stereoResult && !format.upperDualMono)
    balanceLaw(getParameter(kMixPanA), panAL, panAR);
  if (lowerCh == 2) {
    // Also into a mono result: the pan then sets how much of each side
    // goes into the fold.
    if (!format.lowerDualMono)
      balanceLaw(getParameter(kMixPanB), panBL, panBR);
  } else if (stereoResult && !format.upperDualMono) {
    constantPowerLaw(getParameter(kMixPanB), panBL, panBR);
  }

  const int layout = upperCh + 2 * lowerCh + (format.upperDualMono ? 8 : 0) +
                     (format.lowerDualMono ? 16 : 0);
  if (layout != lastMixLayout) {
    for (Ramp* ramp : {&mixGainAL, &mixGainAR, &mixGainBL, &mixGainBR})
      ramp->snapOnNextTarget();
    lastMixLayout = layout;
  }
  mixGainAL.setTarget(levelA * panAL);
  mixGainAR.setTarget(levelA * panAR);
  mixGainBL.setTarget(levelB * panBL);
  mixGainBR.setTarget(levelB * panBR);

  float* up0 = upper.getWritePointer(0);
  float* up1 = stereoResult ? upper.getWritePointer(1) : nullptr;
  const float* lo0 = lower.getReadPointer(0);
  const float* lo1 = lowerCh == 2 ? lower.getReadPointer(1) : nullptr;

  const bool identity = mixGainAL.settledAt(1.0f) && mixGainAR.settledAt(1.0f) &&
                        mixGainBL.settledAt(1.0f) && mixGainBR.settledAt(1.0f);
  if (identity) {
    // The defaults: a plain sum, bit-exact A + B. (The general loop below
    // gives the same samples at unity gains; this only saves the work.)
    if (stereoResult) {
      juce::FloatVectorOperations::add(up0, lo0, numFrames);
      juce::FloatVectorOperations::add(up1, lo1 != nullptr ? lo1 : lo0, numFrames);
    } else if (lo1 == nullptr) {
      juce::FloatVectorOperations::add(up0, lo0, numFrames);
    } else {
      for (int i = 0; i < numFrames; ++i)
        up0[i] += 0.5f * (lo0[i] + lo1[i]);
    }
    return;
  }

  for (int i = 0; i < numFrames; ++i) {
    // All four advance every sample, used or not, so a later change of
    // layout does not find a stale ramp.
    const float gainAL = mixGainAL.next();
    const float gainAR = mixGainAR.next();
    const float gainBL = mixGainBL.next();
    const float gainBR = mixGainBR.next();
    const float b0 = lo0[i];
    const float b1 = lo1 != nullptr ? lo1[i] : b0;
    if (stereoResult) {
      up0[i] = up0[i] * gainAL + b0 * gainBL;
      up1[i] = up1[i] * gainAR + b1 * gainBR;
    } else if (lo1 != nullptr) {
      up0[i] = up0[i] * gainAL + 0.5f * (b0 * gainBL + b1 * gainBR);
    } else {
      up0[i] = up0[i] * gainAL + b0 * gainBL;
    }
  }
}
