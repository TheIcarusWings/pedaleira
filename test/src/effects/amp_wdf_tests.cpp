// Unit tests for the wave digital filter parts of the amp framework
// (plugin/include/amp/WdfTriodeStage.h, WdfToneStack.h, GainStages.h):
//
//   - each linear network of a gain stage, solved as a WDF tree, against its
//     analogue transfer function computed here from the component values
//     (the plate load with the coupling capacitor into the next grid, the
//     cathode resistor with its bypass capacitor, the gain pot with its bright
//     capacitor), within 0.5 dB from 20 Hz to 10 kHz;
//   - the WDF realisation of a stage against the classic one (the static
//     curve with one-pole filters), on small signals and at drive;
//   - the closed-form tone stack (ToneStack.h) against the same passive
//     network built as a WDF with an R-type adaptor (WdfToneStack.h), at
//     several knob settings, and the passivity of its poles.
//
// Everything runs in plain float / double with no JUCE types, at the rate the
// amp's nonlinear section runs at (four times 48 kHz).
#include "amp/AmpCircuit.h"
#include "amp/AmpDescription.h"
#include "amp/GainStages.h"
#include "amp/ToneStack.h"
#include "amp/WdfToneStack.h"
#include "amp/WdfTriodeStage.h"
#include "effects/amp/BuiltinAmps.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <functional>
#include <iostream>
#include <random>
#include <vector>

namespace {

using Complex = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 192000.0;

// The frequencies every network is checked at: 20 Hz to 10 kHz.
const double kFrequencies[] = {20.0,  30.0,  50.0,   80.0,   120.0,  200.0,  300.0,  500.0,
                               800.0, 1200.0, 2000.0, 3000.0, 5000.0, 8000.0, 10000.0};

double toDb(double linear) { return 20.0 * std::log10(std::max(linear, 1.0e-15)); }

// Gain in dB at `hz` of a sample-by-sample process, from a sine of `level`:
// after 0.4 s for the slow poles to settle, the output is correlated with the
// input over a whole number of cycles (at least 8, and at least 0.4 s), so the
// measurement has no leakage to speak of.
double gainDb(const std::function<float(float)>& process, double hz, double level, double rate = kRate) {
  const size_t settle = static_cast<size_t>(std::round(0.4 * rate));
  const double cycles = std::max(8.0, std::round(0.4 * hz));
  const size_t span = static_cast<size_t>(std::round(cycles * rate / hz));
  Complex out = 0.0, in = 0.0;
  for (size_t i = 0; i < settle + span; ++i) {
    const double x = level * std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate);
    const double y = process(static_cast<float>(x));
    if (i >= settle) {
      const Complex w = std::polar(1.0, -2.0 * kPi * hz * static_cast<double>(i) / rate);
      out += y * w;
      in += x * w;
    }
  }
  return toDb(std::abs(out) / std::abs(in));
}

// The same measurement for a process that works on blocks of samples.
double gainDbOfBlocks(const std::function<void(const float*, float*, int)>& processBlock, double hz, double level,
                      double rate = kRate) {
  const size_t settle = static_cast<size_t>(std::round(0.4 * rate));
  const double cycles = std::max(8.0, std::round(0.4 * hz));
  const size_t span = static_cast<size_t>(std::round(cycles * rate / hz));
  const size_t total = (settle + span + 63) / 64 * 64;
  std::vector<float> in(total), out(total);
  for (size_t i = 0; i < total; ++i)
    in[i] = static_cast<float>(level * std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate));
  for (size_t i = 0; i < total; i += 64)
    processBlock(in.data() + i, out.data() + i, 64);
  Complex o = 0.0, x = 0.0;
  for (size_t i = settle; i < settle + span; ++i) {
    const Complex w = std::polar(1.0, -2.0 * kPi * hz * static_cast<double>(i) / rate);
    o += static_cast<double>(out[i]) * w;
    x += static_cast<double>(in[i]) * w;
  }
  return toDb(std::abs(o) / std::abs(x));
}

