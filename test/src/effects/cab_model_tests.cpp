// cab.model: a guitar cab and its mic from physics (docs/cab-framework.md).
//
//   - conformance, and the parameter table;
//   - the shipped speakers load and are voiced like guitar speakers;
//   - the response follows each control the way the physics says: a closed
//     back has more low end near its resonance than an open one, the centre
//     of the cone is brighter than the edge, distance raises the comb ripple,
//     off axis loses highs, a ribbon is darker than a condenser, a close
//     cardioid gets the proximity bass, the room adds early reflections;
//   - the block plays exactly the response the model computes, at any rate;
//   - a control change swaps the response with no allocation on the audio
//     thread and no click;
//   - block.exportImpulse writes the response; block.loadDescription gives
//     the block a speaker of its own, which travels in the saved state;
//   - an amp description's default cab;
//   - what one block costs, steady and while it swaps.
#include "BlockKinds.h"
#include "EngineCommands.h"
#include "amp/AmpDescription.h"
#include "cab/CabResponse.h"
#include "effects/allocation_guard.h"
#include "effects/amp/BuiltinAmps.h"
#include "effects/cab/BuiltinCabs.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fxtest;

namespace {

constexpr const char* kId = "cab.model";
constexpr double kRate = 48000.0;

// ---- The model, measured directly ---------------------------------------------

std::vector<double> logGrid(double lo, double hi, int perOctave = 24) {
  std::vector<double> f;
  for (double x = lo; x <= hi * 1.0001; x *= std::pow(2.0, 1.0 / perOctave))
    f.push_back(x);
  return f;
}

std::vector<double> responseDb(const cab::SpeakerDescription& speaker, const cab::CabSettings& settings,
                               const std::vector<double>& frequencies) {
  std::vector<double> out;
  for (const auto& h : cab::cabFrequencyResponse(speaker, settings, frequencies))
    out.push_back(20.0 * std::log10(std::abs(h) + 1.0e-30));
  return out;
}

/** Mean power over [lo, hi] on a log axis, in dB. */
double bandDb(const cab::SpeakerDescription& speaker, const cab::CabSettings& settings, double lo, double hi) {
  const std::vector<double> db = responseDb(speaker, settings, logGrid(lo, hi));
  double power = 0.0;
  for (const double v : db)
    power += std::pow(10.0, v / 10.0);
  return 10.0 * std::log10(power / static_cast<double>(db.size()));
}

double peakDb(const cab::SpeakerDescription& speaker, const cab::CabSettings& settings, double lo, double hi) {
  const std::vector<double> db = responseDb(speaker, settings, logGrid(lo, hi, 96));
  return *std::max_element(db.begin(), db.end());
}

/** How far the response wanders from its own octave-wide average, 300 Hz to
    4 kHz, in dB rms: comb filtering, not tone. */
double rippleDb(const cab::SpeakerDescription& speaker, const cab::CabSettings& settings) {
  const std::vector<double> f = logGrid(200.0, 6000.0, 48);
  const std::vector<double> db = responseDb(speaker, settings, f);
  double sum = 0.0;
  int count = 0;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] < 300.0 || f[i] > 4000.0)
      continue;
    double smooth = 0.0;
    int n = 0;
    for (size_t j = 0; j < f.size(); ++j)
      if (std::abs(std::log2(f[j] / f[i])) <= 0.5) {
        smooth += db[j];
        ++n;
      }
    smooth /= n;
    sum += (db[i] - smooth) * (db[i] - smooth);
    ++count;
  }
  return std::sqrt(sum / count);
}

const cab::SpeakerDescription& shipped(int index) {
  static std::vector<std::shared_ptr<const cab::SpeakerDescription>> all = [] {
    std::vector<std::shared_ptr<const cab::SpeakerDescription>> loaded;
    for (const char* file : cabfx::kSpeakerFiles)
      loaded.push_back(cabfx::builtinSpeaker(file));
    return loaded;
  }();
  return *all[static_cast<size_t>(index)];
}

constexpr cab::CabSize kSizes[] = {cab::CabSize::OneByTwelve, cab::CabSize::TwoByTwelve, cab::CabSize::FourByTwelve,
                                   cab::CabSize::OneByTen, cab::CabSize::TwoByTen};

/** The closed box's resonance for a speaker in a cab, as the model has it. */
double closedResonanceHz(const cab::SpeakerDescription& speaker, cab::CabSize size) {
  const cab::CabGeometry& geometry = cab::cabGeometry(size);
  const double ratio = speaker.diameterInches / geometry.speakerInches;
  const double vas = speaker.complianceLitres / std::pow(ratio, 4.0);
  return speaker.resonanceHz * ratio * std::sqrt(1.0 + vas / geometry.litresPerSpeaker);
}

std::string label(int speaker, cab::CabSize size) {
  return shipped(speaker).name + " " + cab::cabSizeName(size);
}

// ---- The block --------------------------------------------------------------

/** Renders `in` through `kind`, one block at a time, sleeping between blocks
    so the builder thread can work as it would in real time. Allocations on
    this thread during processing are added to `allocations`. */
std::vector<float> renderLive(DirectKind& kind, const std::vector<float>& in, int block, long& allocations,
                              int sleepMicros = 300) {
  std::vector<float> out(in.size(), 0.0f);
  juce::AudioBuffer<float> buffer(2, block);
  for (size_t offset = 0; offset < in.size(); offset += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min(static_cast<size_t>(block), in.size() - offset));
    buffer.copyFrom(0, 0, in.data() + offset, n);
    buffer.copyFrom(1, 0, in.data() + offset, n);
    juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, n);
    {
      const juce::ScopedNoDenormals noDenormals;
      AllocationGuard guard;
      kind.processor->processInChain(view, kind.ctx);
      allocations += guard.count();
    }
    std::copy_n(view.getReadPointer(0), n, out.begin() + static_cast<std::ptrdiff_t>(offset));
    if (sleepMicros > 0)
      std::this_thread::sleep_for(std::chrono::microseconds(sleepMicros));
  }
  return out;
}

