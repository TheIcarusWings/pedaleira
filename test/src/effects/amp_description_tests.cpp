// The amp description format and the amp kinds built on it:
//
//   - the loader (plugin/src/effects/amp/dsp/AmpDescription.cpp): the shipped
//     descriptions, the provenance of values, and what it refuses and where it
//     says the fault is;
//   - amp.highgain, now the shipped resources/amps/high-gain.json, against a
//     frozen copy of the amp as it was when its constants were hard-coded
//     (amp_highgain_reference.h): the output must be the same samples;
//   - amp.custom: a block that takes a description, keeps it in its state, and
//     builds the amp it describes, with either stage model.
#include "effects/effect_test_helpers.h"

#include "amp/AmpDescription.h"
#include "amp_highgain_reference.h"
#include "effects/amp/BuiltinAmps.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <string>

using namespace fxtest;

namespace {

constexpr double kRate = 48000.0;

std::string shipped(const char* name = "high-gain.json") {
  std::string text = ampfx::builtinAmpText(name);
  EXPECT_FALSE(text.empty()) << name;
  return text;
}

// `text` with the first `from` replaced by `to`.
std::string edited(std::string text, const std::string& from, const std::string& to) {
  const size_t at = text.find(from);
  EXPECT_NE(at, std::string::npos) << "the shipped file has no '" << from << "' to edit";
  if (at != std::string::npos)
    text.replace(at, from.size(), to);
  return text;
}

struct Parsed {
  bool ok{false};
  std::string error;
  amp::AmpDescription description;
};

Parsed parse(const std::string& text) {
  Parsed result;
  result.ok = amp::parseAmpDescription(text, result.description, result.error);
  return result;
}

void expectRejected(const std::string& text, const std::string& fragment) {
  const Parsed parsed = parse(text);
  EXPECT_FALSE(parsed.ok) << "expected an error mentioning '" << fragment << "'";
  EXPECT_NE(parsed.error.find(fragment), std::string::npos)
      << "the error should mention '" << fragment << "' but says: " << parsed.error;
}

// --- Rendering one processor, with parameter changes at given samples --------

struct Change {
  size_t at;
  const char* id;
  double value;
};

Stereo renderProcessor(BlockProcessor& processor, const Params& params, const Stereo& in, int block,
                       int channels, const std::vector<Change>& changes = {}) {
  for (const auto& [id, value] : params) {
    const int index = processor.findParameter(id);
    EXPECT_GE(index, 0) << id.toStdString();
    if (index >= 0)
      processor.setParameter(index, static_cast<float>(value));
  }
  BlockPrepareSpec spec;
  spec.chainSampleRate = kRate;
  spec.oversampleFactor = 1;
  spec.baseBlockSize = block;
  spec.maxBlockSize = block;
  spec.numChannels = 2;
  processor.prepareForChain(spec);
  BlockProcessContext ctx;
  const juce::ScopedNoDenormals noDenormals;
  Stereo out;
  out.left.assign(in.left.size(), 0.0f);
  out.right.assign(in.left.size(), 0.0f);
  juce::AudioBuffer<float> buffer(channels, block);
  size_t nextChange = 0;
  for (size_t offset = 0; offset < in.left.size(); offset += static_cast<size_t>(block)) {
    while (nextChange < changes.size() && changes[nextChange].at <= offset) {
      const int index = processor.findParameter(changes[nextChange].id);
      processor.setParameter(index, static_cast<float>(changes[nextChange].value));
      ++nextChange;
    }
    const int n = static_cast<int>(std::min(static_cast<size_t>(block), in.left.size() - offset));
    buffer.copyFrom(0, 0, in.left.data() + offset, n);
    if (channels > 1)
      buffer.copyFrom(1, 0, in.right.data() + offset, n);
    juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), channels, n);
    processor.processInChain(view, ctx);
    std::copy_n(view.getReadPointer(0), n, out.left.begin() + static_cast<std::ptrdiff_t>(offset));
    std::copy_n(view.getReadPointer(channels > 1 ? 1 : 0), n, out.right.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return out;
}

double maxAbsDifference(const Stereo& a, const Stereo& b) {
  double worst = 0.0;
  for (size_t i = 0; i < a.left.size(); ++i) {
    worst = std::max(worst, static_cast<double>(std::abs(a.left[i] - b.left[i])));
    worst = std::max(worst, static_cast<double>(std::abs(a.right[i] - b.right[i])));
  }
  return worst;
}

double peakOf(const Stereo& a) {
  double peak = 0.0;
  for (size_t i = 0; i < a.left.size(); ++i)
    peak = std::max({peak, static_cast<double>(std::abs(a.left[i])), static_cast<double>(std::abs(a.right[i]))});
  return peak;
}

Stereo guitarLike(unsigned seed, bool differentChannels = false) {
  std::vector<float> in = noise(1.0, seed, 0.2f, kRate);
  const std::vector<float> tone = sine(1.0, 220.0, 0.125f, kRate);
  for (size_t i = 0; i < in.size(); ++i)
    in[i] = tone[i] + 0.02f * in[i];
  Stereo stereo{in, in};
  if (differentChannels)
    stereo.right = noise(1.0, seed + 1u, 0.15f, kRate);
  return stereo;
}

std::unique_ptr<BlockProcessor> makeKind(const char* id) { return BlockKindRegistry::instance().create(id); }

double levelDbOf(const std::vector<float>& x) { return toDb(rmsOf(x, static_cast<size_t>(0.3 * kRate))); }

}  // namespace