struct Worst {
  double db{0.0};
  double at{0.0};
  void note(double difference, double hz) {
    if (std::abs(difference) > db) {
      db = std::abs(difference);
      at = hz;
    }
  }
};

// ---------------------------------------------------------------------------
// The component values of the networks under test (V2 of the shipped amp).
// ---------------------------------------------------------------------------

constexpr double kMu = 100.0, kRp = 62.5e3, kRa = 100.0e3;

amp::GainStageSpec plateStage() {
  amp::GainStageSpec spec;
  spec.triode.plateLoad = static_cast<float>(kRa);
  spec.plateFarads = 345.0e-12f;
  spec.hasNext = true;
  spec.coupling = {4.7e-9f, 470.0e3f, 25.0e3f};
  spec.bypass = amp::CathodeBypass::Ideal;
  return spec;
}

}  // namespace

// ---------------------------------------------------------------------------
// Each network against its analogue transfer function
// ---------------------------------------------------------------------------

TEST(AmpWdfNetworks, PlateLoadAndCouplingCapacitorIntoTheNextGrid) {
  // Ra || Cp || (Cc + Rs + Rg) loaded by the tube's rp; the next grid is the
  // voltage across Rg: H = mu Zp / (rp + Zp) * Rg / (Zc + Rs + Rg). The cathode
  // is shorted, so the only dynamics are the plate side.
  const amp::GainStageSpec spec = plateStage();
  const double cp = spec.plateFarads, cc = spec.coupling.capacitance;
  const double rs = spec.coupling.seriesResistance, rg = spec.coupling.gridLeak;
  amp::WdfGainStage stage;
  stage.prepare(kRate, spec);
  Worst worst;
  for (const double hz : kFrequencies) {
    stage.reset();
    const double measured = gainDb([&](float x) { return stage.process(x); }, hz, 1.0e-3);
    const Complex s(0.0, 2.0 * kPi * hz);
    const Complex branch = 1.0 / (s * cc) + rs + rg;
    const Complex zp = 1.0 / (1.0 / kRa + s * cp + 1.0 / branch);
    const double expected = toDb(std::abs(kMu * zp / (kRp + zp) * (rg / branch)));
    worst.note(measured - expected, hz);
  }
  std::cout << "[  WDF     ] plate + coupling: worst " << worst.db << " dB from the analogue network, at " << worst.at
            << " Hz" << std::endl;
  EXPECT_LT(worst.db, 0.5);
}

TEST(AmpWdfNetworks, CathodeResistorWithItsBypassCapacitor) {
  // A cathode impedance Zk = Rk / (1 + s Rk Ck) degenerates the stage:
  // H = mu Ra / (Ra + rp + (mu + 1) Zk), the textbook common-cathode gain.
  const double rk = 1500.0, ck = 0.68e-6;
  for (const auto mode : {amp::CathodeBypass::Capacitor, amp::CathodeBypass::None, amp::CathodeBypass::Ideal}) {
    amp::GainStageSpec spec;
    spec.triode.plateLoad = static_cast<float>(kRa);
    spec.triode.cathodeResistor = static_cast<float>(rk);
    spec.plateFarads = 0.0f;
    spec.hasNext = false;
    spec.bypass = mode;
    spec.bypassFarads = static_cast<float>(ck);
    amp::WdfGainStage stage;
    stage.prepare(kRate, spec);
    Worst worst;
    for (const double hz : kFrequencies) {
      stage.reset();
      const double measured = gainDb([&](float x) { return stage.process(x); }, hz, 1.0e-3);
      const Complex s(0.0, 2.0 * kPi * hz);
      const Complex zk = mode == amp::CathodeBypass::Capacitor ? rk / (1.0 + s * rk * ck)
                         : mode == amp::CathodeBypass::None     ? Complex(rk)
                                                                : Complex(0.0);
      const double expected = toDb(std::abs(kMu * kRa / (kRa + kRp + (kMu + 1.0) * zk)));
      worst.note(measured - expected, hz);
    }
    const char* name = mode == amp::CathodeBypass::Capacitor ? "bypassed" : mode == amp::CathodeBypass::None ? "unbypassed" : "ideal";
    std::cout << "[  WDF     ] cathode " << name << ": worst " << worst.db << " dB from the analogue network, at "
              << worst.at << " Hz" << std::endl;
    EXPECT_LT(worst.db, 0.5) << name;
  }
}

