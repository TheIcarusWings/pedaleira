// Unit tests for the amp components in plugin/include/amp (namespace amp):
// filters, triode stage, coupling and bright filters, tone stack, power amp
// with feedback, and sag. Each component is exercised on its own, in plain
// float / double arithmetic with no JUCE types, the way another project
// lifting the headers would use it.
#include "amp/CouplingFilters.h"
#include "amp/Filters.h"
#include "amp/PowerAmp.h"
#include "amp/SagModel.h"
#include "amp/ToneStack.h"
#include "amp/TriodeStage.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <functional>
#include <iostream>
#include <vector>

namespace {

constexpr double kPiValue = 3.14159265358979323846;

double toDecibels(double linear) { return 20.0 * std::log10(std::max(linear, 1.0e-15)); }

// Amplitude of the component of `x` at `hz` (plain DFT over whole data).
double amplitudeAt(const std::vector<float>& x, size_t from, double hz, double fs) {
  std::complex<double> acc{0.0, 0.0};
  for (size_t i = from; i < x.size(); ++i) {
    const double phase = -2.0 * kPiValue * hz * static_cast<double>(i) / fs;
    acc += static_cast<double>(x[i]) * std::complex<double>(std::cos(phase), std::sin(phase));
  }
  return 2.0 * std::abs(acc) / static_cast<double>(x.size() - from);
}

// Gain in dB of a sample-by-sample process at `hz`, from a sine of `level`.
double gainDb(const std::function<float(float)>& process, double hz, double fs, float level,
              double seconds = 0.6) {
  const size_t n = static_cast<size_t>(seconds * fs);
  std::vector<float> in(n), out(n);
  for (size_t i = 0; i < n; ++i) {
    in[i] = level * static_cast<float>(std::sin(2.0 * kPiValue * hz * static_cast<double>(i) / fs));
    out[i] = process(in[i]);
  }
  const size_t from = n / 2;
  return toDecibels(amplitudeAt(out, from, hz, fs) / amplitudeAt(in, from, hz, fs));
}

}  // namespace

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

TEST(AmpComponents, OnePoleIsThreeDecibelsDownAtItsCorner) {
  const double fs = 192000.0;
  amp::OnePole low;
  low.setCutoff(1000.0, fs);
  // The one-pole built from exp(-2 pi fc / fs) is -3 dB near fc at this rate.
  EXPECT_NEAR(gainDb([&](float x) { return low.lowpass(x); }, 1000.0, fs, 0.1f), -3.0, 0.4);
  amp::OnePole high;
  high.setCutoff(1000.0, fs);
  EXPECT_NEAR(gainDb([&](float x) { return high.highpass(x); }, 1000.0, fs, 0.1f), -3.0, 0.4);
  EXPECT_LT(gainDb([&](float x) { return high.highpass(x); }, 20.0, fs, 0.1f), -30.0);
}

TEST(AmpComponents, SvfBandOutputPeaksAtTheCentreWithGainQ) {
  const double fs = 192000.0;
  amp::Svf svf;
  svf.set(100.0, 2.0, fs);
  const double atCentre = gainDb([&](float x) { return svf.process(x).band; }, 100.0, fs, 0.1f, 1.5);
  svf.reset();
  const double octaveUp = gainDb([&](float x) { return svf.process(x).band; }, 200.0, fs, 0.1f, 1.5);
  EXPECT_NEAR(atCentre, toDecibels(2.0), 0.3);
  EXPECT_LT(octaveUp, atCentre - 5.0);
}

TEST(AmpComponents, FiltersStayStableAtAbsurdSettings) {
  amp::OnePole p;
  p.setCutoff(1.0e9, 48000.0);
  amp::Svf s;
  s.set(1.0e9, 1.0e-6, 48000.0);
  float worst = 0.0f;
  for (int i = 0; i < 4800; ++i) {
    worst = std::max(worst, std::abs(p.lowpass(i == 0 ? 1.0f : 0.0f)));
    worst = std::max(worst, std::abs(s.process(i == 0 ? 1.0f : 0.0f).band));
  }
  EXPECT_TRUE(std::isfinite(worst));
  EXPECT_LT(worst, 100.0f);
}

// ---------------------------------------------------------------------------
// Triode stage
// ---------------------------------------------------------------------------

TEST(AmpTriode, CircuitValuesGiveTheTextbookNumbers) {
  const amp::TriodeCircuit circuit;  // 12AX7, 100k plate load, 1.5k cathode, 1 mA
  EXPECT_NEAR(circuit.amplification(), 61.5f, 0.1f);          // mu Ra / (Ra + rp)
  EXPECT_NEAR(circuit.biasVolts(), 1.5f, 1.0e-6f);            // Ik Rk
  EXPECT_NEAR(circuit.unbypassedRatio(), 0.52f, 0.01f);       // gain with Rk unbypassed
  EXPECT_NEAR(circuit.sourceImpedance(), 38.5e3f, 0.1e3f);    // Ra || rp
  EXPECT_NEAR(circuit.posLimit(), 1.5f, 1.0e-6f);
  EXPECT_NEAR(circuit.negLimit(), 2.0f, 1.0e-6f);
  EXPECT_NEAR(circuit.gridOnset(), 1.2f, 1.0e-6f);
}