// ---------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------

TEST(AmpDescriptionLoader, TheShippedHighGainDescriptionLoads) {
  const Parsed parsed = parse(shipped());
  ASSERT_TRUE(parsed.ok) << parsed.error;
  const amp::AmpDescription& d = parsed.description;
  EXPECT_EQ(d.name, "High Gain");
  EXPECT_EQ(d.licence, "MIT");
  EXPECT_EQ(d.model, amp::StageModel::Classic);
  ASSERT_EQ(d.numStages, 4);
  EXPECT_EQ(d.potStage, 0);
  EXPECT_TRUE(d.stages[0].spec.hasPot);
  EXPECT_EQ(d.stages[0].tubeType, "12AX7");
  EXPECT_FLOAT_EQ(d.stages[0].spec.triode.tube.mu, 100.0f);
  EXPECT_FLOAT_EQ(d.stages[0].spec.triode.tube.rp, 62.5e3f);
  EXPECT_EQ(d.stages[0].spec.bypass, amp::CathodeBypass::Capacitor);
  EXPECT_FLOAT_EQ(d.stages[0].spec.bypassFarads, 0.68e-6f);
  EXPECT_EQ(d.stages[1].spec.bypass, amp::CathodeBypass::Ideal);
  EXPECT_FALSE(d.stages[3].spec.hasNext);
  EXPECT_FLOAT_EQ(d.stages[1].spec.coupling.seriesResistance, 470.0e3f);
  ASSERT_EQ(d.numVoicings, 3);
  EXPECT_EQ(d.voicings[0].name, "Tight");
  EXPECT_EQ(d.voicings[0].rectifier, "solid_state");
  EXPECT_FLOAT_EQ(d.voicings[0].supply.sourceResistance, 100.0f);
  EXPECT_FLOAT_EQ(d.voicings[2].couplingFarads[0], 15.0e-9f);
  EXPECT_EQ(d.defaultVoicing, 1);
  EXPECT_DOUBLE_EQ(d.tone.parts.r1, 220.0e3);
  EXPECT_EQ(d.power.tube, "EL34");
  EXPECT_EQ(d.power.topology, "push_pull_ab");
  EXPECT_FLOAT_EQ(d.power.params.feedbackResistor, 4000.0f);
  EXPECT_FLOAT_EQ(d.power.supply.plateVolts, 450.0f);
  EXPECT_FLOAT_EQ(d.calibration.powerDriveVolts, 40.0f);
  EXPECT_FALSE(d.effectsLoopPresent);
  for (int i = 0; i < amp::kPanelControls; ++i)
    EXPECT_TRUE(d.panel[i].present) << amp::panelIdName(static_cast<amp::PanelId>(i));
  EXPECT_FLOAT_EQ(d.control(amp::PanelId::Bass).taper, 2.0f);
  EXPECT_FLOAT_EQ(d.control(amp::PanelId::Master).taper, 2.0f);
  EXPECT_FLOAT_EQ(d.control(amp::PanelId::Gain).taper, 1.0f);
  EXPECT_EQ(d.control(amp::PanelId::Gain).target, "stages[0].gain_pot");
}

TEST(AmpDescriptionLoader, TheWaveDigitalVariantIsTheSameAmpWithAnotherSolver) {
  const Parsed classic = parse(shipped("high-gain.json"));
  const Parsed wdf = parse(shipped("high-gain-wdf.json"));
  ASSERT_TRUE(classic.ok) << classic.error;
  ASSERT_TRUE(wdf.ok) << wdf.error;
  EXPECT_EQ(wdf.description.model, amp::StageModel::Wdf);
  EXPECT_EQ(wdf.description.numStages, classic.description.numStages);
  for (int k = 0; k < classic.description.numStages; ++k) {
    EXPECT_FLOAT_EQ(wdf.description.stages[k].spec.plateFarads, classic.description.stages[k].spec.plateFarads);
    EXPECT_FLOAT_EQ(wdf.description.stages[k].spec.triode.plateLoad, classic.description.stages[k].spec.triode.plateLoad);
  }
  EXPECT_DOUBLE_EQ(wdf.description.tone.parts.c1, classic.description.tone.parts.c1);
}

TEST(AmpDescriptionLoader, ProvenanceIsKeptForFittedValuesAndForAnythingWithANote) {
  const Parsed parsed = parse(shipped());
  ASSERT_TRUE(parsed.ok) << parsed.error;
  bool plate = false, supply = false, calibration = false;
  for (const amp::ValueNote& note : parsed.description.notes) {
    if (note.path == "stages[0].plate_capacitance_f") {
      plate = true;
      EXPECT_EQ(note.source, amp::Provenance::Fitted);
      EXPECT_NE(note.note.find("Miller"), std::string::npos);
      EXPECT_NEAR(note.value, 295.0e-12, 1.0e-15);
    }
    if (note.path == "voicings[0].supply.source_ohm") {
      supply = true;
      EXPECT_EQ(note.source, amp::Provenance::Fitted);
    }
    if (note.path == "calibration.power_drive_v") {
      calibration = true;
      EXPECT_EQ(note.source, amp::Provenance::Fitted);
    }
  }
  EXPECT_TRUE(plate);
  EXPECT_TRUE(supply);
  EXPECT_TRUE(calibration);
}

