#pragma once
// amp/PowerAmp.h: a push-pull pentode power stage with global negative
// feedback, Presence and Depth.
//
// What it models:
//   - the output stage: a push-pull pair of pentodes, a symmetric soft
//     saturation (a blend of a soft and a sharper knee) scaled by the
//     available headroom, plus class AB crossover character (the gain dips
//     near zero crossing, `crossoverRatio` of the full gain at idle);
//   - global negative feedback from the output back to the input of the
//     stage: closed-loop gain is A / (1 + A * beta * Hfb), so the loop
//     flattens the response and tames distortion where the feedback is
//     strong and lets the stage run freer where it is weak;
//   - Presence: a high-frequency reduction of the feedback (a high-pass
//     subtracted from the feedback signal above the network corner (see below)), so more
//     feedback is lost at the top and the highs rise;
//   - Depth (resonance): a reduction of the feedback in a low band around
//     `depthHz` (a band-pass subtracted from the feedback), so the bass
//     around 80 to 120 Hz rises, as the speaker's impedance peak does in the
//     real circuit;
//   - the feedback path also has a two-pole low-pass (`transformerLowpassHz`,
//     the output transformer bandwidth) which keeps the loop stable: the
//     feedback uses the previous output sample, so the loop has a one sample
//     delay, and the low-pass keeps the loop gain well below one wherever
//     that delay would turn the feedback positive.
// The structure follows the feedback description in J. Pakarinen and
// D. T. Yeh, "A review of digital techniques for modeling vacuum-tube guitar
// amplifiers", Computer Music Journal 33(2), 2009; the transformer, the
// screen grid and the real phase inverter are not modelled.
//
// Parameters (PowerAmpParams), in component terms:
//   openLoopGain (A)      forward gain of the stage before the saturation
//   feedbackResistor, shuntResistor   the feedback divider: beta = Rs / (Rs +
//                         Rfb); A * beta is the loop gain
//   transformerLowpassHz  corner of both feedback low-pass poles
//   presencePot, presenceMinResistor, presenceCapacitance   the Presence
//                         network: a pot (turned up = less resistance) in
//                         series with a capacitor across the shunt resistor.
//                         With Rp = Rmin + Rpot * (1 - position) the feedback
//                         above 1 / (2 pi C (Rp + Rs)) falls to Rp / (Rp + Rs)
//                         of its low-frequency value, so Presence up removes
//                         more of the high-frequency feedback.
//   depthInductance, depthCapacitance, depthResistance   the resonance branch
//                         (the speaker impedance peak): f0 = 1 / (2 pi sqrt(L
//                         C)) and Q = sqrt(L / C) / R set the centre and width
//                         of the band whose feedback Depth removes
//   depthMaxCut           the largest fraction of that band's feedback removed
//                         at Depth 1 (fitted, not a component value)
//   kneeMix               0 soft knee (x / sqrt(1 + x^2)) .. 1 sharp knee
//                         (the sharp part is not antialiased; 0 by default)
//   crossoverRatio        small-signal gain at idle relative to full gain
//   crossoverWidth        level over which the crossover notch recovers
// Controls: setPresence(0..1) and setDepth(0..1).
//
// Interface: process(x, headroom) returns the output (after the transformer
// low-pass); the headroom (about 0.5..1, from SagModel) scales the level the
// stage can swing to. The output stays within about 10 percent of it (the
// low-pass overshoots a clipped wave slightly).
//
// Sample rate: prepare(sampleRate) sets every filter for the rate the stage
// runs at. The stage is nonlinear, so run it oversampled. Plain C++ and std
// only. One instance per channel.
#include <algorithm>
#include <cmath>

#include "amp/FastMath.h"
#include "amp/Filters.h"

namespace amp {

struct PowerAmpParams {
  float openLoopGain{20.0f};
  float feedbackResistor{4000.0f};
  float shuntResistor{1000.0f};
  float transformerLowpassHz{9000.0f};
  float presencePot{5000.0f};
  float presenceMinResistor{220.0f};
  float presenceCapacitance{47.0e-9f};
  float depthInductance{0.12f};
  float depthCapacitance{21.0e-6f};
  float depthResistance{63.0f};
  float depthMaxCut{0.85f};
  float kneeMix{0.0f};
  float crossoverRatio{0.9f};
  float crossoverWidth{0.3f};