TEST(AmpTriode, CurveHasUnitSlopeAtTheOriginAndIsBounded) {
  const float eps = 1.0e-3f;
  EXPECT_NEAR(amp::triodeCurve(eps, 1.0f, 1.6f) / eps, 1.0f, 1.0e-3f);
  EXPECT_NEAR(amp::triodeCurve(-eps, 1.0f, 1.6f) / -eps, 1.0f, 1.0e-3f);
  EXPECT_LE(amp::triodeCurve(1.0e6f, 1.0f, 1.6f), 1.0f);
  EXPECT_GE(amp::triodeCurve(-1.0e6f, 1.0f, 1.6f), -1.6f);
  EXPECT_TRUE(std::isfinite(amp::triodeCurve(1.0e30f, 1.0f, 1.6f)));
  EXPECT_TRUE(std::isfinite(amp::triodeCurve(-1.0e30f, 1.0f, 1.6f)));
}

TEST(AmpTriode, GridConductionIsSharperThanCutoff) {
  // At the same distance from the origin the positive side has gone further
  // into its limit than the negative side.
  const float x = 1.5f;
  const float pos = amp::triodeCurve(x, 1.0f, 1.6f) / 1.0f;
  const float neg = -amp::triodeCurve(-x, 1.0f, 1.6f) / 1.6f;
  EXPECT_GT(pos, neg);
}

namespace {

amp::TriodeStage makeStage(double fs) {
  amp::TriodeStage stage;
  stage.prepare(fs, amp::TriodeCircuit{});
  stage.setCoupling(0.004, 0.0015);
  return stage;
}

}  // namespace

TEST(AmpTriode, SmallSignalGainIsTheAmplification) {
  const double fs = 192000.0;
  amp::TriodeStage stage = makeStage(fs);
  EXPECT_NEAR(gainDb([&](float x) { return stage.process(x); }, 1000.0, fs, 0.005f, 0.2),
              toDecibels(amp::TriodeCircuit{}.amplification()), 0.3);
}

TEST(AmpTriode, ABigSineMakesEvenHarmonicsAndASmallOneDoesNot) {
  const double fs = 192000.0;
  const auto secondHarmonicDb = [&](float level) {
    amp::TriodeStage stage = makeStage(fs);
    std::vector<float> out(static_cast<size_t>(fs));
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = stage.process(level * static_cast<float>(std::sin(2.0 * kPiValue * 1000.0 * static_cast<double>(i) / fs)));
    return toDecibels(amplitudeAt(out, out.size() / 2, 2000.0, fs) / amplitudeAt(out, out.size() / 2, 1000.0, fs));
  };
  EXPECT_LT(secondHarmonicDb(0.01f), -50.0);
  EXPECT_GT(secondHarmonicDb(3.0f), -30.0);
}

TEST(AmpTriode, BiasShiftBuildsUnderDriveAndLeaksAway) {
  const double fs = 192000.0;
  amp::TriodeStage stage = makeStage(fs);  // leak time 4 ms
  // A quiet signal does not move it at all.
  for (int i = 0; i < 20000; ++i)
    stage.process(0.1f * static_cast<float>(std::sin(2.0 * kPiValue * 500.0 * i / fs)));
  EXPECT_EQ(stage.biasShift(), 0.0f);
  // A loud one charges it, and it stays within its ceiling.
  for (int i = 0; i < 20000; ++i)
    stage.process(5.0f * static_cast<float>(std::sin(2.0 * kPiValue * 500.0 * i / fs)));
  const float charged = stage.biasShift();
  EXPECT_GT(charged, 0.5f);
  EXPECT_LE(charged, amp::TriodeCircuit{}.maxBiasShiftVolts);
  // It leaks away with the grid-leak time constant: after 5 tau it is a small
  // fraction of what it was.
  for (int i = 0; i < static_cast<int>(0.02 * fs); ++i)
    stage.process(0.0f);
  EXPECT_LT(stage.biasShift(), 0.02f * charged);
}

TEST(AmpTriode, TheShiftChangesTheAsymmetryWithDrive) {
  // The DC content of the output of a big sine (before any coupling) depends
  // on the level because the bias shift tracks it.
  const double fs = 192000.0;
  const auto dcOf = [&](float level) {
    amp::TriodeStage stage = makeStage(fs);
    double sum = 0.0;
    const int n = static_cast<int>(fs);
    for (int i = 0; i < n; ++i) {
      const float y = stage.process(level * static_cast<float>(std::sin(2.0 * kPiValue * 500.0 * i / fs)));
      if (i >= n / 2)
        sum += y;
    }
    return sum / (n / 2);
  };
  EXPECT_NE(std::round(dcOf(1.5f) * 100.0), std::round(dcOf(8.0f) * 100.0));
  EXPECT_NE(dcOf(8.0f), 0.0);
}

