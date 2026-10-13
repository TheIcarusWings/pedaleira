#pragma once
// The audio engine shared by the amp kinds (amp.highgain, amp.custom): a
// ParameterisedBlockProcessor that runs an amp::AmpCircuit built from an
// amp::AmpDescription.
//
//   in -> [4x: AmpCircuit preamp (input network, gain stages, tone stack,
//         master) -> power amp with feedback and supply sag] -> DC blocker ->
//         level
//
// The nonlinear parts, and the tone stack and feedback that sit between them,
// run at 4x through the same half-band oversampler the drive kinds use.
//
// The panel (gain, bass, middle, treble, presence, depth, master, bright,
// voicing) is the same ten parameters on every amp kind; the description says
// which parts they move (AmpDescription::panel). A kind differs only in where
// its description comes from: amp.highgain is the shipped high-gain.json,
// amp.custom takes one from a file (block.loadDescription).
//
// Threading: the description is swapped by applyDescription, which the caller
// invokes with the audio excluded (the chain lock held, or before the block
// is installed); the audio thread only reads it.
#include <array>
#include <cstring>
#include <memory>

#include "amp/AmpCircuit.h"
#include "amp/AmpDescription.h"
#include "effects/drive/DriveCommon.h"

namespace ampfx {

using namespace drivefx;

class AmpProcessorBase : public ParameterisedBlockProcessor {
public:
  enum Param { kGain = 0, kBass, kMiddle, kTreble, kPresence, kDepth, kMaster, kLevelDb, kBright, kVoicing };
  enum Reading { kSag = 0 };