TEST(AmpDescriptionLoader, AMeasuredValueCanBeMarkedWithItsNote) {
  const std::string text = edited(shipped(), "\"plate_load_ohm\": 100000,",
                                  "\"plate_load_ohm\": {\"value\": 98500, \"provenance\": \"measured\", \"note\": \"bench, 21 C\"},");
  const Parsed parsed = parse(text);
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_FLOAT_EQ(parsed.description.stages[0].spec.triode.plateLoad, 98500.0f);
  bool found = false;
  for (const amp::ValueNote& note : parsed.description.notes)
    if (note.path == "stages[0].plate_load_ohm") {
      found = true;
      EXPECT_EQ(note.source, amp::Provenance::Measured);
      EXPECT_EQ(note.note, "bench, 21 C");
    }
  EXPECT_TRUE(found);
}

TEST(AmpDescriptionLoader, RefusesWhatIsWrongAndSaysWhere) {
  const std::string good = shipped();
  expectRejected("{", "not valid JSON");
  expectRejected("[]", "top level");
  expectRejected(edited(good, "\"version\": 1", "\"version\": 2"), "version");
  expectRejected(edited(good, "\"format\": \"pedaleira-amp\"", "\"format\": \"other\""), "format");
  expectRejected(edited(good, "\"licence\": \"MIT\"", "\"licence\": \"\""), "metadata.licence");
  expectRejected(edited(good, "\"summary\":", "\"sumary\":"), "metadata.sumary: unknown field");
  expectRejected(edited(good, "\"plate_load_ohm\": 100000,", "\"plate_load_ohm\": \"big\","), "stages[0].plate_load_ohm");
  expectRejected(edited(good, "\"plate_load_ohm\": 100000,", "\"plate_load_ohm\": -5,"), "stages[0].plate_load_ohm: must be between");
  expectRejected(edited(good, "\"tube\": \"12AX7\"", "\"tube\": \"13AX7\""), "stages[0].tube");
  expectRejected(edited(good, "\"topology\": \"push_pull_ab\"", "\"topology\": \"single_ended\""), "power_amp.topology");
  expectRejected(edited(good, "\"stage_solver\": \"classic\"", "\"stage_solver\": \"spice\""), "model.stage_solver");
  expectRejected(edited(good, "\"target\": \"tone_stack.bass_pot_ohm\"", "\"target\": \"tone_stack.treble_pot_ohm\""),
                 "panel.bass.target");
  expectRejected(edited(good, "\"bass\": {", "\"basss\": {"), "panel.basss: unknown control");
  expectRejected(edited(good, "[4.7e-9, 2.7e-9, 2.0e-9]", "[4.7e-9, null, 2.0e-9]"), "link 1");
  expectRejected(edited(good, "[4.7e-9, 2.7e-9, 2.0e-9]", "[4.7e-9, 2.7e-9]"), "voicings[0].coupling_capacitors_f");
  expectRejected(edited(good, "\"default_voicing\": 1", "\"default_voicing\": 7"), "default_voicing");
  expectRejected(edited(good, "\"cathode\": { \"resistor_ohm\": 1500, \"bias_current_a\": 0.001, \"bypass_capacitor_f\": 0.68e-6 }",
                        "\"cathode\": { \"resistor_ohm\": 1500, \"bias_current_a\": 0.001, \"bypass_capacitor_f\": \"short\" }"),
                 "stages[0].cathode.bypass_capacitor_f");
  expectRejected(edited(good, "\"rectifier\": \"solid_state\"", "\"rectifier\": \"magic\""), "voicings[0].supply.rectifier");
  // Nothing follows the last stage.
  expectRejected(edited(good, "\"cathode\": { \"resistor_ohm\": 1500, \"bias_current_a\": 0.001, \"bypass_capacitor_f\": \"ideal\" }\n    }\n  ],",
                        "\"cathode\": { \"resistor_ohm\": 1500, \"bias_current_a\": 0.001, \"bypass_capacitor_f\": \"ideal\" },\n"
                        "      \"coupling\": { \"grid_leak_ohm\": 1000000 }\n    }\n  ],"),
                 "stages[3].coupling");
}

TEST(AmpDescriptionLoader, ATubeOtherThanThe12AX7NeedsItsOwnCutoffVoltage) {
  const std::string text = edited(shipped(), "\"tube\": \"12AX7\",\n      \"plate_load_ohm\": 100000,\n      \"plate_capacitance_f\": { \"value\": 345e-12",
                                  "\"tube\": \"12AT7\",\n      \"plate_load_ohm\": 100000,\n      \"plate_capacitance_f\": { \"value\": 345e-12");
  expectRejected(text, "stages[1].grid.cutoff_v");
  const std::string withGrid = edited(text, "\"cathode\": { \"resistor_ohm\": 1500, \"bias_current_a\": 0.001, \"bypass_capacitor_f\": \"ideal\" },\n      \"coupling\": { \"series_ohm\": 470000, \"grid_leak_ohm\": 25000 }",
                                      "\"cathode\": { \"resistor_ohm\": 1500, \"bias_current_a\": 0.001, \"bypass_capacitor_f\": \"ideal\" },\n      \"grid\": { \"cutoff_v\": 4.2 },\n      \"coupling\": { \"series_ohm\": 470000, \"grid_leak_ohm\": 25000 }");
  const Parsed parsed = parse(withGrid);
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_FLOAT_EQ(parsed.description.stages[1].spec.triode.tube.mu, 60.0f);
  EXPECT_FLOAT_EQ(parsed.description.stages[1].spec.triode.cutoffGridVolts, 4.2f);
}