void setParam(DirectKind& kind, const char* id, double value) {
  const int index = kind.processor->findParameter(id);
  ASSERT_GE(index, 0) << id;
  kind.processor->setParameter(index, static_cast<float>(value));
}

std::vector<float> sineAt(double hz, double seconds, float amplitude) {
  return sine(seconds, hz, amplitude, kRate);
}

double relativeDb(const std::vector<float>& got, const std::vector<float>& want, size_t from, size_t to) {
  double error = 0.0, level = 0.0;
  for (size_t i = from; i < to; ++i) {
    error += (got[i] - want[i]) * static_cast<double>(got[i] - want[i]);
    level += want[i] * static_cast<double>(want[i]);
  }
  return 10.0 * std::log10(std::max(error, 1.0e-30) / std::max(level, 1.0e-30));
}

struct TempFolder {
  TempFolder()
      : folder(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("pedaleira-cab-" + juce::Uuid().toString())) {
    folder.createDirectory();
  }
  ~TempFolder() { folder.deleteRecursively(); }
  juce::File folder;
};

bool readWav(const juce::File& file, std::vector<float>& samples, double& rate, int& bits) {
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(), true));
  if (reader == nullptr || reader->numChannels != 1)
    return false;
  rate = reader->sampleRate;
  bits = static_cast<int>(reader->bitsPerSample);
  juce::AudioBuffer<float> buffer(1, static_cast<int>(reader->lengthInSamples));
  reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, false);
  samples.assign(buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples());
  return true;
}

cab::CabSettings settingsOf(double position = 35.0) {
  cab::CabSettings settings;
  settings.position = position / 100.0;
  return settings;
}

}  // namespace

// ---------------------------------------------------------------------------
// Conformance and the table
// ---------------------------------------------------------------------------

TEST(CabModelTest, ConformsInMonoStereoAndDualMono) {
  ConformanceOptions options;
  options.params = {{"speaker", 1.0}, {"micDistanceCm", 6.0}, {"room", 30.0}};
  options.stateParams = {{"speaker", 3.0},       {"cabSize", 0.0},    {"back", 1.0},
                         {"mic", 2.0},           {"micPosition", 60.0}, {"micDistanceCm", 12.0},
                         {"micAngleDeg", 20.0},  {"room", 40.0},      {"lowCutHz", 80.0},
                         {"highCutHz", 9000.0},  {"levelDb", -3.0}};
  options.latency = LatencyCheck::MatchesOnset;
  options.cpuAverageBudgetNs = 40000.0;
  options.cpuWorstBudgetNs = 400000.0;
  expectKindConforms("cab.model", options);
}

TEST(CabModelTest, IsACabWithTheDocumentedParameters) {
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kId);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->name, juce::String("Cab"));
  EXPECT_EQ(info->category, BlockCategory::Cab);
  EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade);
  EXPECT_FLOAT_EQ(info->defaultMix, 1.0f);
  EXPECT_FALSE(info->spillover);

  const char* ids[] = {"speaker", "cabSize", "back", "mic", "micPosition", "micDistanceCm",
                       "micAngleDeg", "room", "lowCutHz", "highCutHz", "levelDb"};
  ASSERT_EQ(info->params.size(), std::size(ids));
  for (size_t i = 0; i < std::size(ids); ++i)
    EXPECT_EQ(info->params[i].id, juce::String(ids[i]));

  const juce::StringArray speakers = info->params[0].choices;
  EXPECT_EQ(speakers, juce::StringArray({"Granite", "Ember", "Velvet", "Brass", "Oak", "Flint", "Custom"}));
  EXPECT_EQ(info->params[1].choices, juce::StringArray({"1x12", "2x12", "4x12", "1x10", "2x10"}));
  EXPECT_EQ(info->params[2].choices, juce::StringArray({"Closed", "Open"}));
  EXPECT_EQ(info->params[3].choices, juce::StringArray({"Dynamic", "Ribbon", "Condenser"}));
  EXPECT_FLOAT_EQ(info->params[1].defaultValue, 2.0f) << "a 4x12 by default";

  DirectKind kind(kId);
  ASSERT_TRUE(kind.valid());
  EXPECT_EQ(kind.latencySamples(), 0) << "the convolution is zero latency";
  EXPECT_NEAR(kind.tailSeconds(), cab::cabImpulseLength(kRate) / kRate, 1.0e-9);
  EXPECT_TRUE(kind.engine().takesDescription());
}

// ---------------------------------------------------------------------------
// The shipped speakers
// ---------------------------------------------------------------------------