TEST(AmpTriode, ASmallSignalOnALargeLimitStaysClean) {
  // A cold stage: 3.37 V of bias (the positive limit) and 0.13 V to cutoff. A
  // 6 mV sine sits far below both knees, so the stage is linear: the output is
  // the sine times the gain and nothing else. The antiderivative's
  // sqrt(1 + t^2) - 1 once rounded to 0 for t^2 under the float epsilon,
  // which turned a signal like this into steps (noise 8 dB under the sine).
  const double fs = 192000.0;
  amp::TriodeCircuit circuit;
  circuit.biasCurrent = 3.37f / circuit.cathodeResistor;
  amp::TriodeStage stage;
  stage.prepare(fs, circuit);
  const double hz = 1377.0;
  const size_t n = static_cast<size_t>(fs);
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = stage.process(0.006f * static_cast<float>(std::sin(2.0 * kPiValue * hz * static_cast<double>(i) / fs)));
  // What is left after taking out the fundamental, against the fundamental.
  const size_t from = n / 2;
  const double fundamental = amplitudeAt(out, from, hz, fs);
  std::complex<double> acc{0.0, 0.0};
  for (size_t i = from; i < n; ++i)
    acc += static_cast<double>(out[i]) *
           std::complex<double>(std::cos(2.0 * kPiValue * hz * static_cast<double>(i) / fs),
                                -std::sin(2.0 * kPiValue * hz * static_cast<double>(i) / fs));
  acc *= 2.0 / static_cast<double>(n - from);
  double residual = 0.0;
  for (size_t i = from; i < n; ++i) {
    const double phase = 2.0 * kPiValue * hz * static_cast<double>(i) / fs;
    const double sine = acc.real() * std::cos(phase) - acc.imag() * std::sin(phase);
    residual += std::pow(static_cast<double>(out[i]) - sine, 2.0);
  }
  residual = std::sqrt(2.0 * residual / static_cast<double>(n - from));
  EXPECT_NEAR(fundamental, 0.006 * circuit.amplification(), 0.006 * circuit.amplification() * 0.01);
  EXPECT_LT(toDecibels(residual / fundamental), -60.0);
}

TEST(AmpTriode, ResetClearsTheState) {
  amp::TriodeStage stage = makeStage(192000.0);
  for (int i = 0; i < 5000; ++i)
    stage.process(6.0f);
  EXPECT_GT(stage.biasShift(), 0.0f);
  stage.reset();
  EXPECT_EQ(stage.biasShift(), 0.0f);
}

// ---------------------------------------------------------------------------
// Coupling and bright filters
// ---------------------------------------------------------------------------

TEST(AmpCoupling, NetworkCornerAndDividerFollowTheComponents) {
  const amp::CouplingNetwork net{4.7e-9f, 470.0e3f, 51.0e3f};
  EXPECT_NEAR(net.cornerHz(), 1.0 / (2.0 * kPiValue * 4.7e-9 * 521.0e3), 0.5);
  EXPECT_NEAR(net.divider(), 51.0 / 521.0, 1.0e-4);
  EXPECT_NEAR(net.leakSeconds(), 4.7e-9 * 521.0e3, 1.0e-7);
}

TEST(AmpCoupling, HighpassCornerAndSlope) {
  const double fs = 192000.0;
  amp::CouplingHighpass hp;
  hp.setCorner(80.0, fs);
  EXPECT_NEAR(gainDb([&](float x) { return hp.process(x); }, 80.0, fs, 0.1f, 1.5), -3.0, 0.5);
  EXPECT_NEAR(gainDb([&](float x) { return hp.process(x); }, 20.0, fs, 0.1f, 1.5), -12.3, 0.6);
  EXPECT_NEAR(gainDb([&](float x) { return hp.process(x); }, 2000.0, fs, 0.1f, 0.5), 0.0, 0.1);
}

TEST(AmpCoupling, BleedCapacitorLiftsTheDividerToTheFullSwing) {
  // 470k into a 10k grid leak with 220 pF across the 470k: the divider in the
  // lows, the analog first-order shelf above, the full swing far up.
  const double fs = 192000.0;
  amp::CouplingNetwork net{0.0f, 470.0e3f, 10.0e3f, 220.0e-12f};
  ASSERT_TRUE(net.hasBleed());
  const double parallel = 470.0e3 * 10.0e3 / 480.0e3;
  EXPECT_NEAR(net.bleedCornerHz(), 1.0 / (2.0 * kPiValue * parallel * 220.0e-12), 1.0);
  amp::CouplingBleed bleed;
  bleed.setup(net, fs);
  auto analog = [&](double hz) {
    const std::complex<double> s(0.0, 2.0 * kPiValue * hz);
    const double d = net.divider(), wp = 2.0 * kPiValue * net.bleedCornerHz();
    return toDecibels(std::abs((d * wp + s) / (wp + s)));
  };
  EXPECT_NEAR(gainDb([&](float x) { return bleed.process(x); }, 50.0, fs, 0.1f, 1.0), toDecibels(net.divider()), 0.1);
  for (double hz : {1000.0, 4000.0, 10000.0}) {
    bleed.reset();
    EXPECT_NEAR(gainDb([&](float x) { return bleed.process(x); }, hz, fs, 0.1f, 0.5), analog(hz), 0.2) << hz;
  }
  // No capacitor, or no series resistor to bridge: no bleed.
  EXPECT_FALSE((amp::CouplingNetwork{0.0f, 470.0e3f, 10.0e3f}.hasBleed()));
  EXPECT_FALSE((amp::CouplingNetwork{0.0f, 0.0f, 10.0e3f, 220.0e-12f}.hasBleed()));
}