namespace {

// The shipped amp with a tone cut ahead of the power tubes, and its presence
// knob moved onto it when `onPresence`.
std::string withToneCut(bool onPresence) {
  std::string text = edited(shipped(), "\"depth\": { \"inductance_h\": 0.12,",
                            "\"tone_cut\": { \"source_ohm\": 100000, \"pot_ohm\": 100000, \"min_ohm\": 2200, \"capacitor_f\": 4.7e-9 },\n"
                            "    \"depth\": { \"inductance_h\": 0.12,");
  if (onPresence)
    text = edited(text, "\"presence\": { \"label\": \"Presence\", \"target\": \"power_amp.presence\" }",
                  "\"presence\": { \"label\": \"Tone Cut\", \"target\": \"power_amp.tone_cut\" }");
  return text;
}

}  // namespace

TEST(AmpDescriptionLoader, AToneCutIsReadAndThePresenceKnobCanDriveIt) {
  const Parsed plain = parse(shipped());
  ASSERT_TRUE(plain.ok) << plain.error;
  EXPECT_FALSE(plain.description.power.hasToneCut);
  EXPECT_FALSE(plain.description.presenceDrivesToneCut);

  const Parsed held = parse(withToneCut(false));
  ASSERT_TRUE(held.ok) << held.error;
  EXPECT_TRUE(held.description.power.hasToneCut);
  EXPECT_FALSE(held.description.presenceDrivesToneCut) << "a tone cut no knob drives is held at the middle";
  EXPECT_FLOAT_EQ(held.description.power.toneCut.sourceResistance, 100000.0f);
  EXPECT_FLOAT_EQ(held.description.power.toneCut.minResistance, 2200.0f);
  EXPECT_FLOAT_EQ(held.description.power.toneCut.capacitance, 4.7e-9f);

  const Parsed driven = parse(withToneCut(true));
  ASSERT_TRUE(driven.ok) << driven.error;
  EXPECT_TRUE(driven.description.presenceDrivesToneCut);
  EXPECT_EQ(driven.description.control(amp::PanelId::Presence).label, "Tone Cut");
  EXPECT_EQ(driven.description.control(amp::PanelId::Presence).target, "power_amp.tone_cut");

  // Without the network there is nothing for the knob to drive; the network
  // needs its parts; only the presence control can drive it.
  expectRejected(edited(shipped(), "\"target\": \"power_amp.presence\"", "\"target\": \"power_amp.tone_cut\""),
                 "panel.presence.target: this amp has no tone cut");
  expectRejected(edited(withToneCut(false), "\"capacitor_f\": 4.7e-9 }", "\"cap\": 4.7e-9 }"),
                 "power_amp.tone_cut.cap: unknown field");
  expectRejected(edited(withToneCut(false), ", \"capacitor_f\": 4.7e-9 }", " }"), "power_amp.tone_cut.capacitor_f: missing");
  expectRejected(edited(withToneCut(false), "\"target\": \"power_amp.depth\"", "\"target\": \"power_amp.tone_cut\""),
                 "panel.depth.target");
}

TEST(AmpDescriptionLoader, AMinimalDescriptionTakesDefaultsForTheRest) {
  const char* text = R"({
    "format": "pedaleira-amp", "version": 1,
    "metadata": { "name": "Two Stage", "licence": "CC0-1.0" },
    "stages": [
      { "tube": "12AX7", "plate_load_ohm": 100000,
        "cathode": { "resistor_ohm": 1500, "bias_current_a": 0.001 },
        "coupling": { "capacitance_f": 22e-9, "grid_leak_ohm": 470000 } },
      { "tube": "12AX7", "plate_load_ohm": 100000,
        "cathode": { "resistor_ohm": 1500, "bias_current_a": 0.001 } }
    ],
    "tone_stack": { "treble_pot_ohm": 250000, "bass_pot_ohm": 250000, "middle_pot_ohm": 10000, "slope_ohm": 100000,
                    "treble_cap_f": 250e-12, "bass_cap_f": 100e-9, "middle_cap_f": 47e-9 },
    "power_amp": { "supply": { "b_plus_v": 400 } }
  })";
  const Parsed parsed = parse(text);
  ASSERT_TRUE(parsed.ok) << parsed.error;
  const amp::AmpDescription& d = parsed.description;
  EXPECT_EQ(d.numStages, 2);
  EXPECT_EQ(d.numVoicings, 1);
  EXPECT_EQ(d.potStage, -1);
  EXPECT_FALSE(d.control(amp::PanelId::Gain).present) << "no pot: no gain knob";
  EXPECT_TRUE(d.control(amp::PanelId::Bass).present);
  EXPECT_FLOAT_EQ(d.control(amp::PanelId::Master).taper, 2.0f);
  EXPECT_EQ(d.stages[0].name, "stage 1");
  EXPECT_EQ(d.stages[0].spec.bypass, amp::CathodeBypass::Ideal);
  EXPECT_FLOAT_EQ(d.power.supply.plateVolts, 400.0f);
}

// ---------------------------------------------------------------------------
// amp.highgain is the shipped description, and sounds as it did
// ---------------------------------------------------------------------------