TEST(CabModelSpeakers, EveryShippedSpeakerLoadsAndIsVoicedLikeAGuitarSpeaker) {
  std::vector<std::string> names;
  for (int i = 0; i < cabfx::kBuiltinSpeakers; ++i) {
    SCOPED_TRACE(cabfx::kSpeakerFiles[i]);
    const auto speaker = cabfx::builtinSpeaker(cabfx::kSpeakerFiles[i]);
    ASSERT_NE(speaker, nullptr) << "does not parse";
    EXPECT_FALSE(speaker->licence.empty());
    EXPECT_EQ(std::count(names.begin(), names.end(), speaker->name), 0) << "a name used twice";
    names.push_back(speaker->name);
    // A guitar speaker's resonance.
    EXPECT_GE(speaker->resonanceHz, 70.0);
    EXPECT_LE(speaker->resonanceHz, 120.0);

    for (const cab::CabSize size : {cab::CabSize::OneByTwelve, cab::CabSize::FourByTwelve, cab::CabSize::OneByTen}) {
      SCOPED_TRACE(cab::cabSizeName(size));
      cab::CabSettings settings;
      settings.size = size;
      // Strong breakup peaks between 2 and 4 kHz, over the mids.
      const double breakup = peakDb(*speaker, settings, 2000.0, 4000.0);
      EXPECT_GE(breakup - bandDb(*speaker, settings, 300.0, 1000.0), 3.0);
      // Little left above about 6 kHz.
      EXPECT_LE(bandDb(*speaker, settings, 7000.0, 9000.0), breakup - 10.0);
      EXPECT_LE(bandDb(*speaker, settings, 10000.0, 14000.0), breakup - 25.0);
    }
  }
}

TEST(CabModelSpeakers, EachSpeakerHasAVoiceOfItsOwn) {
  // Over the guitar's range, every pair differs by more than a dB and a half
  // rms (levels matched), in the default cab.
  const std::vector<double> f = logGrid(100.0, 6000.0, 12);
  std::vector<std::vector<double>> curves;
  for (int i = 0; i < cabfx::kBuiltinSpeakers; ++i) {
    std::vector<double> db = responseDb(shipped(i), cab::CabSettings{}, f);
    double mean = 0.0;
    for (const double v : db)
      mean += v / static_cast<double>(db.size());
    for (double& v : db)
      v -= mean;
    curves.push_back(db);
  }
  for (size_t a = 0; a < curves.size(); ++a)
    for (size_t b = a + 1; b < curves.size(); ++b) {
      double sum = 0.0;
      for (size_t i = 0; i < f.size(); ++i)
        sum += (curves[a][i] - curves[b][i]) * (curves[a][i] - curves[b][i]);
      EXPECT_GT(std::sqrt(sum / static_cast<double>(f.size())), 1.5)
          << shipped(static_cast<int>(a)).name << " and " << shipped(static_cast<int>(b)).name;
    }
}

TEST(CabModelSpeakers, TheLoaderNamesWhatIsWrong) {
  std::string text = cabfx::builtinSpeakerText("granite.json");
  ASSERT_FALSE(text.empty());
  cab::SpeakerDescription out;
  std::string error;
  EXPECT_TRUE(cab::parseSpeakerDescription(text, out, error)) << error;

  const auto refused = [](std::string edited, const char* from, const char* to) {
    const size_t at = edited.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    edited.replace(at, std::string(from).size(), to);
    cab::SpeakerDescription scratch;
    std::string message;
    EXPECT_FALSE(cab::parseSpeakerDescription(edited, scratch, message));
    return message;
  };
  EXPECT_NE(refused(text, "\"q\": 2.5", "\"q\": 40").find("cone.breakup[1].q"), std::string::npos);
  EXPECT_NE(refused(text, "\"resonance_hz\"", "\"resonance\"").find("thiele_small.resonance"), std::string::npos);
  EXPECT_NE(refused(text, "\"pedaleira-cab-speaker\"", "\"pedaleira-amp\"").find("format"), std::string::npos);
  EXPECT_NE(refused(text, "\"licence\": \"MIT\"", "\"licence\": \"\"").find("metadata.licence"), std::string::npos);
  EXPECT_FALSE(cab::parseSpeakerDescription("{ not json", out, error));
}

// ---------------------------------------------------------------------------
// The response follows the controls
// ---------------------------------------------------------------------------

TEST(CabModelResponse, AClosedBackHasMoreLowEndNearItsResonanceThanAnOpenOne) {
  // The box's air stiffens the cone: the resonance and its Q rise. Measured
  // from the closed box's resonance to half an octave above it, against the
  // mids, for every speaker in every cab, with the mic where it starts.
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s)
    for (const cab::CabSize size : kSizes) {
      SCOPED_TRACE(label(s, size));
      const double fc = closedResonanceHz(shipped(s), size);
      EXPECT_GE(fc, 90.0);
      EXPECT_LE(fc, 160.0);
      cab::CabSettings closed;
      closed.size = size;
      cab::CabSettings open = closed;
      open.back = cab::CabBack::Open;
      const double closedLow = bandDb(shipped(s), closed, fc, 1.5 * fc) - bandDb(shipped(s), closed, 300.0, 3000.0);
      const double openLow = bandDb(shipped(s), open, fc, 1.5 * fc) - bandDb(shipped(s), open, 300.0, 3000.0);
      EXPECT_GT(closedLow, openLow);
    }
  // And by a clear margin on the default 4x12.
  cab::CabSettings closed;
  cab::CabSettings open;
  open.back = cab::CabBack::Open;
  const double fc = closedResonanceHz(shipped(0), closed.size);
  EXPECT_GT((bandDb(shipped(0), closed, fc, 1.5 * fc) - bandDb(shipped(0), closed, 300.0, 3000.0)) -
                (bandDb(shipped(0), open, fc, 1.5 * fc) - bandDb(shipped(0), open, 300.0, 3000.0)),
            1.0);
}