  float feedbackFraction() const noexcept { return shuntResistor / (shuntResistor + feedbackResistor); }
  float depthHz() const noexcept {
    return 1.0f / (static_cast<float>(kTwoPi) * std::sqrt(depthInductance * depthCapacitance));
  }
  float depthQ() const noexcept { return std::sqrt(depthInductance / depthCapacitance) / depthResistance; }
};

class PowerAmp {
public:
  void prepare(double newSampleRate, const PowerAmpParams& newParams) noexcept {
    params = newParams;
    sampleRate = newSampleRate;
    beta = params.feedbackFraction();
    depthQ = params.depthQ();
    lowA.setCutoff(params.transformerLowpassHz, sampleRate);
    lowB.setCutoff(params.transformerLowpassHz, sampleRate);
    depth.set(params.depthHz(), depthQ, sampleRate);
    setPresence(0.0f);
    setDepth(0.0f);
    reset();
  }
  void reset() noexcept {
    lowA.reset();
    lowB.reset();
    presence.reset();
    depth.reset();
    feedbackSignal = 0.0f;
    lastT = 0.0f;
    lastRoot = 1.0f;
  }

  /** 0..1: Presence pot position (1 = most high-frequency feedback removed). */
  void setPresence(float position) noexcept {
    const float p = std::clamp(position, 0.0f, 1.0f);
    const float rp = params.presenceMinResistor + params.presencePot * (1.0f - p);
    presenceCut = params.shuntResistor / (params.shuntResistor + rp);
    presence.setCutoff(1.0 / (kTwoPi * params.presenceCapacitance * (rp + params.shuntResistor)), sampleRate);
  }
  /** 0..1: how much of the low band feedback is removed. */
  void setDepth(float position) noexcept { depthCut = params.depthMaxCut * std::clamp(position, 0.0f, 1.0f); }

  float process(float x, float headroom) noexcept {
    const float h = std::max(headroom, 0.05f);
    const float v = params.openLoopGain * (x - beta * feedbackSignal);
    const float t = std::clamp(v * recip(h), -1.0e3f, 1.0e3f);
    const float t2 = t * t;
    // Soft saturation s(t) = t / sqrt(1 + t^2), anti-aliased with its
    // antiderivative sqrt(1 + t^2) (see TriodeStage.h for the method).
    const float a = 1.0f + t2;
    const float root = a * invSqrt(a);
    const float step = t - lastT;
    float shaped = std::abs(step) > 1.0e-3f ? (root - lastRoot) * recip(step)
                                            : (0.5f * (t + lastT)) * invSqrt(1.0f + 0.25f * (t + lastT) * (t + lastT));
    lastT = t;
    lastRoot = root;
    if (params.kneeMix > 0.0f) {
      const float q = invSqrt(1.0f + t2 * t2);
      shaped += params.kneeMix * (t * q * invSqrt(q) - shaped);
    }
    const float w2 = params.crossoverWidth * params.crossoverWidth;
    const float crossover = (t2 + params.crossoverRatio * w2) * recip(t2 + w2);
    const float y = h * shaped * crossover;

    // The feedback for the NEXT sample: the output through the transformer
    // low-pass, minus the presence and depth bands.
    const float lowpassed = lowB.lowpass(lowA.lowpass(y));
    const float depthBand = depth.process(lowpassed).band * (1.0f / depthQ);
    feedbackSignal = flushTiny(lowpassed - presenceCut * presence.highpass(lowpassed) - depthCut * depthBand);
    // The output is taken after the transformer low-pass too, which is also
    // what keeps the saturation's highest harmonics from folding back.
    return lowpassed;
  }

private:
  PowerAmpParams params;
  double sampleRate{48000.0};
  OnePole lowA, lowB, presence;
  Svf depth;
  float beta{0.2f}, depthQ{1.2f};
  float presenceCut{0.0f}, depthCut{0.0f};
  float feedbackSignal{0.0f};
  float lastT{0.0f}, lastRoot{1.0f};
};

}  // namespace amp