TEST(AmpHighGainFromDescription, MatchesTheHardCodedAmpItReplaced) {
  struct Case {
    const char* name;
    Params params;
    int channels;
    int block;
    bool differentChannels;
    std::vector<Change> changes;
  };
  const std::vector<Case> cases = {
      {"defaults", {}, 1, 256, false, {}},
      {"hot", {{"gain", 80}, {"bass", 70}, {"middle", 30}, {"treble", 70}, {"presence", 60}, {"depth", 40}, {"master", 70}, {"bright", 1}, {"voicing", 0}}, 1, 128, false, {}},
      {"loose, stereo, different channels", {{"gain", 10}, {"voicing", 2}, {"master", 30}, {"levelDb", -6}, {"bass", 20}, {"treble", 90}}, 2, 64, true, {}},
      {"clean, big blocks", {{"gain", 0}, {"master", 50}, {"middle", 100}}, 1, 512, false, {}},
      {"identical channels (mirrored)", {{"gain", 55}, {"depth", 100}, {"presence", 0}}, 2, 128, false, {}},
      {"knobs moved while playing", {}, 2, 100, true,
       {{9000, "gain", 90.0}, {15000, "voicing", 0.0}, {20000, "bass", 10.0}, {26000, "bright", 1.0}, {31000, "master", 80.0}, {38000, "voicing", 2.0}}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const Stereo in = guitarLike(21u, c.differentChannels);
    legacyamp::LegacyHighGainProcessor legacy;
    const std::unique_ptr<BlockProcessor> shippedAmp = makeKind("amp.highgain");
    ASSERT_NE(shippedAmp, nullptr);
    const Stereo before = renderProcessor(legacy, c.params, in, c.block, c.channels, c.changes);
    const Stereo after = renderProcessor(*shippedAmp, c.params, in, c.block, c.channels, c.changes);
    const double worst = maxAbsDifference(before, after);
    std::cout << "[  AMP     ] " << c.name << ": peak " << peakOf(before) << ", largest difference between the hard-coded amp and the description "
              << worst << std::endl;
    EXPECT_GT(peakOf(before), 0.01);
    // Held settings must match exactly. When knobs move, the coefficient updates may be contracted
    // into fused multiply-adds differently by the compiler (seen on arm64: 3.4e-5 on a 0.21 peak),
    // so that case allows a difference far below hearing (about -66 dB re its peak).
    EXPECT_LE(worst, c.changes.empty() ? 1.0e-6 : 1.0e-4);
  }
}

TEST(AmpHighGainFromDescription, KeepsItsParametersAndReadings) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find("amp.highgain");
  ASSERT_NE(info, nullptr);
  ASSERT_EQ(info->params.size(), 10u);
  EXPECT_EQ(info->params[9].choices.joinIntoString(","), juce::String("Tight,Classic,Loose"));
  EXPECT_DOUBLE_EQ(info->params[9].defaultValue, 1.0);
  ASSERT_EQ(info->readings.size(), 1u);
  EXPECT_EQ(info->readings[0].id, juce::String("sag"));
}

// ---------------------------------------------------------------------------
// amp.custom
// ---------------------------------------------------------------------------

TEST(AmpCustom, StartsAsTheHighGainAmp) {
  const Stereo in = guitarLike(31u);
  const auto custom = makeKind("amp.custom");
  const auto highGain = makeKind("amp.highgain");
  ASSERT_NE(custom, nullptr);
  ASSERT_TRUE(custom->takesDescription());
  EXPECT_FALSE(highGain->takesDescription());
  const Params params = {{"gain", 70}, {"voicing", 1}, {"bright", 1}};
  const Stereo a = renderProcessor(*custom, params, in, 128, 2);
  const Stereo b = renderProcessor(*highGain, params, in, 128, 2);
  EXPECT_EQ(maxAbsDifference(a, b), 0.0);
  EXPECT_EQ(custom->displayName(), juce::String("High Gain"));
}

TEST(AmpCustom, LoadsADescriptionAndBuildsThatAmp) {
  const Stereo in = guitarLike(32u);
  const auto custom = makeKind("amp.custom");
  const Params params = {{"gain", 60}};
  const Stereo before = renderProcessor(*custom, params, in, 128, 1);

  // A much brighter, tighter amp: smaller coupling capacitors and a 100 pF
  // treble capacitor in the stack.
  std::string text = edited(shipped(), "\"name\": \"High Gain\"", "\"name\": \"Bright Cousin\"");
  text = edited(text, "\"treble_cap_f\": 400e-12", "\"treble_cap_f\": 100e-12");
  text = edited(text, "[8.2e-9, 4.7e-9, 3.9e-9]", "[3.3e-9, 1.5e-9, 1.0e-9]");
  juce::String error;
  ASSERT_TRUE(custom->loadDescription(juce::String(text), error)) << error.toStdString();
  EXPECT_EQ(custom->displayName(), juce::String("Bright Cousin"));
  const Stereo after = renderProcessor(*custom, params, in, 128, 1);
  EXPECT_GT(maxAbsDifference(before, after), 0.01);
  EXPECT_GT(peakOf(after), 0.01);
  EXPECT_LT(peakOf(after), 3.0);
}