TEST(CabModelResponse, AnOpenBackLosesItsDeepBassFarAway) {
  // The dipole: a metre away the rear wave, inverted, comes round the box and
  // cancels the bottom. Against the same mic up close.
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s) {
    SCOPED_TRACE(shipped(s).name);
    cab::CabSettings openNear;
    openNear.size = cab::CabSize::OneByTwelve;
    openNear.back = cab::CabBack::Open;
    openNear.mic = cab::MicType::Condenser;
    cab::CabSettings openFar = openNear;
    openFar.distanceCm = 100.0;
    cab::CabSettings closedFar = openFar;
    closedFar.back = cab::CabBack::Closed;
    cab::CabSettings closedNear = openNear;
    closedNear.back = cab::CabBack::Closed;
    const auto bass = [&](const cab::CabSettings& c) {
      return bandDb(shipped(s), c, 40.0, 80.0) - bandDb(shipped(s), c, 300.0, 3000.0);
    };
    // What going from near to far costs the open back's deep bass, against
    // what it costs the closed back's (the speaker is the same in both, so
    // this is the paths alone).
    EXPECT_GT((bass(closedFar) - bass(closedNear)) - (bass(openFar) - bass(openNear)), 0.5);
  }
}

TEST(CabModelResponse, TheCentreOfTheConeIsBrighterThanTheEdge) {
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s)
    for (const cab::CabSize size : kSizes) {
      SCOPED_TRACE(label(s, size));
      double previous = 1.0e9;
      double centre = 0.0, edge = 0.0;
      for (const double position : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        cab::CabSettings c;
        c.size = size;
        c.position = position;
        const double brightness = bandDb(shipped(s), c, 2000.0, 6000.0) - bandDb(shipped(s), c, 200.0, 1000.0);
        EXPECT_LE(brightness, previous + 0.25) << "position " << position;
        previous = brightness;
        if (position == 0.0)
          centre = brightness;
        edge = brightness;
      }
      EXPECT_GT(centre - edge, 5.0);
    }
}

TEST(CabModelResponse, DistanceRaisesTheCombRipple) {
  // The other speakers, the floor and the baffle edges come in as the mic
  // backs off, each a delayed copy: comb filtering. Measured on each speaker
  // with its cone's breakup taken out, so the ripple is the paths' alone and
  // not the speaker's own peaks.
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s) {
    cab::SpeakerDescription plain = shipped(s);
    plain.breakup.clear();
    for (const cab::CabSize size : kSizes) {
      SCOPED_TRACE(label(s, size));
      cab::CabSettings nearMic;
      nearMic.size = size;
      nearMic.room = 0.0;
      cab::CabSettings farMic = nearMic;
      farMic.distanceCm = 100.0;
      EXPECT_GT(rippleDb(plain, farMic), rippleDb(plain, nearMic) + 0.3);
    }
  }
  // Clearly on one speaker, where only the floor and the edges are there.
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s) {
    SCOPED_TRACE(shipped(s).name);
    cab::CabSettings nearMic;
    nearMic.size = cab::CabSize::OneByTwelve;
    nearMic.room = 0.0;
    cab::CabSettings farMic = nearMic;
    farMic.distanceCm = 100.0;
    EXPECT_GT(rippleDb(shipped(s), farMic), rippleDb(shipped(s), nearMic) + 0.5);
  }
}

TEST(CabModelResponse, OffAxisLosesHighs) {
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s)
    for (const cab::MicType mic : {cab::MicType::Dynamic, cab::MicType::Ribbon, cab::MicType::Condenser}) {
      SCOPED_TRACE(shipped(s).name + " " + cab::micTypeName(mic));
      double previous = 1.0e9;
      double onAxis = 0.0, offAxis = 0.0;
      for (const double angle : {0.0, 15.0, 30.0, 45.0, 60.0}) {
        cab::CabSettings c;
        c.mic = mic;
        c.angleDeg = angle;
        const double highs = bandDb(shipped(s), c, 4000.0, 8000.0) - bandDb(shipped(s), c, 500.0, 2000.0);
        EXPECT_LT(highs, previous + 1.0e-9) << "angle " << angle;
        previous = highs;
        if (angle == 0.0)
          onAxis = highs;
        if (angle == 45.0)
          offAxis = highs;
      }
      EXPECT_GT(onAxis - offAxis, 1.5);
    }
}

TEST(CabModelResponse, ARibbonIsDarkerThanACondenserAndADynamicHasItsPresencePeak) {
  for (int s = 0; s < cabfx::kBuiltinSpeakers; ++s)
    for (const cab::CabSize size : kSizes) {
      SCOPED_TRACE(label(s, size));
      cab::CabSettings ribbon, condenser, dynamic;
      ribbon.size = condenser.size = dynamic.size = size;
      ribbon.mic = cab::MicType::Ribbon;
      condenser.mic = cab::MicType::Condenser;
      const auto top = [&](const cab::CabSettings& c) {
        return bandDb(shipped(s), c, 2000.0, 8000.0) - bandDb(shipped(s), c, 300.0, 1000.0);
      };
      EXPECT_GT(top(condenser) - top(ribbon), 2.5);
      const auto presence = [&](const cab::CabSettings& c) {
        return bandDb(shipped(s), c, 4000.0, 6500.0) - bandDb(shipped(s), c, 1000.0, 2000.0);
      };
      EXPECT_GT(presence(dynamic), presence(condenser) + 2.0);
    }
}