TEST(AmpWdfNetworks, GainPotWithTheBrightCapacitor) {
  // The pot as a divider from the source resistance, the upper section with the
  // bright capacitor across it, the lower section in parallel with the coupling
  // branch into the next grid. H = Zb / (Rsrc + Zt + Zb) * Rg / (Zc + Rs + Rg).
  amp::GainPotParams pot;
  const amp::CouplingNetwork coupling{8.2e-9f, 0.0f, 470.0e3f};
  for (const float position : {0.1f, 0.35f, 0.6f, 0.9f}) {
    for (const bool bright : {false, true}) {
      amp::WdfPotTree<float> tree;
      tree.setParts(pot, coupling);
      tree.prepare(static_cast<float>(kRate));
      tree.setPot(position, bright);
      const double below = pot.floorResistance + pot.potResistance * std::pow(static_cast<double>(position), pot.taperExponent);
      const double top = pot.potResistance + pot.floorResistance - below;
      Worst worst;
      for (const double hz : kFrequencies) {
        tree.reset();
        const double measured = gainDb([&](float x) { return tree.process(x); }, hz, 0.1);
        const Complex s(0.0, 2.0 * kPi * hz);
        const Complex zTop = bright ? 1.0 / (1.0 / top + s * static_cast<double>(pot.brightCapacitance)) : Complex(top);
        const Complex branch = 1.0 / (s * static_cast<double>(coupling.capacitance)) + static_cast<double>(coupling.seriesResistance) + static_cast<double>(coupling.gridLeak);
        const Complex zBottom = 1.0 / (1.0 / below + 1.0 / branch);
        const Complex h = zBottom / (static_cast<double>(pot.sourceResistance) + zTop + zBottom) * (static_cast<double>(coupling.gridLeak) / branch);
        worst.note(measured - toDb(std::abs(h)), hz);
      }
      std::cout << "[  WDF     ] gain pot at " << position << (bright ? " with bright cap" : "") << ": worst "
                << worst.db << " dB, at " << worst.at << " Hz" << std::endl;
      EXPECT_LT(worst.db, 0.5) << "position " << position << " bright " << bright;
    }
  }
}

TEST(AmpWdfNetworks, TheTubeEquationIsSolvedToTheEndByNewtonsMethod) {
  // The cathode stage is an implicit solve. Two steps per sample, as shipped,
  // against forty: the difference stays far under a millivolt on a plate swing
  // of tens of volts, driven into both knees of the curve.
  amp::GainStageSpec spec;
  spec.triode.plateLoad = static_cast<float>(kRa);
  spec.plateFarads = 295.0e-12f;
  spec.hasNext = true;
  spec.coupling = {8.2e-9f, 0.0f, 470.0e3f};
  spec.bypass = amp::CathodeBypass::Capacitor;
  spec.bypassFarads = 0.68e-6f;
  amp::WdfGainStage shipped, reference;
  shipped.prepare(kRate, spec);
  reference.prepare(kRate, spec);
  shipped.setNewtonSteps(2);
  reference.setNewtonSteps(40);
  double worst = 0.0, peak = 0.0;
  for (int i = 0; i < 96000; ++i) {
    const double t = i / kRate;
    const float x = static_cast<float>(2.0 * std::sin(2.0 * kPi * 440.0 * t) + 3.0 * std::sin(2.0 * kPi * 3000.0 * t));
    const float a = shipped.process(x), b = reference.process(x);
    worst = std::max(worst, static_cast<double>(std::abs(a - b)));
    peak = std::max(peak, static_cast<double>(std::abs(b)));
  }
  std::cout << "[  WDF     ] Newton 2 steps vs 40: worst " << worst << " V on a " << peak << " V peak" << std::endl;
  EXPECT_GT(peak, 20.0);
  EXPECT_LT(worst, 1.0e-3);
}