TEST(AmpCustom, ABadDescriptionIsRefusedAndTheAmpIsLeftAlone) {
  const Stereo in = guitarLike(33u);
  const auto custom = makeKind("amp.custom");
  const Params params = {{"gain", 60}};
  const Stereo reference = renderProcessor(*makeKind("amp.custom"), params, in, 128, 1);
  juce::String error;
  EXPECT_FALSE(custom->loadDescription("{\"format\": \"pedaleira-amp\", \"version\": 1}", error));
  EXPECT_TRUE(error.isNotEmpty());
  EXPECT_FALSE(custom->checkDescription("not json", error));
  EXPECT_EQ(maxAbsDifference(renderProcessor(*custom, params, in, 128, 1), reference), 0.0);
  EXPECT_EQ(custom->displayName(), juce::String("High Gain"));
  // Other kinds take no description at all.
  const auto drive = makeKind("amp.highgain");
  EXPECT_FALSE(drive->loadDescription(juce::String(shipped()), error));
}

TEST(AmpCustom, TheDescriptionTravelsInTheBlocksState) {
  const Stereo in = guitarLike(34u);
  const Params params = {{"gain", 65}, {"voicing", 2}};
  const auto custom = makeKind("amp.custom");

  // A block that still has the shipped description saves nothing extra.
  juce::ValueTree plain = custom->getKindState("Custom Amp");
  EXPECT_EQ(plain.getProperty("chunk").getBinaryData(), nullptr);

  std::string text = edited(shipped("high-gain-wdf.json"), "\"name\": \"High Gain (wave digital)\"", "\"name\": \"State Test\"");
  juce::String error;
  ASSERT_TRUE(custom->loadDescription(juce::String(text), error)) << error.toStdString();
  juce::ValueTree saved = custom->getKindState("State Test");
  ASSERT_NE(saved.getProperty("chunk").getBinaryData(), nullptr);

  // Restored into a fresh block, and into a block that has another one.
  const auto fresh = makeKind("amp.custom");
  fresh->setKindState(saved);
  EXPECT_EQ(fresh->displayName(), juce::String("State Test"));
  const auto other = makeKind("amp.custom");
  ASSERT_TRUE(other->loadDescription(juce::String(shipped("high-gain.json")), error));
  other->setKindState(saved);
  EXPECT_EQ(other->displayName(), juce::String("State Test"));
  const Stereo a = renderProcessor(*custom, params, in, 128, 1);
  const Stereo b = renderProcessor(*fresh, params, in, 128, 1);
  const Stereo c = renderProcessor(*other, params, in, 128, 1);
  EXPECT_EQ(maxAbsDifference(a, b), 0.0);
  EXPECT_EQ(maxAbsDifference(a, c), 0.0);

  // And a restore to the node without a description puts the shipped one back.
  other->setKindState(plain);
  EXPECT_EQ(other->displayName(), juce::String("High Gain"));
}

TEST(AmpCustom, TheNewDescriptionsDefaultVoicingBecomesTheBlocksVoicing) {
  const auto custom = makeKind("amp.custom");
  const int voicing = custom->findParameter("voicing");
  ASSERT_GE(voicing, 0);
  EXPECT_FLOAT_EQ(custom->getParameter(voicing), 0.0f);
  juce::String error;
  ASSERT_TRUE(custom->loadDescription(juce::String(shipped()), error));
  EXPECT_FLOAT_EQ(custom->getParameter(voicing), 1.0f);
}

TEST(AmpCustom, APanelWithoutAControlLeavesThatPartAtMiddle) {
  // An amp with no treble knob: the Treble parameter does nothing, Bass does.
  const std::string noTreble = edited(shipped(), "    \"treble\": { \"label\": \"Treble\", \"target\": \"tone_stack.treble_pot_ohm\" },\n", "");
  ASSERT_TRUE(parse(noTreble).ok) << parse(noTreble).error;
  const Stereo in = guitarLike(35u);
  auto render = [&](const Params& params) {
    const auto custom = makeKind("amp.custom");
    juce::String error;
    EXPECT_TRUE(custom->loadDescription(juce::String(noTreble), error)) << error.toStdString();
    return renderProcessor(*custom, params, in, 128, 1);
  };
  const Stereo trebleLow = render({{"treble", 0}, {"voicing", 1}});
  const Stereo trebleHigh = render({{"treble", 100}, {"voicing", 1}});
  const Stereo bassLow = render({{"bass", 0}, {"voicing", 1}});
  const Stereo bassHigh = render({{"bass", 100}, {"voicing", 1}});
  EXPECT_EQ(maxAbsDifference(trebleLow, trebleHigh), 0.0);
  EXPECT_GT(maxAbsDifference(bassLow, bassHigh), 0.01);
}