TEST(CabModelResponse, ACloseFirstOrderMicGetsTheProximityBass) {
  double ribbonGain = 0.0, cardioidGain = 1.0e9;
  for (const cab::MicType mic : {cab::MicType::Dynamic, cab::MicType::Ribbon, cab::MicType::Condenser}) {
    SCOPED_TRACE(cab::micTypeName(mic));
    cab::CabSettings close;
    close.size = cab::CabSize::OneByTwelve;
    close.mic = mic;
    close.distanceCm = 1.0;
    close.room = 0.0;
    cab::CabSettings far = close;
    far.distanceCm = 60.0;
    const auto bass = [](const cab::CabSettings& c) {
      return bandDb(shipped(0), c, 100.0, 200.0) - bandDb(shipped(0), c, 800.0, 1600.0);
    };
    const double gain = bass(close) - bass(far);
    EXPECT_GT(gain, 2.5);
    if (mic == cab::MicType::Ribbon)
      ribbonGain = gain;
    else
      cardioidGain = std::min(cardioidGain, gain);
  }
  // The ribbon is all pressure gradient, a cardioid half: the ribbon gets the most.
  EXPECT_GT(ribbonGain, cardioidGain + 2.0);
}

TEST(CabModelResponse, TheRoomAddsEarlyReflections) {
  // The share of the response's energy that arrives more than 4 ms after it
  // starts, with no room and with the most room, for a close and a far mic.
  for (const double distance : {2.5, 50.0}) {
    SCOPED_TRACE(distance);
    const auto lateShare = [&](double room) {
      cab::CabSettings c;
      c.room = room;
      c.distanceCm = distance;
      const std::vector<float> ir = cab::cabImpulseResponse(shipped(0), c, kRate);
      double early = 0.0, late = 0.0;
      for (size_t i = 0; i < ir.size(); ++i)
        (i < static_cast<size_t>(0.004 * kRate) ? early : late) += ir[i] * static_cast<double>(ir[i]);
      return late / (early + late);
    };
    const double dry = lateShare(0.0);
    const double wet = lateShare(1.0);
    EXPECT_GT(wet, 2.5 * dry) << "no room " << dry << ", full room " << wet;
  }
}

TEST(CabModelResponse, TheImpulseResponseIsMinimumPhaseNormalisedAndFollowsTheRate) {
  cab::CabSettings settings;
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    SCOPED_TRACE(rate);
    const std::vector<float> ir = cab::cabImpulseResponse(shipped(0), settings, rate);
    ASSERT_EQ(static_cast<int>(ir.size()), cab::cabImpulseLength(rate));
    EXPECT_GE(ir.size() / rate, 0.08);
    EXPECT_LE(ir.size() / rate, 0.2);
    // Minimum phase: the energy is up front; nothing comes before the onset.
    double energy = 0.0, first2ms = 0.0;
    for (size_t i = 0; i < ir.size(); ++i) {
      energy += ir[i] * static_cast<double>(ir[i]);
      if (i < static_cast<size_t>(0.002 * rate))
        first2ms += ir[i] * static_cast<double>(ir[i]);
    }
    EXPECT_GT(first2ms / energy, 0.6);
    EXPECT_LE(onsetIndex(ir, 0.01f), static_cast<int>(rate / 48000.0) + 1) << "a minimum-phase response starts at once";
    // Faded to nothing at its end.
    EXPECT_LT(std::abs(ir.back()), 1.0e-6f);
    // The magnitude is the model's, normalised to 0 dB from 150 Hz to 3.5 kHz.
    const std::vector<double> f = logGrid(150.0, 3500.0, 24);
    const std::vector<double> measured = magnitudeResponseDb(ir, f, rate);
    const std::vector<double> model = responseDb(shipped(0), settings, f);
    double measuredPower = 0.0, modelPower = 0.0;
    for (size_t i = 0; i < f.size(); ++i) {
      measuredPower += std::pow(10.0, measured[i] / 10.0) / static_cast<double>(f.size());
      modelPower += std::pow(10.0, model[i] / 10.0) / static_cast<double>(f.size());
    }
    EXPECT_NEAR(10.0 * std::log10(measuredPower), 0.0, 0.5);
    const double offset = 10.0 * std::log10(measuredPower) - 10.0 * std::log10(modelPower);
    for (const double hz : {100.0, 300.0, 1000.0, 2500.0, 4000.0}) {
      const double m = magnitudeResponseDb(ir, {hz}, rate)[0];
      const double want = responseDb(shipped(0), settings, {hz})[0] + offset;
      EXPECT_NEAR(m, want, 0.75) << hz << " Hz";
    }
  }
}

// ---------------------------------------------------------------------------
// The block
// ---------------------------------------------------------------------------

TEST(CabModelBlock, PlaysTheResponseTheModelComputes) {
  const Params params = {{"speaker", 4.0}, {"cabSize", 1.0}, {"back", 1.0}, {"mic", 2.0},
                         {"micPosition", 20.0}, {"micDistanceCm", 8.0}, {"micAngleDeg", 10.0},
                         {"room", 25.0}, {"lowCutHz", 60.0}, {"highCutHz", 12000.0}};
  DirectKind kind(kId, params, Layout::Mono);
  ASSERT_TRUE(kind.valid());
  const std::vector<float> played = kind.render(impulse(8192));

  cab::CabSettings settings;
  settings.size = cab::CabSize::TwoByTwelve;
  settings.back = cab::CabBack::Open;
  settings.mic = cab::MicType::Condenser;
  settings.position = 0.2;
  settings.distanceCm = 8.0;
  settings.angleDeg = 10.0;
  settings.room = 0.25;
  settings.lowCutHz = 60.0;
  settings.highCutHz = 12000.0;
  const std::vector<float> computed = cab::cabImpulseResponse(shipped(4), settings, kRate);
  float worst = 0.0f;
  for (size_t i = 0; i < played.size(); ++i) {
    const float want = i < computed.size() ? computed[i] : 0.0f;
    worst = std::max(worst, std::abs(played[i] - want));
  }
  EXPECT_LT(worst, 1.0e-4f * peakOf(computed));

  // The level is a gain after the response.
  DirectKind louder(kId, {{"levelDb", 6.0}}, Layout::Mono);
  DirectKind plain(kId, {}, Layout::Mono);
  const std::vector<float> a = louder.render(impulse(4096));
  const std::vector<float> b = plain.render(impulse(4096));
  EXPECT_NEAR(peakOf(a) / peakOf(b), std::pow(10.0, 6.0 / 20.0), 1.0e-3);
}