TEST(AmpCoupling, CathodeShelfDropsTheLowsByTheRatio) {
  const double fs = 192000.0;
  const amp::TriodeCircuit circuit;
  amp::CathodeShelf shelf;
  const double corner = amp::CathodeShelf::cornerFor(1.5e3, 0.68e-6);
  EXPECT_NEAR(corner, 156.0, 1.0);
  shelf.setup(corner, circuit.unbypassedRatio(), fs);
  EXPECT_NEAR(gainDb([&](float x) { return shelf.process(x); }, 10.0, fs, 0.1f, 2.0),
              toDecibels(circuit.unbypassedRatio()), 0.5);
  shelf.reset();
  EXPECT_NEAR(gainDb([&](float x) { return shelf.process(x); }, 4000.0, fs, 0.1f, 0.5), 0.0, 0.3);
}

TEST(AmpCoupling, GainPotFollowsTheDividerAndTaper) {
  amp::GainPot pot;
  pot.prepare(192000.0, {});
  pot.setPosition(1.0f, false);
  EXPECT_GT(pot.gain(), 0.6f);
  EXPECT_LT(pot.gain(), 1.0f);
  pot.setPosition(0.0f, false);
  // 22k floor loaded by 470k against 100k + 1M: about -34.5 dB.
  EXPECT_NEAR(toDecibels(pot.gain()), -34.5, 0.5);
  // Audio taper: the half-rotation resistance below the wiper is about 10
  // percent of the pot.
  pot.setPosition(0.5f, false);
  EXPECT_NEAR(toDecibels(pot.gain()), -19.5, 1.5);
  float last = 0.0f;
  for (int i = 0; i <= 10; ++i) {
    pot.setPosition(static_cast<float>(i) / 10.0f, false);
    EXPECT_GE(pot.gain(), last);
    last = pot.gain();
  }
}

TEST(AmpCoupling, WiperCapacitanceDarkensTheMiddleOfTheRotationOnly) {
  const double fs = 192000.0;
  amp::GainPotParams params;
  params.wiperCapacitance = 200.0e-12f;
  // The loss at 8 kHz against 500 Hz with the wiper capacitance, relative to
  // the same pot without it.
  const auto lossDb = [&](float position) {
    double out[2];
    for (int withCap = 0; withCap < 2; ++withCap) {
      amp::GainPotParams p = params;
      if (withCap == 0)
        p.wiperCapacitance = 0.0f;
      amp::GainPot pot;
      pot.prepare(fs, p);
      pot.setPosition(position, false);
      const double high = gainDb([&](float x) { return pot.process(x); }, 8000.0, fs, 0.1f, 0.5);
      pot.reset();
      const double low = gainDb([&](float x) { return pot.process(x); }, 500.0, fs, 0.1f, 0.5);
      out[withCap] = high - low;
    }
    return out[0] - out[1];
  };
  // Near the bottom the wiper sees the floor resistor; at the top, the
  // source (100k against the 322k below it, about 2 dB at 8 kHz); in between
  // it sees hundreds of kilohms and the 200 pF takes the most.
  const double bottom = lossDb(0.05f), middle = lossDb(0.75f), top = lossDb(1.0f);
  EXPECT_LT(bottom, 0.5);
  EXPECT_NEAR(top, 2.0, 0.5);
  EXPECT_GT(middle, top + 2.0);
  // Without it the pot is exactly what it was.
  amp::GainPot plain, off;
  plain.prepare(fs, {});
  amp::GainPotParams zero;
  zero.wiperCapacitance = 0.0f;
  off.prepare(fs, zero);
  plain.setPosition(0.6f, false);
  off.setPosition(0.6f, false);
  for (int i = 0; i < 64; ++i) {
    const float x = std::sin(0.3f * static_cast<float>(i));
    EXPECT_EQ(plain.process(x), off.process(x));
  }
}

