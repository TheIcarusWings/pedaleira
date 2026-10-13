// amp.highgain: a modern high gain tube amp model, composed from the
// components in plugin/include/amp (namespace amp).
//
//   in -> [4x: input coupling high-pass -> grid stopper low-pass -> cathode
//         shelf -> triode 1 -> gain pot with bright cap -> (coupling high-pass
//         -> triode) x 3 -> passive tone stack -> master -> power amp with
//         presence / depth feedback and supply sag] -> DC blocker -> level
//
// Every number that sets the sound is in AmpTuning below, so a later tuning
// against measurements changes that struct and nothing else. The nonlinear
// parts, and the tone stack and feedback that sit between them, run at 4x
// through the same half-band oversampler the drive kinds use.
#include <array>
#include <cstring>

#include "amp/CouplingFilters.h"
#include "amp/PowerAmp.h"
#include "amp/SagModel.h"
#include "amp/ToneStack.h"
#include "amp/TriodeStage.h"
#include "effects/drive/DriveCommon.h"

namespace {

using namespace drivefx;

// ---------------------------------------------------------------------------
// Tuning: all constants in one place, in physical terms where there is a
// physical meaning (ohms, farads, henries, volts, amps), with the corner
// frequencies and gains derived from them in code. A schematic can be typed
// in here. The few values that have no component behind them are listed in
// the last group, "fitted".
// ---------------------------------------------------------------------------
struct AmpTuning {
  // ---- Input ---------------------------------------------------------------
  // Input coupling capacitor into the first grid leak (25 Hz), and the grid
  // stopper with the Miller capacitance of the first stage (14.6 kHz).
  amp::CouplingNetwork inputNetwork = {6.8e-9f, 0.0f, 1.0e6f};
  float gridStopperOhms = 68.0e3f;
  float millerFarads = 160.0e-12f;
  float guitarSourceOhms = 10.0e3f;  // pickup impedance, for the grid charge path

  // ---- Triode stages (V1 is the input stage, V2 to V4 follow the gain pot) --
  // Each is a 12AX7 with a 100k plate load, a 1.5k cathode resistor and 1 mA
  // of bias current (1.5 V of bias).
  amp::TriodeCircuit triode[4] = {};
  float cathodeBypassFarads = 0.68e-6f;  // V1: its shelf corner is 1 / (2 pi Rk Ck)
  // Capacitance on each plate (wiring plus the next grid's Miller capacitance):
  // with the plate impedance it sets the stage's high-frequency roll-off.
  float plateLoadFarads[4] = {295.0e-12f, 345.0e-12f, 413.0e-12f, 459.0e-12f};

  // ---- Gain pot and bright cap (amp/CouplingFilters.h) -----------------------
  // 1M audio taper pot after V1's 100k plate load, a 22k floor resistor, loaded
  // by V2's 470k grid leak, with a 100 pF bright cap.
  amp::GainPotParams gainPot = {};

  // ---- Interstage networks feeding V2, V3, V4 -------------------------------
  // The series resistor and grid leak (the divider) are fixed; the coupling
  // capacitor belongs to the voicing. V2 is fed from the gain pot.
  amp::CouplingNetwork interstage[3] = {{0.0f, 0.0f, 470.0e3f},
                                        {0.0f, 470.0e3f, 25.0e3f},
                                        {0.0f, 470.0e3f, 10.0e3f}};

  // ---- Voicings: the interstage capacitors and the power supply ----------------
  // Tight uses small capacitors (corners about 70, 110 and 150 Hz) and a stiff
  // solid-state supply; Classic medium ones and a tube rectifier; Loose large
  // ones (about 22, 30 and 40 Hz) and a soft supply with a big source resistance.
  struct Voicing {
    float couplingFarads[3];
    amp::SagSupply supply;
  };
  Voicing voicing[3] = {
      {{4.7e-9f, 2.7e-9f, 2.0e-9f}, {450.0f, 100.0f, 0.3f, 220.0e-6f, 0.008f}},    // Tight
      {{8.2e-9f, 4.7e-9f, 3.9e-9f}, {450.0f, 250.0f, 0.3f, 220.0e-6f, 0.008f}},    // Classic
      {{15.0e-9f, 10.0e-9f, 8.2e-9f}, {450.0f, 450.0f, 0.3f, 220.0e-6f, 0.008f}},  // Loose
  };