TEST(AmpCustom, APresenceKnobOnTheToneCutTakesTheHighsAway) {
  // Two quiet tones, 300 Hz and 4 kHz, at low gain: the amp stays linear.
  std::vector<float> tones(static_cast<size_t>(kRate));
  for (size_t i = 0; i < tones.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    tones[i] = static_cast<float>(1.0e-3 * (std::sin(2.0 * juce::MathConstants<double>::pi * 300.0 * t) +
                                            std::sin(2.0 * juce::MathConstants<double>::pi * 4000.0 * t)));
  }
  const Stereo in{tones, tones};
  auto render = [&](const std::string& text, double presence) {
    const auto custom = makeKind("amp.custom");
    juce::String error;
    EXPECT_TRUE(custom->loadDescription(juce::String(text), error)) << error.toStdString();
    return renderProcessor(*custom, {{"presence", presence}, {"gain", 0}, {"voicing", 1}}, in, 128, 1);
  };
  auto amplitude = [](const std::vector<float>& x, double hz) {
    double c = 0.0, s = 0.0;
    const size_t from = x.size() / 2;
    for (size_t i = from; i < x.size(); ++i) {
      const double phase = 2.0 * juce::MathConstants<double>::pi * hz * static_cast<double>(i) / kRate;
      c += x[i] * std::cos(phase);
      s += x[i] * std::sin(phase);
    }
    return std::sqrt(c * c + s * s);
  };
  // The 4 kHz tone against the 300 Hz one, in dB.
  auto tiltDb = [&](const Stereo& out) { return toDb(amplitude(out.left, 4000.0) / amplitude(out.left, 300.0)); };
  const std::string driven = withToneCut(true);
  const double open = tiltDb(render(driven, 0.0)), cut = tiltDb(render(driven, 100.0));
  std::cout << "[ TONE CUT ] 4 kHz against 300 Hz: " << open << " dB open, " << cut << " dB cut" << std::endl;
  EXPECT_LT(cut, open - 10.0) << "turning the tone cut up darkens the amp";
  // The presence network stays at the middle: with the tone cut held there
  // too, presence 50 on either description is the same amp.
  const Stereo a = render(driven, 50.0);
  const Stereo b = render(withToneCut(false), 50.0);
  EXPECT_LT(maxAbsDifference(a, b), 1.0e-9);
  // And the amp without a tone cut is not the amp with one.
  EXPECT_GT(maxAbsDifference(b, render(shipped(), 50.0)), 1.0e-6);
}

TEST(AmpCustom, TheKnobTaperAndRangeOfThePanelAreHonoured) {
  // Gain with a travel limited to the upper half of the pot: knob 0 is where
  // the full amp sits at 50 percent, knob 100 where it sits at 100.
  const std::string limited = edited(shipped(), "\"gain\": { \"label\": \"Gain\", \"target\": \"stages[0].gain_pot\" }",
                                     "\"gain\": { \"label\": \"Gain\", \"target\": \"stages[0].gain_pot\", \"range\": [0.5, 1.0] }");
  const Stereo in = guitarLike(36u);
  juce::String error;
  const auto ranged = makeKind("amp.custom");
  ASSERT_TRUE(ranged->loadDescription(juce::String(limited), error)) << error.toStdString();
  const auto plain = makeKind("amp.custom");
  const Stereo a = renderProcessor(*ranged, {{"gain", 0}, {"voicing", 1}}, in, 128, 1);
  const Stereo b = renderProcessor(*plain, {{"gain", 50}, {"voicing", 1}}, in, 128, 1);
  EXPECT_LT(maxAbsDifference(a, b), 1.0e-5);
}

TEST(AmpCustom, ConformsInMonoStereoAndDualMono) {
  ConformanceOptions options;
  options.params = {{"gain", 70.0}, {"bass", 60.0}, {"middle", 40.0}, {"treble", 65.0},
                    {"presence", 55.0}, {"depth", 45.0}, {"master", 60.0}, {"bright", 1.0}, {"voicing", 1.0}};
  options.stateParams = {{"gain", 20.0}, {"bass", 30.0}, {"middle", 80.0}, {"treble", 25.0},
                         {"presence", 10.0}, {"depth", 90.0}, {"master", 35.0}, {"levelDb", -6.0},
                         {"voicing", 2.0}};
  options.sweepBound = 12.0f;
  options.cpuAverageBudgetNs = 80000.0;
  options.cpuWorstBudgetNs = 600000.0;
  expectKindConforms("amp.custom", options);
}

// ---------------------------------------------------------------------------
// The wave digital model, end to end
// ---------------------------------------------------------------------------

TEST(AmpCustomWdf, ARealAmpSoundsLikeTheClassicOneWithinAFewDecibels) {
  const Stereo in = guitarLike(41u);
  const Params params = {{"gain", 50}, {"voicing", 1}};
  const auto classic = makeKind("amp.custom");
  const auto wdf = makeKind("amp.custom");
  juce::String error;
  ASSERT_TRUE(wdf->loadDescription(juce::String(shipped("high-gain-wdf.json")), error)) << error.toStdString();
  const Stereo a = renderProcessor(*classic, params, in, 128, 1);
  const Stereo b = renderProcessor(*wdf, params, in, 128, 1);
  const double levelClassic = levelDbOf(a.left), levelWdf = levelDbOf(b.left);
  std::cout << "[  AMP     ] defaults at -18 dBFS: classic " << levelClassic << " dBFS rms, wave digital " << levelWdf
            << " dBFS rms" << std::endl;
  EXPECT_NEAR(levelWdf, levelClassic, 4.0);
  EXPECT_GT(levelWdf, -40.0);
}

TEST(AmpCustomWdf, DistortionRisesWithGainAndTheOutputStaysFinite) {
  const auto wdf = makeKind("amp.custom");
  juce::String error;
  ASSERT_TRUE(wdf->loadDescription(juce::String(shipped("high-gain-wdf.json")), error)) << error.toStdString();
  double last = -1.0;
  for (const double gain : {0.0, 30.0, 60.0, 100.0}) {
    DirectKind kind("amp.custom", {{"voicing", 1}});
    // The block is built by the registry; load the description into it.
    ASSERT_TRUE(kind.engine().loadDescription(juce::String(shipped("high-gain-wdf.json")), error));
    kind.engine().setParameter(kind.engine().findParameter("gain"), static_cast<float>(gain));
    kind.prepare();
    const std::vector<float> out = kind.render(sine(1.0, 1000.0, 0.1f, kRate));
    for (const float v : out)
      ASSERT_TRUE(std::isfinite(v));
    const double thd = thdPercent(out, 1000.0, kRate, static_cast<size_t>(0.3 * kRate), 10);
    std::cout << "[   THD    ] wave digital amp, 1 kHz at -20 dBFS, gain " << gain << ": " << thd << " %" << std::endl;
    EXPECT_GT(thd, last);
    last = thd;
  }
}