TEST(AmpCoupling, BrightCapLiftsHighsMostWhenTurnedDownAndFadesToNothing) {
  const double fs = 192000.0;
  const auto liftDb = [&](float position) {
    amp::GainPot pot;
    pot.prepare(fs, {});
    pot.setPosition(position, true);
    const double high = gainDb([&](float x) { return pot.process(x); }, 8000.0, fs, 0.1f, 0.5);
    pot.reset();
    pot.setPosition(position, false);
    const double highOff = gainDb([&](float x) { return pot.process(x); }, 8000.0, fs, 0.1f, 0.5);
    return high - highOff;
  };
  // The lift is largest in the low to middle part of the rotation (where the
  // cap's zero and pole straddle the measured frequency) and dies away as the
  // upper section of the pot shrinks to nothing.
  const double low = liftDb(0.1f), mid = liftDb(0.5f), nearTop = liftDb(0.9f), full = liftDb(1.0f);
  EXPECT_GT(low, 8.0);
  EXPECT_GT(mid, 8.0);
  EXPECT_GT(mid, nearTop + 3.0);
  EXPECT_GT(nearTop, full);
  EXPECT_NEAR(full, 0.0, 0.01);
  // The lows are not lifted by the cap.
  amp::GainPot pot;
  pot.prepare(fs, {});
  pot.setPosition(0.1f, true);
  const double lowFreq = gainDb([&](float x) { return pot.process(x); }, 100.0, fs, 0.1f, 1.0);
  EXPECT_NEAR(lowFreq, toDecibels(pot.gain()), 0.5);
}

TEST(AmpCoupling, ToneCutFollowsItsNetworkAndCutsMoreTurnedUp) {
  const double fs = 192000.0;
  amp::ToneCutParams parts;
  parts.sourceResistance = 100.0e3f;
  parts.potResistance = 100.0e3f;
  parts.minResistance = 2.2e3f;
  parts.capacitance = 4.7e-9f;
  // H(s) = (1 + s Rc C) / (1 + s (Rsrc + Rc) C), Rc = Rmin + Rpot (1 - position).
  const auto analogDb = [&](float position, double hz) {
    const double rc = parts.minResistance + parts.potResistance * (1.0 - position);
    const std::complex<double> s(0.0, 2.0 * kPiValue * hz);
    const double c = parts.capacitance;
    return toDecibels(std::abs((1.0 + s * rc * c) / (1.0 + s * (parts.sourceResistance + rc) * c)));
  };
  double lastHigh = 1.0e9;
  for (const float position : {0.0f, 0.5f, 1.0f}) {
    amp::ToneCut cut;
    cut.prepare(fs, parts);
    cut.setPosition(position);
    const double branch = parts.minResistance + parts.potResistance * (1.0f - position);
    EXPECT_NEAR(cut.highRatio(), branch / (parts.sourceResistance + branch), 1.0e-6);
    for (const double hz : {100.0, 1000.0, 3000.0, 10000.0}) {
      cut.reset();
      EXPECT_NEAR(gainDb([&](float x) { return cut.process(x); }, hz, fs, 0.1f), analogDb(position, hz), 0.75)
          << "position " << position << " at " << hz << " Hz";
    }
    // Unity in the lows, less of the highs the further it is turned up.
    cut.reset();
    EXPECT_NEAR(gainDb([&](float x) { return cut.process(x); }, 15.0, fs, 0.1f, 2.0), 0.0, 0.1);
    cut.reset();
    const double high = gainDb([&](float x) { return cut.process(x); }, 8000.0, fs, 0.1f);
    EXPECT_LT(high, lastHigh - 1.0);
    lastHigh = high;
  }
}

// ---------------------------------------------------------------------------
// Tone stack
// ---------------------------------------------------------------------------

TEST(AmpToneStack, DigitalResponseFollowsTheAnalogOne) {
  const amp::ToneStackParts parts;
  const double fs = 192000.0;
  for (const double knob : {0.1, 0.5, 0.9}) {
    const amp::ToneStackDesign d = amp::designToneStack(parts, knob, knob, knob * knob, fs);
    for (const double hz : {50.0, 120.0, 400.0, 1000.0, 3000.0, 6000.0})
      EXPECT_NEAR(toDecibels(d.digitalMagnitude(hz, fs)), toDecibels(d.analogMagnitude(hz)), 0.15)
          << "knob " << knob << " at " << hz << " Hz";
  }
}

TEST(AmpToneStack, ItIsPassiveAndBlocksDc) {
  const amp::ToneStackParts parts;
  for (const double t : {0.0, 0.5, 1.0})
    for (const double m : {0.0, 0.5, 1.0})
      for (const double l : {0.0, 0.25, 1.0}) {
        const amp::ToneStackDesign d = amp::designToneStack(parts, t, m, l, 192000.0);
        for (const double hz : {20.0, 100.0, 500.0, 2000.0, 8000.0, 20000.0})
          EXPECT_LE(d.analogMagnitude(hz), 1.0 + 1.0e-6) << t << " " << m << " " << l << " " << hz;
        EXPECT_LT(d.analogMagnitude(0.01), 0.01);
      }
}