TEST(CabModelBlock, SwapsItsResponseWithoutAllocatingOnTheAudioThread) {
  DirectKind kind(kId, {}, Layout::Stereo, 128);
  ASSERT_TRUE(kind.valid());
  const std::vector<float> in = noise(0.25, 5u, 0.2f, kRate);
  long allocations = 0;
  renderLive(kind, in, 128, allocations);

  // Every control but the level asks for a new response, one after the
  // other; each one is built on the builder thread and faded in here.
  const std::pair<const char*, double> moves[] = {
      {"speaker", 2.0}, {"cabSize", 0.0}, {"back", 1.0}, {"mic", 1.0}, {"micPosition", 80.0},
      {"micDistanceCm", 30.0}, {"micAngleDeg", 40.0}, {"room", 70.0}, {"lowCutHz", 120.0}, {"highCutHz", 5000.0}};
  for (const auto& [id, value] : moves) {
    setParam(kind, id, value);
    renderLive(kind, in, 128, allocations);
  }
  EXPECT_EQ(allocations, 0) << "the audio thread allocated while the cab swapped its response";

  // And it ends on the response the last settings ask for.
  Params finalParams;
  for (const auto& [id, value] : moves)
    finalParams.emplace_back(id, value);
  DirectKind reference(kId, finalParams, Layout::Stereo, 128);
  const std::vector<float> tailIn = noise(0.5, 6u, 0.2f, kRate);
  long ignored = 0;
  // Time for the last response to be built and faded in.
  renderLive(kind, noise(0.5, 7u, 0.2f, kRate), 128, ignored, 1000);
  const std::vector<float> got = renderLive(kind, tailIn, 128, ignored, 0);
  const std::vector<float> want = reference.render(tailIn);
  // Once the response's length has passed, both convolvers have the same input history.
  EXPECT_LT(relativeDb(got, want, static_cast<size_t>(0.1 * kRate), got.size()), -80.0);
}

TEST(CabModelBlock, ACrossfadeBetweenResponsesDoesNotClick) {
  // A low sine through a closed and then an open back, near the box's
  // resonance, where the two differ most in level and phase: a hard switch
  // from one to the other jumps by a large part of the sine; the block's swap
  // must stay a smooth blend of the two. One parameter moves, so the builder
  // cannot catch a half-made change.
  const Params before = {{"back", 0.0}};
  const Params after = {{"back", 1.0}};
  // A whole number of blocks, so every sample is rendered.
  const std::vector<float> in = sineAt(110.0, 256.0 * 560.0 / kRate, 0.3f);
  const int block = 256;

  DirectKind oldOnly(kId, before, Layout::Mono, block);
  DirectKind newOnly(kId, after, Layout::Mono, block);
  const std::vector<float> a = oldOnly.render(in);
  const std::vector<float> b = newOnly.render(in);

  DirectKind live(kId, before, Layout::Mono, block);
  std::vector<float> out(in.size(), 0.0f);
  juce::AudioBuffer<float> buffer(1, block);
  const size_t changeAt = static_cast<size_t>(0.5 * kRate);
  bool changed = false;
  for (size_t offset = 0; offset + block <= in.size(); offset += block) {
    if (!changed && offset >= changeAt) {
      for (const auto& [id, value] : after)
        setParam(live, id.toRawUTF8(), value);
      changed = true;
    }
    buffer.copyFrom(0, 0, in.data() + offset, block);
    live.processor->processInChain(buffer, live.ctx);
    std::copy_n(buffer.getReadPointer(0), block, out.begin() + static_cast<std::ptrdiff_t>(offset));
    // Real time, roughly: the builder has a few milliseconds per block.
    std::this_thread::sleep_for(std::chrono::microseconds(changed ? 2000 : 0));
  }

  // Where the swap began: the first sample that leaves the old cab's output.
  size_t start = 0;
  for (size_t i = changeAt; i < out.size(); ++i)
    if (std::abs(out[i] - a[i]) > 1.0e-4f) {
      start = i;
      break;
    }
  ASSERT_GT(start, 0u) << "the response never changed";
  const size_t fadeEnd = start + static_cast<size_t>(0.075 * kRate);
  ASSERT_LT(fadeEnd + static_cast<size_t>(0.2 * kRate), out.size());

  // A blend: between the two outputs throughout the swap.
  float outside = 0.0f;
  for (size_t i = start; i < fadeEnd; ++i) {
    const float lo = std::min(a[i], b[i]), hi = std::max(a[i], b[i]);
    outside = std::max(outside, std::max(lo - out[i], out[i] - hi));
  }
  EXPECT_LT(outside, 0.01f * peakOf(b)) << "the swap left the blend of the two cabs";

  // No step larger than either cab makes on its own.
  const float steady = std::max(maxStep(a, changeAt, out.size()), maxStep(b, changeAt, out.size()));
  const float during = maxStep(out, start - 1, fadeEnd + 1);
  EXPECT_LE(during, 1.25f * steady);
  // While a hard switch at the swap would have clicked.
  float hard = 0.0f;
  for (size_t i = start; i < start + static_cast<size_t>(kRate / 110.0); ++i)
    hard = std::max(hard, std::abs(b[i] - a[i - 1]));
  EXPECT_GT(hard, 4.0f * steady) << "the test signal cannot tell a click";

  // And after it, the new cab exactly.
  EXPECT_LT(relativeDb(out, b, fadeEnd + static_cast<size_t>(0.1 * kRate), out.size()), -80.0);
}