TEST(AmpWdfNetworks, CathodeFeedbackReducesDistortionAtLowFrequencies) {
  // An unbypassed cathode resistor is negative feedback through the tube
  // equation: the same drive distorts less than with the cathode shorted, which
  // the classic stage (a gain shelf ahead of the curve) cannot show.
  auto thd = [](amp::CathodeBypass bypass) {
    amp::GainStageSpec spec;
    spec.triode.plateLoad = static_cast<float>(kRa);
    spec.plateFarads = 0.0f;
    spec.hasNext = false;
    spec.bypass = bypass;
    amp::WdfGainStage stage;
    stage.prepare(kRate, spec);
    const size_t n = static_cast<size_t>(kRate * 0.5);
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i)
      y[i] = stage.process(static_cast<float>(1.0 * std::sin(2.0 * kPi * 400.0 * static_cast<double>(i) / kRate)));
    // Amplitude of the fundamental and of everything else, over the second half.
    Complex f = 0.0;
    double power = 0.0;
    const size_t from = n / 2;
    for (size_t i = from; i < n; ++i) {
      f += static_cast<double>(y[i]) * std::polar(1.0, -2.0 * kPi * 400.0 * static_cast<double>(i) / kRate);
      power += static_cast<double>(y[i]) * y[i];
    }
    const double count = static_cast<double>(n - from);
    const double fundamental = std::norm(f) * 2.0 / (count * count);
    const double all = power / count;
    return std::sqrt(std::max(0.0, all - fundamental) / fundamental);
  };
  const double shorted = thd(amp::CathodeBypass::Ideal);
  const double degenerated = thd(amp::CathodeBypass::None);
  std::cout << "[  WDF     ] 1 V at 400 Hz: THD " << 100.0 * shorted << " % with the cathode shorted, "
            << 100.0 * degenerated << " % with it unbypassed" << std::endl;
  EXPECT_GT(shorted, 0.02);
  EXPECT_LT(degenerated, 0.7 * shorted);
}

// ---------------------------------------------------------------------------
// WDF stage against the classic stage
// ---------------------------------------------------------------------------
// The two are not meant to be identical: the classic stage drives the next grid
// from an ideal voltage (the WDF plate network is loaded by it), shapes the
// cathode as a first-order shelf ahead of the curve (its transition sits at
// the wrong frequency by the stage's feedback ratio), and approximates the
// bright cap with a one-pole low-pass at the oversampled rate. The tolerances
// below are those differences, stated per network; the WDF figures are the
// accurate ones (previous tests).

namespace {

double compareStages(const amp::GainStageSpec& spec, double level, double lowHz, double highHz,
                     const std::function<void(amp::ClassicGainStage&, amp::WdfGainStage&)>& setup = {},
                     const char* label = "") {
  amp::ClassicGainStage classic;
  amp::WdfGainStage wdf;
  classic.prepare(kRate, spec);
  wdf.prepare(kRate, spec);
  if (setup)
    setup(classic, wdf);
  Worst worst;
  for (const double hz : kFrequencies) {
    if (hz < lowHz || hz > highHz)
      continue;
    classic.reset();
    wdf.reset();
    const double a = gainDb([&](float x) { return classic.process(x); }, hz, level);
    const double b = gainDb([&](float x) { return wdf.process(x); }, hz, level);
    worst.note(b - a, hz);
  }
  std::cout << "[  WDF     ] " << label << ": classic and WDF differ by at most " << worst.db << " dB between "
            << lowHz << " Hz and " << highHz << " Hz (at " << worst.at << " Hz)" << std::endl;
  return worst.db;
}

}  // namespace

TEST(AmpWdfVersusClassic, PlateLoadAndCouplingAgreeWithinOneDecibel) {
  // The loading of the plate by the next grid network: Ra || (Rs + Rg) in place
  // of Ra, 0.64 dB for these values.
  EXPECT_LT(compareStages(plateStage(), 1.0e-3, 20.0, 10000.0, {}, "plate + coupling"), 1.0);
}