TEST(AmpCustomWdf, SurvivesRandomKnobsAndHotInput) {
  const auto wdf = makeKind("amp.custom");
  juce::String error;
  ASSERT_TRUE(wdf->loadDescription(juce::String(shipped("high-gain-wdf.json")), error)) << error.toStdString();
  BlockPrepareSpec spec;
  spec.chainSampleRate = kRate;
  spec.oversampleFactor = 1;
  spec.baseBlockSize = 128;
  spec.maxBlockSize = 128;
  spec.numChannels = 2;
  wdf->prepareForChain(spec);
  std::mt19937 random(3);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  juce::AudioBuffer<float> buffer(2, 128);
  BlockProcessContext ctx;
  const juce::ScopedNoDenormals noDenormals;
  float peak = 0.0f;
  for (int block = 0; block < 600; ++block) {
    for (int p = 0; p < 10; ++p)
      if (unit(random) < 0.3f) {
        const BlockParamSpec& s = wdf->parameterTable()[static_cast<size_t>(p)];
        wdf->setParameter(p, s.min + unit(random) * (s.max - s.min));
      }
    for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < 128; ++i)
        buffer.setSample(ch, i, (unit(random) * 2.0f - 1.0f) * (block % 50 < 25 ? 1.0f : 0.01f));
    wdf->processInChain(buffer, ctx);
    for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < 128; ++i) {
        const float v = buffer.getSample(ch, i);
        ASSERT_TRUE(std::isfinite(v)) << "block " << block;
        peak = std::max(peak, std::abs(v));
      }
  }
  EXPECT_LT(peak, 12.0f);
}

// ---------------------------------------------------------------------------
// Cost
// ---------------------------------------------------------------------------

TEST(AmpCost, PrintsWhatEachStageModelCosts) {
  // One instance at 48 kHz in blocks of 128, the way measureKindCost times a
  // kind. Only printed (and bounded loosely): the numbers belong to the machine.
  auto cost = [](const char* kindId, bool wdf, int channels) {
    std::unique_ptr<BlockProcessor> processor =
        std::string(kindId) == "legacy" ? std::make_unique<legacyamp::LegacyHighGainProcessor>() : makeKind(kindId);
    juce::String error;
    if (wdf)
      EXPECT_TRUE(processor->loadDescription(juce::String(shipped("high-gain-wdf.json")), error));
    BlockPrepareSpec spec;
    spec.chainSampleRate = kRate;
    spec.oversampleFactor = 1;
    spec.baseBlockSize = 128;
    spec.maxBlockSize = 128;
    spec.numChannels = 2;
    const std::vector<float> source = noise(128.0 * 64.0 / kRate, 7u, 0.25f, kRate);
    juce::AudioBuffer<float> buffer(channels, 128);
    BlockProcessContext ctx;
    const juce::ScopedNoDenormals noDenormals;
    double best = 1.0e18;
    for (int pass = 0; pass < 3; ++pass) {
      processor->prepareForChain(spec);
      double total = 0.0;
      const int blocks = 2000;
      for (int b = 0; b < blocks; ++b) {
        const float* chunk = source.data() + static_cast<size_t>(b % 64) * 128;
        for (int ch = 0; ch < channels; ++ch)
          buffer.copyFrom(ch, 0, chunk, 128);
        const auto begin = std::chrono::steady_clock::now();
        processor->processInChain(buffer, ctx);
        const auto end = std::chrono::steady_clock::now();
        total += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
      }
      best = std::min(best, total / blocks);
    }
    return best;
  };
  const double legacyStereo = cost("legacy", false, 2);
  const double legacyMono = cost("legacy", false, 1);
  const double highGainStereo = cost("amp.highgain", false, 2);
  const double customClassicStereo = cost("amp.custom", false, 2);
  const double customWdfStereo = cost("amp.custom", true, 2);
  const double highGainMono = cost("amp.highgain", false, 1);
  const double customWdfMono = cost("amp.custom", true, 1);
  auto percent = [](double ns) { return 100.0 * ns / 2666667.0; };
  std::cout << "[   CPU    ] per 128-sample block at 48 kHz (identical channels, one lane for the classic model, "
               "two for the wave digital one):\n"
            << "[   CPU    ]   hard-coded amp (before) stereo " << legacyStereo << " ns (" << percent(legacyStereo) << "% of a core), mono "
            << legacyMono << " ns (" << percent(legacyMono) << "%)\n"
            << "[   CPU    ]   amp.highgain (description, after) stereo " << highGainStereo << " ns (" << percent(highGainStereo) << "% of a core), mono "
            << highGainMono << " ns (" << percent(highGainMono) << "%)\n"
            << "[   CPU    ]   amp.custom classic stereo " << customClassicStereo << " ns (" << percent(customClassicStereo) << "%)\n"
            << "[   CPU    ]   amp.custom wave digital stereo " << customWdfStereo << " ns (" << percent(customWdfStereo)
            << "%), mono " << customWdfMono << " ns (" << percent(customWdfMono) << "%)" << std::endl;
  EXPECT_LT(percent(customWdfMono), 25.0);
}