TEST(CabModelBlock, ExportsItsResponseAsAWavFile) {
  TempFolder temp;
  EffectRig rig(kId, {{"speaker", 1.0}, {"micDistanceCm", 10.0}});
  const juce::String blockId(rig.blockId());
  const juce::File file24 = temp.folder.getChildFile("cab-24.wav");
  const juce::var answer = rig.must("block.exportImpulse",
                                    "{\"blockId\":\"" + blockId + "\",\"file\":" +
                                        juce::JSON::toString(file24.getFullPathName()) + ",\"sampleRate\":44100}");
  EXPECT_EQ(static_cast<int>(answer["bits"]), 24);
  EXPECT_EQ(static_cast<int>(answer["samples"]), cab::cabImpulseLength(44100.0));

  cab::CabSettings settings;
  settings.distanceCm = 10.0;
  const std::vector<float> want = cab::cabImpulseResponse(shipped(1), settings, 44100.0);
  std::vector<float> got;
  double rate = 0.0;
  int bits = 0;
  ASSERT_TRUE(readWav(file24, got, rate, bits));
  EXPECT_EQ(rate, 44100.0);
  EXPECT_EQ(bits, 24);
  ASSERT_EQ(got.size(), want.size());
  // Peak at -1 dBFS, the shape the model's.
  EXPECT_NEAR(peakOf(got), std::pow(10.0, -1.0 / 20.0), 1.0e-4);
  const float gain = static_cast<float>(std::pow(10.0, static_cast<double>(answer["gainDb"]) / 20.0));
  for (size_t i = 0; i < got.size(); ++i)
    ASSERT_NEAR(got[i], want[i] * gain, 5.0e-6f) << "sample " << i;

  // 32 bit float keeps the level the block plays at.
  const juce::File file32 = temp.folder.getChildFile("cab-32.wav");
  rig.must("block.exportImpulse", "{\"blockId\":\"" + blockId + "\",\"file\":" +
                                      juce::JSON::toString(file32.getFullPathName()) + ",\"bits\":32}");
  ASSERT_TRUE(readWav(file32, got, rate, bits));
  EXPECT_EQ(rate, 48000.0);
  EXPECT_EQ(got, cab::cabImpulseResponse(shipped(1), settings, 48000.0));

  // Only a block with a response of its own.
  const juce::var gain2 = rig.must("chain.addBlock", R"({"kind":"utility.gain"})");
  const CommandResult refused = rig.commands.registry().run(
      "block.exportImpulse", juce::JSON::parse("{\"blockId\":\"" + gain2["blockId"].toString() + "\",\"file\":" +
                                               juce::JSON::toString(file32.getFullPathName()) + "}"));
  EXPECT_FALSE(refused.ok);
  EXPECT_EQ(refused.errorCode, juce::String("invalid_args"));
  const CommandResult badBits = rig.commands.registry().run(
      "block.exportImpulse", juce::JSON::parse("{\"blockId\":\"" + blockId + "\",\"file\":" +
                                               juce::JSON::toString(file32.getFullPathName()) + ",\"bits\":20}"));
  EXPECT_FALSE(badBits.ok);
}

TEST(CabModelBlock, TakesASpeakerOfItsOwnAndKeepsItInTheSavedState) {
  TempFolder temp;
  // Granite with its breakup moved down an octave and a new name.
  std::string text = cabfx::builtinSpeakerText("granite.json");
  const auto replace = [&](const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    ASSERT_NE(at, std::string::npos) << from;
    text.replace(at, from.size(), to);
  };
  replace("\"name\": \"Granite\"", "\"name\": \"Test Speaker\"");
  replace("\"frequency_hz\": 2500", "\"frequency_hz\": 1250");
  replace("\"frequency_hz\": 4000", "\"frequency_hz\": 2000");
  const juce::File file = temp.folder.getChildFile("test-speaker.json");
  ASSERT_TRUE(file.replaceWithText(juce::String(text)));

  EffectRig rig(kId);
  const juce::String blockId(rig.blockId());
  const std::vector<float> in = musicalSignal(1.0, kRate);
  rig.restart();
  const std::vector<float> granite = rig.render(in);

  rig.must("block.loadDescription", "{\"blockId\":\"" + blockId + "\",\"file\":" +
                                        juce::JSON::toString(file.getFullPathName()) + "}");
  EXPECT_DOUBLE_EQ(static_cast<double>(rig.blockState()["kindParams"]["speaker"]), 6.0) << "Custom is selected";
  rig.restart();
  const std::vector<float> custom = rig.render(in);
  EXPECT_GT(relativeDb(custom, granite, 4800, custom.size()), -20.0) << "the speaker did not change the sound";

  // Saved and restored, the block keeps its speaker.
  juce::MemoryBlock saved;
  rig.proc.getStateInformation(saved);
  EffectRig restored{juce::String()};
  restored.proc.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
  restored.restart();
  const std::vector<float> again = restored.render(in);
  EXPECT_LT(relativeDb(again, custom, 0, again.size()), -100.0);

  // A broken file is refused with the field named, and the block keeps what it had.
  replace("\"rolloff_hz\": 5600", "\"rolloff_hz\": 500");
  ASSERT_TRUE(file.replaceWithText(juce::String(text)));
  const CommandResult refused = rig.commands.registry().run(
      "block.loadDescription",
      juce::JSON::parse("{\"blockId\":\"" + blockId + "\",\"file\":" + juce::JSON::toString(file.getFullPathName()) + "}"));
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(refused.errorMessage.contains("cone.rolloff_hz")) << refused.errorMessage;
}