TEST(AmpWdfVersusClassic, CathodeShelfAgreesWithinTwoDecibels) {
  amp::GainStageSpec spec;
  spec.plateFarads = 0.0f;
  spec.hasNext = false;
  spec.bypass = amp::CathodeBypass::Capacitor;
  spec.bypassFarads = 0.68e-6f;
  // The two transition at different frequencies by the feedback ratio (1.9):
  // at most 1.7 dB, far from the corner they coincide.
  EXPECT_LT(compareStages(spec, 1.0e-3, 20.0, 10000.0, {}, "cathode shelf"), 2.0);
  spec.bypass = amp::CathodeBypass::None;
  EXPECT_LT(compareStages(spec, 1.0e-3, 20.0, 10000.0, {}, "unbypassed cathode"), 0.3);
}

TEST(AmpWdfVersusClassic, GainPotAgreesWithinHalfADecibelAcrossTheMidBand) {
  amp::GainStageSpec spec;
  spec.plateFarads = 295.0e-12f;
  spec.hasNext = true;
  spec.coupling = {8.2e-9f, 0.0f, 470.0e3f};
  spec.hasPot = true;
  for (const float position : {0.2f, 0.5f, 0.9f}) {
    const auto setup = [position](amp::ClassicGainStage& c, amp::WdfGainStage& w) {
      c.setPot(position, false);
      w.setPot(position, false);
    };
    EXPECT_LT(compareStages(spec, 1.0e-3, 100.0, 8000.0, setup, ("gain pot at " + std::to_string(position)).c_str()), 0.5);
  }
  // With the bright cap the classic low-pass form lifts the highs by a few
  // tenths of a dB less than the network does at the top of the range.
  for (const float position : {0.2f, 0.5f, 0.9f}) {
    const auto setup = [position](amp::ClassicGainStage& c, amp::WdfGainStage& w) {
      c.setPot(position, true);
      w.setPot(position, true);
    };
    EXPECT_LT(compareStages(spec, 1.0e-3, 100.0, 8000.0, setup, ("gain pot with bright cap at " + std::to_string(position)).c_str()), 2.0);
  }
}

TEST(AmpWdfVersusClassic, TheWholePreampAgreesWithinThreeDecibelsOnSmallSignals) {
  // The four stages of the shipped high gain amp, with the shared input network
  // and tone stack, at three gain settings, both realisations built from the
  // same description. The differences add up the three effects above.
  const std::string text = ampfx::builtinAmpText("high-gain.json");
  ASSERT_FALSE(text.empty());
  amp::AmpDescription classicDescription;
  std::string error;
  ASSERT_TRUE(amp::parseAmpDescription(text, classicDescription, error)) << error;
  amp::AmpDescription wdfDescription = classicDescription;
  classicDescription.model = amp::StageModel::Classic;
  wdfDescription.model = amp::StageModel::Wdf;

  amp::PanelValues panel;
  panel.master = 0.5f;
  for (int k = 0; k + 1 < classicDescription.numStages; ++k) {
    amp::CouplingNetwork net = classicDescription.stages[k].spec.coupling;
    net.capacitance = classicDescription.voicings[1].couplingFarads[k];
    panel.couplingCornerHz[k] = net.cornerHz();
  }
  amp::AmpCircuit classic, wdf;
  classic.prepare(kRate, classicDescription);
  wdf.prepare(kRate, wdfDescription);

  for (const float gain : {0.0f, 0.4f, 0.8f}) {
    panel.gain = gain;
    const amp::ControlFrame frame = amp::makeControlFrame(classicDescription, panel, kRate);
    classic.apply(frame, classicDescription);
    wdf.apply(frame, wdfDescription);
    auto response = [&](amp::AmpCircuit& circuit, double hz) {
      circuit.reset();
      return gainDbOfBlocks([&](const float* in, float* out, int n) { circuit.preamp(in, out, n); }, hz, 5.0e-4);
    };
    Worst worst;
    for (const double hz : {100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0})
      worst.note(response(wdf, hz) - response(classic, hz), hz);
    std::cout << "[  WDF     ] preamp at gain " << gain << ": classic and WDF differ by at most " << worst.db
              << " dB from 100 Hz to 6.4 kHz (at " << worst.at << " Hz)" << std::endl;
    EXPECT_LT(worst.db, 3.0) << "gain " << gain;
  }
}