  // ---- Tone stack (amp/ToneStack.h) -------------------------------------------
  // Treble 220k, bass 1M, middle 22k, slope 39k; 400 pF, 22 nF, 22 nF. The bass
  // pot has a log taper (position = knob ^ bassTaper).
  amp::ToneStackParts toneParts = {220.0e3, 1.0e6, 22.0e3, 39.0e3, 400.0e-12, 22.0e-9, 22.0e-9};
  float bassTaper = 2.0f;

  // ---- Master and power amp (amp/PowerAmp.h) -----------------------------------
  float masterTaper = 2.0f;  // the master pot: gain = knob ^ taper
  amp::PowerAmpParams power = {};

  // ---- Fitted: no component value behind these -------------------------------
  // Digital full scale at the first grid, in volts (-18 dBFS is then 0.25 V).
  float inputVolts = 2.0f;
  // Volts at the tone stack output (after the master) that drive the power
  // stage to its full swing: stands for the recovery stage and phase inverter.
  float powerDriveVolts = 40.0f;
  // How much the interstage corners rise with gain (the factor at full gain is
  // 1 + couplingGainRise): the gain-dependent tightening of the low end.
  float couplingGainRise = 1.8f;
  // Scale from the power stage's swing to digital, and the output DC blocker.
  float outputTrim = 0.2f;
  float outputDcBlockHz = 12.0f;
  // How fast the knobs glide, in seconds.
  float controlSmoothSeconds = 0.02f;
  // Not derivable and set in the component structs above: the triode curve
  // shapes and the grid-conduction fit (cutoffGridVolts, contactPotential,
  // gridConductionResistance), the power stage knee and crossover (kneeMix,
  // crossoverRatio, crossoverWidth), depthMaxCut, the transformer low-pass,
  // the sag attack time.
};

const AmpTuning kTuning{};

class HighGainAmpProcessor final : public ParameterisedBlockProcessor {
public:
  static constexpr const char* kKindId = "amp.highgain";

  enum Param {
    kGain = 0,
    kBass,
    kMiddle,
    kTreble,
    kPresence,
    kDepth,
    kMaster,
    kLevelDb,
    kBright,
    kVoicing
  };
  enum Reading { kSag = 0 };

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
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
      t.push_back(BlockParamSpec::choiceParam("voicing", "Voicing", {"Tight", "Classic", "Loose"}, 1));
      return t;
    }();
    return table;
  }

  static const BlockReadingTable& readingSpecs() {
    // Headroom lost to supply sag, as a fraction of the deepest voicing's.
    static const BlockReadingTable table = {{"sag", "Sag", "", 0.0f, 1.0f}};
    return table;
  }

  HighGainAmpProcessor() : ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs()) {}

  // The half-band filters of the 4x oversampler (drive/DriveCommon.h).
  int latencySamples() const noexcept override { return kLatencySamples; }

  // Sag recovery, the grid leak and the low poles of the stack and the
  // coupling capacitors, plus the output DC blocker.
  double tailSeconds() const noexcept override { return 0.5; }