  static BlockParamTable makeParameterTable(const juce::StringArray& voicings, int defaultVoicing) {
    BlockParamTable t;
    t.push_back(BlockParamSpec::floatParam("gain", "Gain", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("bass", "Bass", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("middle", "Middle", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("treble", "Treble", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("presence", "Presence", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("depth", "Depth", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("master", "Master", "%", 0.0f, 100.0f, 50.0f));
    t.push_back(BlockParamSpec::floatParam("levelDb", "Level", "dB", -48.0f, 18.0f, 0.0f));
    t.push_back(BlockParamSpec::boolParam("bright", "Bright", false));
    t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing", voicings, defaultVoicing));
    return t;
  }

  static const BlockReadingTable& readingSpecs() {
    // Headroom lost to supply sag, as a fraction of the deepest voicing's.
    static const BlockReadingTable table = {{"sag", "Sag", "", 0.0f, 1.0f}};
    return table;
  }

  // The half-band filters of the 4x oversampler (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // Sag recovery, the grid leak and the low poles of the stack and the
  // coupling capacitors, plus the output DC blocker.
  double tailSeconds() const noexcept override { return 0.5; }

  const amp::AmpDescription& description() const noexcept { return *desc; }

  /** Swaps the description. Rebuilds the circuits when the block is already
      prepared (which allocates for the wave digital model): the caller must
      keep the audio thread out. */
  void applyDescription(std::shared_ptr<const amp::AmpDescription> next) {
    desc = std::move(next);
    if (prepared)
      buildLanes();
  }

protected:
  AmpProcessorBase(const char* kindId, const BlockParamTable& table, std::shared_ptr<const amp::AmpDescription> initial)
      : ParameterisedBlockProcessor(kindId, table, readingSpecs()), desc(std::move(initial)) {}

  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    osRate = sampleRate * kOversampleFactor;
    oversampler.prepare(spec.maxBlockSize);
    level.prepare(sampleRate, 0.02);
    prepared = true;
    buildLanes();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes)
      lane.reset();
    ticksLeft = 0;
    lane1Mirrors = lanes[0].circuit.copyable();
    level.snapOnNextTarget();
    snapControls = true;
    setReading(kSag, 0.0f);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const amp::AmpDescription& d = *desc;
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);

    readTargets(d);
    level.setTarget(fx::dbToGain(param(kLevelDb)) * d.calibration.outputTrim);

    // A stereo block that carries the same audio on both channels (what the
    // processor hands a block after it folds its input to mono) runs one
    // circuit and copies the result. The second lane keeps no state of its
    // own while it mirrors the first; the moment the channels differ it takes
    // a copy of the first lane's state, which is exactly what it would hold.
    // The wave digital model cannot be copied, so it runs both lanes.
    bool mirrored = false;
    if (numLanes == 2) {
      const bool same = std::memcmp(buffer.getReadPointer(0), buffer.getReadPointer(1),
                                    sizeof(float) * static_cast<size_t>(numSamples)) == 0;
      mirrored = lane1Mirrors && same;
      if (lane1Mirrors && !same) {
        lanes[1].copyStateFrom(lanes[0]);
        lane1Mirrors = false;
      }
    }
    const int runLanes = mirrored ? 1 : numLanes;

    oversampler.run(buffer, [&](float* const* io, int frames, int) {
      // Work in chunks that end on the control grid, so a control update
      // lands on the same sample whatever the block size.
      int done = 0;
      while (done < frames) {
        if (ticksLeft == 0) {
          stepControls(d);
          ticksLeft = kControlGridOs;
        }
        const int n = std::min(ticksLeft, frames - done);
        float v0[kChunk], v1[kChunk];
        lanes[0].circuit.preamp(io[0] + done, v0, n);
        if (runLanes > 1)
          lanes[1].circuit.preamp(io[1] + done, v1, n);
        powerStage(lanes[0], v0, io[0] + done, runLanes > 1 ? &lanes[1] : nullptr, v1, io[1] + done, n);
        if (mirrored)
          std::memcpy(io[1] + done, io[0] + done, sizeof(float) * static_cast<size_t>(n));
        done += n;
        ticksLeft -= n;
      }
    });

    for (int i = 0; i < numSamples; ++i) {
      const float g = level.next();
      for (int ch = 0; ch < runLanes; ++ch) {
        float* data = buffer.getWritePointer(ch);
        data[i] = lanes[ch].dcBlock.process(data[i]) * g;
      }
      if (mirrored)
        buffer.getWritePointer(1)[i] = buffer.getReadPointer(0)[i];
    }

    float droop = 0.0f;
    for (int ch = 0; ch < runLanes; ++ch)
      droop = juce::jmax(droop, lanes[ch].circuit.droop());
    setReading(kSag, droop / deepestSag(d));
  }

private:
  static constexpr int kChunk = kControlGridOs;

  // The controls that move smoothly, in one array so they can be smoothed
  // and compared as a block. The coupling entries are corner frequencies in
  // Hz derived from the voicing's capacitors.
  enum Control {
    cGain = 0,
    cBass,
    cMiddle,
    cTreble,
    cPresence,
    cDepth,
    cMaster,
    cCoupling0,
    cSagDepth = cCoupling0 + amp::kMaxLinks,
    cSagRelease,
    cBright,
    kControls
  };

  struct Lane {
    amp::AmpCircuit circuit;
    DcBlocker dcBlock;

    void reset() noexcept {
      circuit.reset();
      dcBlock.reset();
    }
    void copyStateFrom(const Lane& other) noexcept {
      circuit.copyStateFrom(other.circuit);
      dcBlock = other.dcBlock;
    }
  };

  void buildLanes() {
    const amp::AmpDescription& d = *desc;
    tickSeconds = static_cast<double>(kControlGridOs) / osRate;
    smoothCoeff = static_cast<float>(1.0 - std::exp(-tickSeconds / d.calibration.controlSmoothSeconds));
    for (Lane& lane : lanes) {
      lane.circuit.prepare(osRate, d);
      lane.dcBlock.setCutoff(d.power.transformerLowCornerHz, sampleRate);
    }
    resetEffect();
  }

  // The power stage, whose feedback ties each sample to the one before it, is
  // the one serial part. Two lanes run it together so that their chains
  // overlap.
  static void powerStage(Lane& a, const float* va, float* oa, Lane* b, const float* vb, float* ob, int n) noexcept {
    if (b == nullptr) {
      for (int i = 0; i < n; ++i)
        oa[i] = a.circuit.powerSample(va[i]);
      return;
    }
    for (int i = 0; i < n; ++i) {
      oa[i] = a.circuit.powerSample(va[i]);
      ob[i] = b->circuit.powerSample(vb[i]);
    }
  }

  float knob(const amp::AmpDescription& d, amp::PanelId id, Param p) const noexcept {
    return d.control(id).present ? param(p) * 0.01f : 0.5f;
  }

  void readTargets(const amp::AmpDescription& d) noexcept {
    const int index = juce::jlimit(0, d.numVoicings - 1, static_cast<int>(std::lround(param(kVoicing))));
    const amp::VoicingDescription& voicing = d.voicings[index];
    target[cGain] = knob(d, amp::PanelId::Gain, kGain);
    target[cBass] = knob(d, amp::PanelId::Bass, kBass);
    target[cMiddle] = knob(d, amp::PanelId::Middle, kMiddle);
    target[cTreble] = knob(d, amp::PanelId::Treble, kTreble);
    target[cPresence] = knob(d, amp::PanelId::Presence, kPresence);
    target[cDepth] = knob(d, amp::PanelId::Depth, kDepth);
    target[cMaster] = knob(d, amp::PanelId::Master, kMaster);
    for (int k = 0; k + 1 < d.numStages; ++k) {
      amp::CouplingNetwork net = d.stages[k].spec.coupling;
      if (voicing.couplingFarads[k] > 0.0f)
        net.capacitance = voicing.couplingFarads[k];
      target[cCoupling0 + k] = net.cornerHz();
    }
    const amp::SagParams sag = amp::sagParamsFor(voicing.supply);
    target[cSagDepth] = sag.depth;
    target[cSagRelease] = sag.releaseSeconds;
    const bool bright = d.control(amp::PanelId::Bright).present && param(kBright) >= 0.5f;
    target[cBright] = bright ? 1.0f : 0.0f;
  }

  // Once per control grid: glide the controls and push what depends on them
  // into the components. Nothing is rebuilt when nothing moved.
  void stepControls(const amp::AmpDescription& d) noexcept {
    if (snapControls) {
      current = target;
      snapControls = false;
      applied[0] = -1.0f;  // force the first application
    } else {
      for (int i = 0; i < kControls; ++i)
        current[i] += smoothCoeff * (target[i] - current[i]);
    }
    if (std::memcmp(applied.data(), current.data(), sizeof(float) * kControls) == 0)
      return;
    applied = current;

    amp::PanelValues values;
    values.gain = current[cGain];
    values.bass = current[cBass];
    values.middle = current[cMiddle];
    values.treble = current[cTreble];
    values.presence = current[cPresence];
    values.depth = current[cDepth];
    values.master = current[cMaster];
    values.bright = current[cBright] > 0.5f;
    for (int k = 0; k < amp::kMaxLinks; ++k)
      values.couplingCornerHz[k] = current[cCoupling0 + k];
    values.sagDepth = current[cSagDepth];
    values.sagReleaseSeconds = current[cSagRelease];
    const amp::ControlFrame frame = amp::makeControlFrame(d, values, osRate);
    for (Lane& lane : lanes)
      lane.circuit.apply(frame, d);
  }

  static float deepestSag(const amp::AmpDescription& d) noexcept {
    float deepest = 0.0f;
    for (int v = 0; v < d.numVoicings; ++v)
      deepest = juce::jmax(deepest, amp::sagParamsFor(d.voicings[v].supply).depth);
    return juce::jmax(deepest, 1.0e-6f);
  }

  std::shared_ptr<const amp::AmpDescription> desc;
  bool prepared{false};
  Lane lanes[kMaxLanes];
  Oversampler4x oversampler;
  int ticksLeft{0};
  bool lane1Mirrors{true};
  fx::SmoothedParam level;
  std::array<float, kControls> target{}, current{}, applied{};
  bool snapControls{true};
  float smoothCoeff{0.01f};
  double sampleRate{48000.0}, osRate{192000.0}, tickSeconds{0.0003};
};

}  // namespace ampfx