TEST(AmpWdfVersusClassic, BothSaturateTheSameWay) {
  // At drive the stage is the same curve: the distortion of a 1 kHz sine rises
  // with level in both, to within a few points of each other.
  amp::GainStageSpec spec;
  spec.triode.plateLoad = static_cast<float>(kRa);
  spec.plateFarads = 0.0f;
  spec.hasNext = false;
  spec.bypass = amp::CathodeBypass::Ideal;
  auto thd = [&](auto& stage, double amplitude) {
    const size_t n = static_cast<size_t>(kRate * 0.5);
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i)
      y[i] = stage.process(static_cast<float>(amplitude * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kRate)));
    Complex f = 0.0;
    double power = 0.0;
    const size_t from = n / 2;
    for (size_t i = from; i < n; ++i) {
      f += static_cast<double>(y[i]) * std::polar(1.0, -2.0 * kPi * 1000.0 * static_cast<double>(i) / kRate);
      power += static_cast<double>(y[i]) * y[i];
    }
    const double count = static_cast<double>(n - from);
    const double fundamental = std::norm(f) * 2.0 / (count * count);
    return std::sqrt(std::max(0.0, power / count - fundamental) / fundamental);
  };
  double last = 0.0;
  for (const double amplitude : {0.05, 0.2, 0.5, 1.5, 4.0}) {
    amp::ClassicGainStage classic;
    amp::WdfGainStage wdf;
    classic.prepare(kRate, spec);
    wdf.prepare(kRate, spec);
    const double a = thd(classic, amplitude), b = thd(wdf, amplitude);
    std::cout << "[  WDF     ] " << amplitude << " V at 1 kHz: THD classic " << 100.0 * a << " %, WDF " << 100.0 * b << " %"
              << std::endl;
    EXPECT_NEAR(b, a, 0.04 + 0.15 * a) << amplitude;
    EXPECT_GE(b, last - 1.0e-3);
    last = b;
  }
}

TEST(AmpWdfVersusClassic, TheWdfStageIsStableAndResets) {
  amp::GainStageSpec spec;
  spec.plateFarads = 345.0e-12f;
  spec.hasNext = true;
  spec.coupling = {4.7e-9f, 470.0e3f, 25.0e3f};
  spec.bypass = amp::CathodeBypass::Capacitor;
  spec.bypassFarads = 0.68e-6f;
  amp::WdfGainStage stage;
  stage.prepare(kRate, spec);
  std::mt19937 random(5);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
  float worst = 0.0f;
  for (int i = 0; i < 200000; ++i) {
    // Hot noise, then a long silence: nothing blows up and the state decays.
    const float x = i < 100000 ? 40.0f * noise(random) : 0.0f;
    const float y = stage.process(x);
    ASSERT_TRUE(std::isfinite(y)) << i;
    worst = std::max(worst, std::abs(y));
  }
  EXPECT_LT(worst, 400.0f);
  stage.reset();
  amp::WdfGainStage fresh;
  fresh.prepare(kRate, spec);
  for (int i = 0; i < 1000; ++i)
    ASSERT_EQ(stage.process(0.3f * std::sin(0.01f * static_cast<float>(i))),
              fresh.process(0.3f * std::sin(0.01f * static_cast<float>(i))));
}

// ---------------------------------------------------------------------------
// The tone stack against an independent reference
// ---------------------------------------------------------------------------

namespace {

double wdfStackDb(amp::WdfToneStack<double>& stack, double hz) {
  stack.reset();
  return gainDb([&](float x) { return static_cast<float>(stack.process(static_cast<double>(x))); }, hz, 0.1);
}

}  // namespace