protected:
  void prepareEffect(const EffectSpec& spec) override {
    sampleRate = spec.sampleRate;
    osRate = sampleRate * kOversampleFactor;
    oversampler.prepare(spec.maxBlockSize);
    level.prepare(sampleRate, 0.02);
    tickSeconds = static_cast<double>(kControlGridOs) / osRate;
    smoothCoeff = static_cast<float>(1.0 - std::exp(-tickSeconds / kTuning.controlSmoothSeconds));
    for (Lane& lane : lanes)
      lane.prepare(osRate, sampleRate);
    resetEffect();
  }

  void resetEffect() noexcept override {
    oversampler.reset();
    for (Lane& lane : lanes)
      lane.reset();
    ticksLeft = 0;
    lane1Mirrors = true;
    level.snapOnNextTarget();
    snapControls = true;
    setReading(kSag, 0.0f);
  }

  void processEffect(juce::AudioBuffer<float>& buffer, const BlockProcessContext&) override {
    const int numSamples = buffer.getNumSamples();
    const int numLanes = juce::jmin(buffer.getNumChannels(), kMaxLanes);

    readTargets();
    level.setTarget(fx::dbToGain(param(kLevelDb)) * kTuning.outputTrim);

    // A stereo block that carries the same audio on both channels (what the
    // processor hands a block after it folds its input to mono) runs one
    // circuit and copies the result. The second lane keeps no state of its
    // own while it mirrors the first; the moment the channels differ it takes
    // a copy of the first lane's state, which is exactly what it would hold.
    bool mirrored = false;
    if (numLanes == 2) {
      const bool same = std::memcmp(buffer.getReadPointer(0), buffer.getReadPointer(1),
                                    sizeof(float) * static_cast<size_t>(numSamples)) == 0;
      mirrored = lane1Mirrors && same;
      if (lane1Mirrors && !same) {
        lanes[1] = lanes[0];
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
          stepControls();
          ticksLeft = kControlGridOs;
        }
        const int n = std::min(ticksLeft, frames - done);
        float v0[kChunk], v1[kChunk];
        lanes[0].preamp(io[0] + done, v0, n);
        if (runLanes > 1)
          lanes[1].preamp(io[1] + done, v1, n);
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
      droop = juce::jmax(droop, lanes[ch].sag.droop());
    setReading(kSag, droop / deepestSag());
  }

private:
  static constexpr int kStages = 4;
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
    cCoupling1,
    cCoupling2,
    cSagDepth,
    cSagRelease,
    cBright,
    kControls
  };

  struct Lane {
    amp::CouplingHighpass inputHighpass;
    amp::OnePole gridStopper;
    amp::CathodeShelf cathode;
    amp::TriodeStage triode[kStages];
    amp::OnePole plate[kStages];
    amp::GainPot pot;
    amp::CouplingHighpass coupling[3];
    amp::ToneStackFilter tone;
    amp::PowerAmp power;
    amp::SagModel sag;
    DcBlocker dcBlock;
    float masterGain{0.0f};
    float divider[3]{1.0f, 1.0f, 1.0f};

    void prepare(double osRate, double baseRate) noexcept {
      const AmpTuning& t = kTuning;
      inputHighpass.setCorner(t.inputNetwork.cornerHz(), osRate);
      gridStopper.setCutoff(1.0 / (amp::kTwoPi * t.gridStopperOhms * t.millerFarads), osRate);
      cathode.setup(amp::CathodeShelf::cornerFor(t.triode[0].cathodeResistor, t.cathodeBypassFarads),
                    t.triode[0].unbypassedRatio(), osRate);
      for (int k = 0; k < kStages; ++k) {
        triode[k].prepare(osRate, t.triode[k]);
        plate[k].setCutoff(1.0 / (amp::kTwoPi * t.triode[k].sourceImpedance() * t.plateLoadFarads[k]), osRate);
      }
      // The input stage's grid network is fixed (see AmpTuning).
      triode[0].setCoupling(t.inputNetwork.leakSeconds(),
                            t.inputNetwork.leakSeconds() * chargeRatio(t.guitarSourceOhms + t.gridStopperOhms, t.inputNetwork,
                                                                       t.triode[0]));
      pot.prepare(osRate, t.gainPot);
      for (int k = 0; k < 3; ++k)
        divider[k] = t.interstage[k].divider();
      sag.prepare(osRate, amp::sagParamsFor(t.voicing[1].supply));
      power.prepare(osRate, t.power);
      dcBlock.setCutoff(t.outputDcBlockHz, baseRate);
    }

    // The ratio between the charge path (source + series + conducting grid)
    // and the discharge path (series + grid leak) of a coupling network.
    static float chargeRatio(float sourceOhms, const amp::CouplingNetwork& net, const amp::TriodeCircuit& circuit) noexcept {
      return (sourceOhms + net.seriesResistance + circuit.gridConductionResistance) / net.totalResistance();
    }

    void reset() noexcept {
      inputHighpass.reset();
      gridStopper.reset();
      cathode.reset();
      for (int k = 0; k < kStages; ++k) {
        triode[k].reset();
        plate[k].reset();
      }
      pot.reset();
      for (amp::CouplingHighpass& c : coupling)
        c.reset();
      tone.reset();
      power.reset();
      sag.reset();
      dcBlock.reset();
    }

    // The preamp and tone stack for `n` (at most kChunk) oversampled samples:
    // `io` in, `v` out. Each section is its own loop over the chunk, so the
    // filters and curves of consecutive samples overlap in the processor
    // instead of waiting for one sample to travel the whole circuit.
    void preamp(const float* io, float* v, int n) noexcept {
      for (int i = 0; i < n; ++i) {
        float s = inputHighpass.process(io[i] * kTuning.inputVolts);
        s = cathode.process(gridStopper.lowpass(s));
        s = plate[0].lowpass(triode[0].process(s));
        v[i] = pot.process(s);
      }
      for (int k = 1; k < kStages; ++k)
        for (int i = 0; i < n; ++i)
          v[i] = plate[k].lowpass(triode[k].process(coupling[k - 1].process(v[i]) * divider[k - 1]));
      for (int i = 0; i < n; ++i)
        v[i] = tone.process(v[i]) * masterGain;
    }

    float powerSample(float v) noexcept {
      const float y = power.process(v, sag.headroom());
      sag.update(std::abs(y));
      return y;
    }
  };

  // The power stage, whose feedback ties each sample to the one before it, is
  // the one serial part. Two lanes run it together so that their chains
  // overlap.
  static void powerStage(Lane& a, const float* va, float* oa, Lane* b, const float* vb, float* ob, int n) noexcept {
    if (b == nullptr) {
      for (int i = 0; i < n; ++i)
        oa[i] = a.powerSample(va[i]);
      return;
    }
    for (int i = 0; i < n; ++i) {
      oa[i] = a.powerSample(va[i]);
      ob[i] = b->powerSample(vb[i]);
    }
  }

  void readTargets() noexcept {
    const AmpTuning& t = kTuning;
    const int index = juce::jlimit(0, 2, static_cast<int>(std::lround(param(kVoicing))));
    const AmpTuning::Voicing& voicing = t.voicing[index];
    target[cGain] = param(kGain) * 0.01f;
    target[cBass] = param(kBass) * 0.01f;
    target[cMiddle] = param(kMiddle) * 0.01f;
    target[cTreble] = param(kTreble) * 0.01f;
    target[cPresence] = param(kPresence) * 0.01f;
    target[cDepth] = param(kDepth) * 0.01f;
    target[cMaster] = param(kMaster) * 0.01f;
    for (int k = 0; k < 3; ++k) {
      amp::CouplingNetwork net = t.interstage[k];
      net.capacitance = voicing.couplingFarads[k];
      target[cCoupling0 + k] = net.cornerHz();
    }
    const amp::SagParams sag = amp::sagParamsFor(voicing.supply);
    target[cSagDepth] = sag.depth;
    target[cSagRelease] = sag.releaseSeconds;
    target[cBright] = param(kBright) >= 0.5f ? 1.0f : 0.0f;
  }

  // Once per control grid: glide the controls and push what depends on them
  // into the components. Nothing is rebuilt when nothing moved.
  void stepControls() noexcept {
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

    const AmpTuning& t = kTuning;
    const float gain = current[cGain];
    const float rise = 1.0f + t.couplingGainRise * gain;
    const amp::ToneStackDesign design =
        amp::designToneStack(t.toneParts, current[cTreble], current[cMiddle],
                             std::pow(current[cBass], t.bassTaper), osRate);
    const float master = std::pow(current[cMaster], t.masterTaper) / t.powerDriveVolts;
    // Per stage: the corner after the gain rise, the time the grid charge leaks
    // away in, and the time it takes to build.
    float corner[3], leak[3], charge[3];
    for (int k = 0; k < 3; ++k) {
      corner[k] = current[cCoupling0 + k] * rise;
      leak[k] = 1.0f / (static_cast<float>(amp::kTwoPi) * corner[k]);
      const float sourceOhms = k == 0 ? t.gainPot.sourceResistance : t.triode[k].sourceImpedance();
      charge[k] = leak[k] * Lane::chargeRatio(sourceOhms, t.interstage[k], t.triode[k + 1]);
    }
    for (Lane& lane : lanes) {
      lane.pot.setPosition(gain, current[cBright] > 0.5f);
      for (int k = 0; k < 3; ++k) {
        lane.coupling[k].setCorner(corner[k], osRate);
        lane.triode[k + 1].setCoupling(leak[k], charge[k]);
      }
      lane.tone.setDesign(design);
      lane.power.setPresence(current[cPresence]);
      lane.power.setDepth(current[cDepth]);
      lane.sag.setDepth(current[cSagDepth]);
      lane.sag.setReleaseSeconds(current[cSagRelease]);
      lane.masterGain = master;
    }
  }

  static float deepestSag() noexcept {
    float deepest = 0.0f;
    for (const AmpTuning::Voicing& v : kTuning.voicing)
      deepest = juce::jmax(deepest, amp::sagParamsFor(v.supply).depth);
    return deepest;
  }

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

}  // namespace

PEDALEIRA_EFFECT_KINDS(HighGainAmp) {
  addEffectKind<HighGainAmpProcessor>(registry, "High Gain Amp", BlockCategory::Amp);
}