TEST(AmpToneStack, MidDipAtTheMiddleSettings) {
  const amp::ToneStackParts parts;
  const amp::ToneStackDesign d = amp::designToneStack(parts, 0.5, 0.5, 0.25, 48000.0);
  double dipDb = 0.0, dipHz = 0.0;
  for (double hz = 200.0; hz < 1500.0; hz *= 1.02) {
    const double g = toDecibels(d.analogMagnitude(hz));
    if (g < dipDb) {
      dipDb = g;
      dipHz = hz;
    }
  }
  const double lows = toDecibels(d.analogMagnitude(100.0));
  const double highs = toDecibels(d.analogMagnitude(3000.0));
  std::cout << "[  STACK   ] 5/5/5: dip " << dipDb << " dB at " << dipHz << " Hz, 100 Hz " << lows
            << " dB, 3 kHz " << highs << " dB" << std::endl;
  EXPECT_GT(dipHz, 250.0);
  EXPECT_LT(dipHz, 1200.0);
  EXPECT_LT(dipDb, lows - 3.0);
  EXPECT_LT(dipDb, highs - 3.0);
}

TEST(AmpToneStack, EachKnobMovesItsBandTheRightWay) {
  const amp::ToneStackParts parts;
  const double fs = 48000.0;
  const auto at = [&](double t, double m, double l, double hz) {
    return toDecibels(amp::designToneStack(parts, t, m, l, fs).analogMagnitude(hz));
  };
  // Bass: lows. Treble: highs. Middle: mids.
  EXPECT_GT(at(0.5, 0.5, 1.0, 80.0) - at(0.5, 0.5, 0.0, 80.0), 6.0);
  EXPECT_GT(at(1.0, 0.5, 0.25, 4000.0) - at(0.0, 0.5, 0.25, 4000.0), 6.0);
  EXPECT_GT(at(0.5, 1.0, 0.25, 600.0) - at(0.5, 0.0, 0.25, 600.0), 6.0);
}

TEST(AmpToneStack, TheControlsInteract) {
  // Turning the middle down changes the treble response too: not three
  // independent bands.
  const amp::ToneStackParts parts;
  const auto highs = [&](double m) {
    return toDecibels(amp::designToneStack(parts, 0.5, m, 0.25, 48000.0).analogMagnitude(4000.0));
  };
  EXPECT_GT(std::abs(highs(0.0) - highs(1.0)), 0.5);
}

TEST(AmpToneStack, FilterMatchesTheDesignedResponseAndIsStable) {
  const double fs = 192000.0;
  const amp::ToneStackDesign d = amp::designToneStack({}, 0.5, 0.5, 0.25, fs);
  amp::ToneStackFilter filter;
  filter.setDesign(d);
  for (const double hz : {200.0, 1000.0, 3000.0}) {
    filter.reset();
    EXPECT_NEAR(gainDb([&](float x) { return filter.process(x); }, hz, fs, 0.1f, 0.6),
                toDecibels(d.digitalMagnitude(hz, fs)), 0.2)
        << hz;
  }
  for (const double t : {0.0, 1.0})
    for (const double m : {0.0, 1.0})
      for (const double l : {0.0, 1.0}) {
        amp::ToneStackFilter f;
        f.setDesign(amp::designToneStack({}, t, m, l, fs));
        float tail = 0.0f;
        for (int i = 0; i < static_cast<int>(2.0 * fs); ++i) {
          const float y = f.process(i == 0 ? 1.0f : 0.0f);
          if (i > static_cast<int>(1.9 * fs))
            tail = std::max(tail, std::abs(y));
          ASSERT_TRUE(std::isfinite(y));
        }
        EXPECT_LT(tail, 1.0e-4f) << t << " " << m << " " << l;
      }
}

// ---------------------------------------------------------------------------
// Sag
// ---------------------------------------------------------------------------

TEST(AmpSag, DroopsUnderLoadAndRecoversInTensOfMilliseconds) {
  const double fs = 192000.0;
  amp::SagParams params;
  params.attackSeconds = 0.008f;
  params.releaseSeconds = 0.07f;
  params.depth = 0.3f;
  amp::SagModel sag;
  sag.prepare(fs, params);
  EXPECT_FLOAT_EQ(sag.headroom(), 1.0f);
  for (int i = 0; i < static_cast<int>(0.1 * fs); ++i)
    sag.update(1.0f);
  EXPECT_NEAR(sag.headroom(), 0.7f, 0.01f);
  EXPECT_NEAR(sag.droop(), 0.3f, 0.01f);
  // After one release time constant a bit over a third of the droop is left.
  for (int i = 0; i < static_cast<int>(0.07 * fs); ++i)
    sag.update(0.0f);
  EXPECT_NEAR(sag.droop() / 0.3f, std::exp(-1.0f), 0.05f);
  for (int i = 0; i < static_cast<int>(0.5 * fs); ++i)
    sag.update(0.0f);
  EXPECT_GT(sag.headroom(), 0.999f);
}