TEST(AmpToneStackReference, ClosedFormMatchesThePassiveNetworkBuiltAsAWdf) {
  // The same network two ways: the third-order polynomial of ToneStack.h and
  // the netlist solved by wave digital filtering (an R-type adaptor with the
  // scattering matrix from the nodal analysis). They share no algebra.
  const amp::ToneStackParts parts{220.0e3, 1.0e6, 22.0e3, 39.0e3, 400.0e-12, 22.0e-9, 22.0e-9};
  const amp::ToneStackParts fender{250.0e3, 1.0e6, 25.0e3, 56.0e3, 250.0e-12, 20.0e-9, 20.0e-9};
  const double settings[][3] = {{0.5, 0.5, 0.5}, {1.0, 1.0, 1.0}, {0.8, 0.2, 0.4}, {0.3, 0.7, 0.9},
                                {0.1, 0.5, 1.0}, {0.9, 0.9, 0.1}, {0.6, 0.0, 0.3}};
  for (const amp::ToneStackParts& set : {parts, fender}) {
    for (const auto& knobs : settings) {
      amp::WdfToneStack<double> stack;
      stack.setParts(set);
      stack.setControls(knobs[0], knobs[1], knobs[2]);
      stack.prepare(kRate);
      const amp::ToneStackDesign design = amp::designToneStack(set, knobs[0], knobs[1], knobs[2], kRate);
      Worst worst;
      for (const double hz : kFrequencies)
        worst.note(wdfStackDb(stack, hz) - toDb(design.analogMagnitude(hz)), hz);
      std::cout << "[  STACK   ] R1 " << set.r1 << " t " << knobs[0] << " m " << knobs[1] << " l " << knobs[2]
                << ": closed form and WDF differ by at most " << worst.db << " dB (at " << worst.at << " Hz)" << std::endl;
      EXPECT_LT(worst.db, 0.5) << "treble " << knobs[0] << " middle " << knobs[1] << " bass " << knobs[2];
    }
  }
}

TEST(AmpToneStackReference, TheStackIsAPassiveRcNetwork) {
  // The poles of any passive RC network are real and negative. The cubic is
  // checked through its discriminant, over random parts and knobs. (An earlier version of
  // ToneStack.h had wrong third-order terms, and about two settings in a
  // hundred gave complex poles.)
  std::mt19937 random(11);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  auto logUniform = [&](double lo, double hi) { return std::exp(std::log(lo) + unit(random) * (std::log(hi) - std::log(lo))); };
  int checked = 0;
  for (int trial = 0; trial < 4000; ++trial) {
    const amp::ToneStackParts parts{logUniform(50e3, 500e3), logUniform(100e3, 2e6), logUniform(5e3, 100e3),
                                    logUniform(5e3, 200e3),  logUniform(50e-12, 1e-9), logUniform(5e-9, 100e-9),
                                    logUniform(5e-9, 100e-9)};
    const double t = unit(random), m = unit(random), l = unit(random);
    const amp::ToneStackDesign d = amp::designToneStack(parts, t, m, l, 192000.0);
    // Denominator 1 + a1 s + a2 s^2 + a3 s^3: all coefficients positive, and
    // the cubic's discriminant non-negative (three real roots).
    const double a1 = d.analogDen[1], a2 = d.analogDen[2], a3 = d.analogDen[3];
    EXPECT_GT(a1, 0.0);
    EXPECT_GT(a2, 0.0);
    EXPECT_GT(a3, 0.0);
    const double disc = 18.0 * a3 * a2 * a1 - 4.0 * a2 * a2 * a2 + a2 * a2 * a1 * a1 - 4.0 * a3 * a1 * a1 * a1 - 27.0 * a3 * a3;
    const double scale = std::abs(18.0 * a3 * a2 * a1) + std::abs(4.0 * a2 * a2 * a2) + a2 * a2 * a1 * a1 + 4.0 * a3 * a1 * a1 * a1 + 27.0 * a3 * a3;
    EXPECT_GT(disc, -1.0e-9 * scale) << "complex poles at t " << t << " m " << m << " l " << l;
    // (Only the poles of a passive RC network are real: its zeros can be a complex pair.)
    EXPECT_GT(d.analogNum[1], 0.0);
    ++checked;
  }
  EXPECT_EQ(checked, 4000);
}