// ---------------------------------------------------------------------------
// An amp's default cab
// ---------------------------------------------------------------------------

TEST(CabModelAmps, EveryShippedAmpNamesAShippedSpeakerAsItsDefaultCab) {
  juce::StringArray speakerNames;
  for (int i = 0; i < cabfx::kBuiltinSpeakers; ++i)
    speakerNames.add(shipped(i).name);
  for (const char* file : {"high-gain.json", "high-gain-wdf.json", "lead-100.json", "red-forty.json",
                           "heavy-hot.json", "modern-three.json", "cell-four.json"}) {
    SCOPED_TRACE(file);
    const auto amp = ampfx::builtinAmp(file);
    ASSERT_NE(amp, nullptr);
    ASSERT_TRUE(amp->defaultCab.present);
    EXPECT_TRUE(speakerNames.contains(juce::String(amp->defaultCab.speaker))) << amp->defaultCab.speaker;
    EXPECT_TRUE(juce::StringArray({"1x12", "2x12", "4x12", "1x10", "2x10"}).contains(juce::String(amp->defaultCab.size)));
  }
}

TEST(CabModelAmps, TheDefaultCabIsOptionalAndChecked) {
  std::string text = ampfx::builtinAmpText("high-gain.json");
  const size_t at = text.find(",\n\n  \"default_cab\"");
  ASSERT_NE(at, std::string::npos);
  const std::string without = text.substr(0, at) + "\n}\n";
  amp::AmpDescription parsed;
  std::string error;
  ASSERT_TRUE(amp::parseAmpDescription(without, parsed, error)) << error;
  EXPECT_FALSE(parsed.defaultCab.present);

  ASSERT_TRUE(amp::parseAmpDescription(text, parsed, error)) << error;
  EXPECT_EQ(parsed.defaultCab.speaker, "Granite");
  EXPECT_EQ(parsed.defaultCab.size, "4x12");
  EXPECT_FLOAT_EQ(parsed.defaultCab.distanceCm, 2.5f);

  const auto refused = [&](const char* from, const char* to) {
    std::string edited = text;
    const size_t where = edited.find(from);
    EXPECT_NE(where, std::string::npos) << from;
    edited.replace(where, std::string(from).size(), to);
    amp::AmpDescription scratch;
    std::string message;
    EXPECT_FALSE(amp::parseAmpDescription(edited, scratch, message));
    return message;
  };
  EXPECT_NE(refused("\"size\": \"4x12\"", "\"size\": \"3x12\"").find("default_cab.size"), std::string::npos);
  EXPECT_NE(refused("\"distance_cm\": 2.5", "\"distance_cm\": 500").find("default_cab.distance_cm"),
            std::string::npos);
  EXPECT_NE(refused("\"mic\": \"dynamic\"", "\"microphone\": \"dynamic\"").find("default_cab.microphone"),
            std::string::npos);
}

// ---------------------------------------------------------------------------
// Cost
// ---------------------------------------------------------------------------

TEST(CabModelCost, OneBlockSteadyAndWhileItSwaps) {
  // Steady: the conformance test's measure, printed again for the report.
  const KindCost steady = measureKindCost(kId, {}, 2000);
  // While swapping: both convolvers run for the 70 ms of a swap. Measured on
  // the blocks of a swap, one after another.
  DirectKind kind(kId, {}, Layout::Stereo, 128);
  const std::vector<float> in = noise(0.1, 9u, 0.2f, kRate);
  long allocations = 0;
  renderLive(kind, in, 128, allocations, 0);
  double swapTotal = 0.0;
  int swapBlocks = 0;
  for (int round = 0; round < 6; ++round) {
    setParam(kind, "micPosition", round % 2 == 0 ? 70.0 : 20.0);
    // Wait for the builder, then time the 70 ms that follow.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    juce::AudioBuffer<float> buffer(2, 128);
    for (int b = 0; b < 30; ++b) {
      buffer.copyFrom(0, 0, in.data(), 128);
      buffer.copyFrom(1, 0, in.data(), 128);
      const juce::ScopedNoDenormals noDenormals;
      const auto begin = std::chrono::steady_clock::now();
      kind.processor->processInChain(buffer, kind.ctx);
      const auto end = std::chrono::steady_clock::now();
      swapTotal += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
      ++swapBlocks;
    }
  }
  const double swapAverage = swapTotal / swapBlocks;

  // Building one response, off the audio thread.
  const auto buildBegin = std::chrono::steady_clock::now();
  constexpr int kBuilds = 10;
  for (int i = 0; i < kBuilds; ++i) {
    cab::CabSettings settings;
    settings.position = 0.05 * i;
    (void)cab::cabImpulseResponse(shipped(0), settings, kRate);
  }
  const double buildMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildBegin).count() / kBuilds;

  std::cout << "[   CPU    ] cab.model steady: " << static_cast<long long>(steady.averageNs) << " ns per 128-sample "
            << "stereo block (" << juce::String(100.0 * steady.averageNs / 2666667.0, 2) << "% of a core); "
            << "around a swap: " << static_cast<long long>(swapAverage) << " ns ("
            << juce::String(100.0 * swapAverage / 2666667.0, 2) << "%); one response built in "
            << juce::String(buildMs, 2) << " ms off the audio thread" << std::endl;
  EXPECT_EQ(allocations, 0);
  EXPECT_GT(steady.averageNs, 0.0);
  EXPECT_LT(buildMs, 100.0);
}