TEST(AmpSag, DepthChangesTheDroopAndResetClears) {
  amp::SagModel sag;
  sag.prepare(48000.0, {});
  for (int i = 0; i < 20000; ++i)
    sag.update(1.0f);
  sag.setDepth(0.5f);
  EXPECT_NEAR(sag.droop(), 0.5f * sag.load(), 1.0e-6f);
  sag.reset();
  EXPECT_FLOAT_EQ(sag.headroom(), 1.0f - 0.0f);
  EXPECT_EQ(sag.load(), 0.0f);
}

// ---------------------------------------------------------------------------
// Power amp
// ---------------------------------------------------------------------------

namespace {

amp::PowerAmp makePowerAmp(double fs, float presence, float depth) {
  amp::PowerAmp pa;
  pa.prepare(fs, {});
  pa.setPresence(presence);
  pa.setDepth(depth);
  return pa;
}

}  // namespace

TEST(AmpPowerAmp, OutputIsBoundedByTheHeadroom) {
  amp::PowerAmp pa = makePowerAmp(192000.0, 0.5f, 0.5f);
  for (const float headroom : {1.0f, 0.6f}) {
    pa.reset();
    float peak = 0.0f;
    for (int i = 0; i < 40000; ++i)
      peak = std::max(peak, std::abs(pa.process(50.0f * static_cast<float>(std::sin(2.0 * kPiValue * 200.0 * i / 192000.0)), headroom)));
    EXPECT_LE(peak, 1.1f * headroom);
    EXPECT_GT(peak, 0.9f * headroom);
  }
}

TEST(AmpPowerAmp, SaturatesSymmetrically) {
  amp::PowerAmp pa = makePowerAmp(192000.0, 0.5f, 0.5f);
  double positive = 0.0, negative = 0.0;
  for (int i = 0; i < 192000; ++i) {
    const float y = pa.process(5.0f * static_cast<float>(std::sin(2.0 * kPiValue * 200.0 * i / 192000.0)), 1.0f);
    if (i > 96000) {
      positive = std::max(positive, static_cast<double>(y));
      negative = std::min(negative, static_cast<double>(y));
    }
  }
  EXPECT_NEAR(positive, -negative, 0.02);
}

TEST(AmpPowerAmp, ClosedLoopGainFollowsTheFeedbackFormula) {
  // Small but past the crossover notch: gain is about A / (1 + A beta).
  const double fs = 192000.0;
  amp::PowerAmpParams params;
  params.crossoverRatio = 1.0f;  // no notch for this check
  amp::PowerAmp pa;
  pa.prepare(fs, params);
  pa.setPresence(0.0f);
  pa.setDepth(0.0f);
  const double expected = toDecibels(params.openLoopGain / (1.0 + params.openLoopGain * params.feedbackFraction()));
  EXPECT_NEAR(gainDb([&](float x) { return pa.process(x, 1.0f); }, 500.0, fs, 0.01f, 0.5), expected, 0.6);
}

TEST(AmpPowerAmp, ASolidStateStageIsCleanUpToItsRailAndClipsThere) {
  const double fs = 192000.0;
  auto thdAt = [&](bool hardClip, float level, float& peak) {
    amp::PowerAmpParams params;
    params.hardClip = hardClip;
    params.crossoverRatio = 1.0f;
    params.presenceMinResistor = 1.0e7f;  // no presence network: the loop is flat
    amp::PowerAmp pa;
    pa.prepare(fs, params);
    pa.setPresence(0.0f);
    pa.setDepth(0.0f);
    const size_t n = static_cast<size_t>(0.25 * fs);
    std::vector<float> out(n);
    peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      out[i] = pa.process(level * static_cast<float>(std::sin(2.0 * kPiValue * 1000.0 * static_cast<double>(i) / fs)), 1.0f);
      if (i > n / 2)
        peak = std::max(peak, std::abs(out[i]));
    }
    double harmonics = 0.0;
    for (int k = 2; k <= 10; ++k)
      harmonics += std::pow(amplitudeAt(out, n / 2, 1000.0 * k, fs), 2.0);
    return 100.0 * std::sqrt(harmonics) / amplitudeAt(out, n / 2, 1000.0, fs);
  };
  // The closed-loop gain is A / (1 + A beta) = 4: 0.18 in swings to about 0.72.
  float peak = 0.0f;
  const double clean = thdAt(true, 0.18f, peak);
  EXPECT_LT(clean, 0.01) << "linear below the rail";
  EXPECT_NEAR(peak, 0.72f, 0.03f);
  float softPeak = 0.0f;
  EXPECT_GT(thdAt(false, 0.18f, softPeak), 10.0 * clean + 0.1) << "where the tube stage already bends";
  // Driven past the rail it clips flat at the rail.
  const double clipped = thdAt(true, 1.0f, peak);
  EXPECT_GT(clipped, 5.0);
  EXPECT_LE(peak, 1.05f);
  EXPECT_GT(peak, 0.95f);
}

