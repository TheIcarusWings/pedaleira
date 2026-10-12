#pragma once
#include "ParameterisedBlockProcessor.h"

/**
 * utility.gain: level, balance and polarity. The reference registry kind: as
 * small as a kind gets, and exact enough to test the framework against
 * (unity is a bit-exact pass-through, polarity and a hard pan are exact).
 */
class GainBlockProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "utility.gain";

  // Table order; the indices the audio path reads.
  enum Param { kGainDb = 0, kPan, kPhaseInvert };
  static const BlockParamTable& parameterSpecs();

  GainBlockProcessor();

  // A gain is exact at any rate, so the round trip to the base rate would
  // only add its filters to the signal.
  bool runsAtBaseRate() const noexcept override { return false; }

protected:
  void prepareEffect(const EffectSpec& spec) override;
  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext& ctx) override;

private:
  // Per-channel gain targets from the three parameters. Pan is a balance
  // control: the far channel is attenuated linearly and the near one stays
  // at unity, so centre leaves both channels untouched.
  void targetGains(bool stereo, float& left, float& right) const noexcept;

  juce::LinearSmoothedValue<float> gainLeft{1.0f};
  juce::LinearSmoothedValue<float> gainRight{1.0f};
  // Pan only acts on two channels, so the gains to start from depend on a
  // channel count that prepare cannot know (a lane is mono or stereo by
  // routing, and can change with it). The first block after a prepare, and
  // any block whose channel count differs from the last, snaps to its
  // targets instead of ramping from values meant for the other layout.
  bool snapOnNextBlock{true};
  bool lastStereo{false};
};