TEST(AmpPowerAmp, PresenceLiftsTheHighsAndDepthLiftsTheLows) {
  const double fs = 192000.0;
  const auto response = [&](float presence, float depth, double hz) {
    amp::PowerAmp pa = makePowerAmp(fs, presence, depth);
    return gainDb([&](float x) { return pa.process(x, 1.0f); }, hz, fs, 0.05f, 0.6);
  };
  const double presenceLift = response(1.0f, 0.0f, 4500.0) - response(0.0f, 0.0f, 4500.0);
  const double depthLift = response(0.0f, 1.0f, 100.0) - response(0.0f, 0.0f, 100.0);
  std::cout << "[  POWER   ] presence lift at 4.5 kHz " << presenceLift << " dB, depth lift at 100 Hz "
            << depthLift << " dB" << std::endl;
  EXPECT_GT(presenceLift, 4.0);
  EXPECT_GT(depthLift, 4.0);
  // And each leaves the other band alone.
  EXPECT_LT(std::abs(response(1.0f, 0.0f, 100.0) - response(0.0f, 0.0f, 100.0)), 1.5);
  EXPECT_LT(std::abs(response(0.0f, 1.0f, 4500.0) - response(0.0f, 0.0f, 4500.0)), 1.5);
}

TEST(AmpPowerAmp, TheFeedbackLoopIsStableAtEverySettingAndRate) {
  for (const double fs : {48000.0, 96000.0, 192000.0, 384000.0})
    for (const float presence : {0.0f, 1.0f})
      for (const float depth : {0.0f, 1.0f}) {
        amp::PowerAmp pa = makePowerAmp(fs, presence, depth);
        float tail = 0.0f;
        const int n = static_cast<int>(1.0 * fs);
        for (int i = 0; i < n; ++i) {
          // A small impulse stays in the linear loop.
          const float y = pa.process(i == 0 ? 0.01f : 0.0f, 1.0f);
          ASSERT_TRUE(std::isfinite(y)) << fs;
          if (i > n / 2)
            tail = std::max(tail, std::abs(y));
        }
        EXPECT_LT(tail, 1.0e-6f) << "fs " << fs << " presence " << presence << " depth " << depth;
      }
}

TEST(AmpPowerAmp, CrossoverReducesGainAtIdle) {
  const double fs = 192000.0;
  amp::PowerAmpParams withNotch, without;
  withNotch.crossoverRatio = 0.7f;
  without.crossoverRatio = 1.0f;
  amp::PowerAmp a, b;
  a.prepare(fs, withNotch);
  b.prepare(fs, without);
  a.setPresence(0.0f);
  b.setPresence(0.0f);
  const double quiet = gainDb([&](float x) { return a.process(x, 1.0f); }, 500.0, fs, 0.001f, 0.5);
  const double quietPlain = gainDb([&](float x) { return b.process(x, 1.0f); }, 500.0, fs, 0.001f, 0.5);
  EXPECT_LT(quiet, quietPlain - 0.3);
}

TEST(AmpPowerAmp, ResetClearsTheFeedbackState) {
  amp::PowerAmp pa = makePowerAmp(192000.0, 0.5f, 0.5f);
  std::vector<float> first, second;
  for (int i = 0; i < 2000; ++i)
    first.push_back(pa.process(0.3f * static_cast<float>(std::sin(i * 0.05)), 1.0f));
  pa.reset();
  for (int i = 0; i < 2000; ++i)
    second.push_back(pa.process(0.3f * static_cast<float>(std::sin(i * 0.05)), 1.0f));
  EXPECT_EQ(first, second);
}

TEST(AmpSag, SupplyComponentsGiveTheDepthAndTheRecovery) {
  const amp::SagParams p = amp::sagParamsFor(amp::SagSupply{450.0f, 250.0f, 0.3f, 220.0e-6f, 0.008f});
  EXPECT_NEAR(p.depth, 250.0 * 0.3 / 450.0, 1.0e-5);       // R * I / B+
  EXPECT_NEAR(p.releaseSeconds, 250.0 * 220.0e-6, 1.0e-6);  // R * C
  const amp::SagParams stiff = amp::sagParamsFor(amp::SagSupply{450.0f, 100.0f, 0.3f, 220.0e-6f, 0.008f});
  EXPECT_LT(stiff.depth, p.depth);
  EXPECT_LT(stiff.releaseSeconds, p.releaseSeconds);
}

TEST(AmpPowerAmp, ComponentValuesGiveTheFeedbackAndResonance) {
  const amp::PowerAmpParams p;
  EXPECT_NEAR(p.feedbackFraction(), 0.2f, 1.0e-6f);  // Rs / (Rs + Rfb)
  EXPECT_NEAR(p.depthHz(), 100.0f, 2.0f);            // 1 / (2 pi sqrt(L C))
  EXPECT_NEAR(p.depthQ(), 1.2f, 0.05f);              // sqrt(L / C) / R
}
