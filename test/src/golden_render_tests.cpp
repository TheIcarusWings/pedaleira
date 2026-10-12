// Golden render tests
//
// A fixed, deterministic input is rendered through representative rigs and
// compared with reference audio stored in the repo (test/files/golden/*.wav,
// 32-bit float). They are the safety net for engine refactors: the other
// suites pin properties (nulls, bit-exact schedules, contracts), these pin the
// actual sound, so a change that keeps every property but moves the output
// shows up here.
//
// Each rig is driven the way tools/render_tool.cpp (PedaleiraRender) drives the
// processor: multicore off, prepared, restored from a chain tree with model
// bytes embedded (cache-first, no network), settled, prepared again like a
// host starting transport, then processed in fixed blocks with the reported
// latency compensated (extra frames rendered, leading ones dropped).
//
// Comparison is a max absolute sample difference of 1e-4 rather than bit
// equality, since other platforms and compilers differ in the last bits (libm,
// FMA contraction). The exact result is printed so a drift is visible long
// before it reaches the tolerance.
//
// Regenerating the references (after an intentional sound change, on a
// machine whose build you trust):
//
//   PEDALEIRA_UPDATE_GOLDENS=1 build/test/DspTests_artefacts/Release/DspTests \
//       --gtest_filter='GoldenRenderTest.*'
//
// The tests then write the files instead of comparing and report as skipped.
#include "EngineCommands.h"
#include "Processor.h"
#include "chain_test_helpers.h"
#include "hosted_plugin_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using InputMode = TONE3000Processor::InputMode;
using Channels = std::vector<std::vector<float>>;

constexpr int kBlock = 128;
constexpr std::int64_t kWobbleSeed = 0x47524944;
constexpr double kMaxDiff = 1e-4;
constexpr double kMonoSeconds = 0.75;
// Shorter for the two-channel rigs, to keep their references near 150 kB.
constexpr double kStereoSeconds = 0.375;

// Peak of the generated input, -12 dBFS.
constexpr double kInputPeak = 0.25118864315095796;

// Three decaying plucked-string-like tones (a few harmonics, higher ones dying
// faster) followed by a short noise burst, peak -12 dBFS. The noise is a
// fixed-seed LCG, not a <random> distribution: those are unspecified by the
// standard and differ between standard libraries.
std::vector<float> makeGuitarishInput(double rate, double seconds) {
  const int frames = static_cast<int>(std::llround(seconds * rate));
  std::vector<double> x(static_cast<size_t>(frames), 0.0);

  const double pitches[] = {110.0, 164.81, 220.0};
  const double pluckStart[] = {0.0, 0.28, 0.56};
  for (int p = 0; p < 3; ++p) {
    const int start = static_cast<int>(std::llround(pluckStart[p] * seconds * rate));
    for (int i = start; i < frames; ++i) {
      const double t = (i - start) / rate;
      double v = 0.0;
      for (int h = 1; h <= 4; ++h)
        v += std::sin(2.0 * kPi * pitches[p] * h * t) / h * std::exp(-t * 9.0 * h);
      x[static_cast<size_t>(i)] += v;
    }
  }

  std::uint32_t lcg = 0x1234ABCDu;
  const int burstStart = static_cast<int>(std::llround(0.86 * seconds * rate));
  const int burstLen = static_cast<int>(std::llround(0.04 * seconds * rate));
  for (int i = 0; i < burstLen && burstStart + i < frames; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const double noise = static_cast<double>(lcg >> 8) / 8388608.0 - 1.0;
    x[static_cast<size_t>(burstStart + i)] += 0.8 * noise * (1.0 - static_cast<double>(i) / burstLen);
  }

  double peak = 0.0;
  for (const double v : x)
    peak = std::max(peak, std::abs(v));
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i)
    out[i] = static_cast<float>(x[i] * (kInputPeak / peak));
  return out;
}

// Right = left delayed 5 samples, scaled 0.8: a stereo source whose channels
// differ in level and phase, so channel mix-ups show.
Channels makeStereoInput(double rate, double seconds) {
  Channels in{makeGuitarishInput(rate, seconds), {}};
  in[1].assign(in[0].size(), 0.0f);
  for (size_t i = 5; i < in[0].size(); ++i)
    in[1][i] = 0.8f * in[0][i - 5];
  return in;
}

struct Rig {
  juce::ValueTree state;
  double rate = 48000.0;
  int oversample = 1;
  bool dualMono = false;
  // Extra edits applied through the public setters once the rig has loaded.
  std::function<void(ChainTestProcessor&)> afterLoad;
  // Output channels when they are not as many as the input's (a grid rig
  // with more outputs than inputs), and the scheduling the render runs with.
  int outChannels = 0;
  bool multiCore = false;
};

Channels render(const Rig& rig, const Channels& in) {
  const int inChannels = static_cast<int>(in.size());
  const int channels = rig.outChannels > 0 ? rig.outChannels : inChannels;
  const int frames = static_cast<int>(in[0].size());

  // Fixed wobble seed so a future rig with Spread/Align wobble on is
  // reproducible; none of today's rigs uses wobble, so the references are
  // unaffected. Scoped: other suites keep the random default.
  struct WobbleSeed {
    WobbleSeed() { TONE3000Processor::setDeterministicRandomForOfflineRender(kWobbleSeed); }
    ~WobbleSeed() { TONE3000Processor::clearDeterministicRandomForOfflineRender(); }
  } wobbleSeed;

  ChainTestProcessor proc;
  // Never persisted: the test must not touch the user's machine-wide setting.
  proc.setMultiCoreEnabled(rig.multiCore, /*persist=*/false);
  proc.setPlayConfigDetails(inChannels, channels, rig.rate, kBlock);
  if (rig.oversample > 1) {
    proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
    proc.parameters.getParameter("osFactor")
        ->setValueNotifyingHost((rig.oversample == 2 ? 0 : rig.oversample == 4 ? 1 : 2) / 2.0f);
  }
  proc.prepareToPlay(rig.rate, kBlock);

  proc.restoreFromTree(rig.state);
  EXPECT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";
  if (rig.dualMono) {
    proc.setInputMode(InputMode::DualMono);
    EXPECT_TRUE(waitForChainLoaded(proc)) << "engines never settled after the mode change";
  }
  if (rig.afterLoad) {
    rig.afterLoad(proc);
    EXPECT_TRUE(waitForChainLoaded(proc)) << "chain never settled after the edits";
  }

  // Prepared again with the rig in place, like a host starting transport
  // after a session load (see dual_mono_tests.cpp makeRig).
  proc.prepareToPlay(rig.rate, kBlock);
  EXPECT_TRUE(waitForChainLoaded(proc));

  const int latency = proc.getLatencySamples();
  const int total = frames + latency;
  Channels out(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(frames), 0.0f));
  juce::AudioBuffer<float> buffer(std::max(inChannels, channels), kBlock);
  juce::MidiBuffer midi;
  for (int start = 0; start < total; start += kBlock) {
    buffer.clear();
    const int fromInput = std::clamp(frames - start, 0, kBlock);
    for (int ch = 0; ch < inChannels; ++ch)
      if (fromInput > 0)
        buffer.copyFrom(ch, 0, in[static_cast<size_t>(ch)].data() + start, fromInput);

    proc.processBlock(buffer, midi);

    // Output frame n of the render is processed frame n + latency.
    for (int i = 0; i < kBlock; ++i) {
      const int dest = start + i - latency;
      if (dest < 0 || dest >= frames)
        continue;
      for (int ch = 0; ch < channels; ++ch)
        out[static_cast<size_t>(ch)][static_cast<size_t>(dest)] = buffer.getSample(ch, i);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Reference files
// ---------------------------------------------------------------------------

juce::File goldenFile(const char* name) {
  return juce::File(T3K_TEST_FILES_DIR).getChildFile("golden").getChildFile(name);
}

bool updatingGoldens() {
  const char* flag = std::getenv("PEDALEIRA_UPDATE_GOLDENS");
  return flag != nullptr && std::string(flag) == "1";
}

bool writeGolden(const juce::File& file, const Channels& audio, double rate) {
  file.getParentDirectory().createDirectory();
  file.deleteFile();  // an output stream appends to an existing file
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    return false;
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(rate)
                  .withNumChannels(static_cast<int>(audio.size()))
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  if (writer == nullptr)
    return false;
  juce::AudioBuffer<float> buffer(static_cast<int>(audio.size()),
                                  static_cast<int>(audio[0].size()));
  for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    buffer.copyFrom(ch, 0, audio[static_cast<size_t>(ch)].data(), buffer.getNumSamples());
  return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

float peakOf(const Channels& audio) {
  float peak = 0.0f;
  for (const auto& channel : audio)
    for (const float v : channel)
      peak = std::max(peak, std::abs(v));
  return peak;
}

// Renders the rig and either rewrites its reference or compares against it.
// How a render is held against its reference. A golden of its own may be
// (re)written. A promoted rig is a legacy rig converted to the grid and held
// against the LEGACY rig's reference, which it never writes. Either way the
// comparison uses the cross-platform tolerance: the file was rendered on one
// machine and is read on others, so no render is required to match it bit
// for bit (whether it did is printed). Exact equality is asserted only
// between two renders of one process (checkPromotion).
struct Check {
  bool ownReference = true;
  double tolerance = kMaxDiff;
};

void checkGolden(const char* name, const Rig& rig, const Channels& in, const Check& check = {}) {
  const Channels out = render(rig, in);

  // A reference of silence (or garbage) would pass any refactor.
  const float peak = peakOf(out);
  ASSERT_TRUE(std::isfinite(peak));
  ASSERT_GT(peak, 1e-3f) << "render is silent";

  const juce::File file = goldenFile(name);
  if (updatingGoldens() && check.ownReference) {
    ASSERT_TRUE(writeGolden(file, out, rig.rate)) << "cannot write " << file.getFullPathName();
    std::cout << "[golden] wrote " << file.getFullPathName() << " (peak " << peak << ")\n";
    GTEST_SKIP() << "PEDALEIRA_UPDATE_GOLDENS=1: wrote " << name << " instead of comparing";
  }

  ASSERT_TRUE(file.existsAsFile())
      << "missing reference " << file.getFullPathName()
      << "; generate it with PEDALEIRA_UPDATE_GOLDENS=1";
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  ASSERT_NE(reader, nullptr) << "unreadable reference " << name;
  ASSERT_EQ(static_cast<size_t>(reader->numChannels), out.size()) << "channel count differs";
  ASSERT_EQ(static_cast<size_t>(reader->lengthInSamples), out[0].size()) << "length differs";
  EXPECT_DOUBLE_EQ(reader->sampleRate, rig.rate);

  juce::AudioBuffer<float> reference(static_cast<int>(reader->numChannels),
                                     static_cast<int>(reader->lengthInSamples));
  ASSERT_TRUE(reader->read(&reference, 0, reference.getNumSamples(), 0, true, true));

  double maxDiff = 0.0;
  size_t worst = 0;
  bool bitExact = true;
  for (size_t ch = 0; ch < out.size(); ++ch)
    for (size_t i = 0; i < out[ch].size(); ++i) {
      const float ref = reference.getSample(static_cast<int>(ch), static_cast<int>(i));
      bitExact = bitExact && ref == out[ch][i];
      const double diff = std::abs(static_cast<double>(out[ch][i]) - static_cast<double>(ref));
      if (diff > maxDiff) {
        maxDiff = diff;
        worst = i;
      }
    }

  if (!bitExact)
    for (size_t ch = 0; ch < out.size(); ++ch) {
      double channelMax = 0.0;
      long first = -1;
      for (size_t i = 0; i < out[ch].size(); ++i) {
        const double diff = std::abs(static_cast<double>(out[ch][i]) -
                                     static_cast<double>(reference.getSample(static_cast<int>(ch), static_cast<int>(i))));
        if (diff > 0.0 && first < 0)
          first = static_cast<long>(i);
        channelMax = std::max(channelMax, diff);
      }
      std::cout << "  channel " << ch << ": max abs diff " << channelMax << ", first at frame "
                << first << "\n";
    }
  std::cout << (check.ownReference ? "[golden] " : "[promoted] ") << name << ": max abs diff "
            << maxDiff << (bitExact ? " (bit-exact)" : " (not bit-exact)") << ", peak " << peak
            << "\n";
  EXPECT_LE(maxDiff, check.tolerance)
      << name << " drifted from its reference; worst at frame " << worst
      << (check.ownReference ? ". If the sound change is intended, regenerate with "
                               "PEDALEIRA_UPDATE_GOLDENS=1"
                             : "");
}

// The same rig, converted to the grid once it has loaded and been edited.
Rig promoted(Rig rig) {
  const auto before = rig.afterLoad;
  rig.afterLoad = [before](ChainTestProcessor& proc) {
    if (before)
      before(proc);
    EXPECT_TRUE(waitForChainLoaded(proc));
    const juce::var converted = proc.convertRouting(true);
    EXPECT_TRUE(static_cast<bool>(converted["ok"]))
        << juce::JSON::toString(converted["reasons"]).toStdString();
    EXPECT_TRUE(proc.isGridMode());
    EXPECT_EQ(proc.validateActivePlanForTesting(), "");
  };
  return rig;
}

// A promoted rig is held to two things. The hard one, on every platform:
// rendered in this process, it is the legacy rig's render sample for sample
// (the design's claim, section 3.2). That needs no stored file, so it does
// not depend on which machine made one. And as for any golden: it is within
// the cross-platform tolerance of the stored legacy reference, which this
// never writes; whether it matched that file bit for bit is printed (it
// does on the Mac that rendered it), not asserted.
constexpr Check kPromotedAgainstFile{false, kMaxDiff};

void checkPromotion(const char* name, const Rig& legacy, const Channels& in,
                    const Rig* promotedRig = nullptr) {
  const Rig grid = promotedRig != nullptr ? *promotedRig : promoted(legacy);
  const Channels expected = render(legacy, in);
  const Channels out = render(grid, in);
  ASSERT_EQ(out.size(), expected.size());
  ASSERT_GT(peakOf(expected), 1e-3f) << "the legacy render is silent";
  double maxDiff = 0.0;
  for (size_t ch = 0; ch < out.size(); ++ch) {
    ASSERT_EQ(out[ch].size(), expected[ch].size());
    for (size_t i = 0; i < out[ch].size(); ++i)
      maxDiff = std::max(maxDiff, std::abs(static_cast<double>(out[ch][i]) -
                                           static_cast<double>(expected[ch][i])));
  }
  std::cout << "[promoted] " << name << ": against the legacy rig rendered in this process, max "
            << "abs diff " << maxDiff << (out == expected ? " (bit-exact)" : " (not bit-exact)")
            << "\n";
  EXPECT_TRUE(out == expected) << name << ": the promoted rig is not bit-identical to the legacy "
                                          "rig rendered beside it; max abs diff " << maxDiff;
  checkGolden(name, grid, in, kPromotedAgainstFile);
}

// ---------------------------------------------------------------------------
// Rigs
// ---------------------------------------------------------------------------

// [amp, cab] in one mono lane.
juce::ValueTree monoAmpCabState() {
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
  lane.appendChild(makeIrBlockTree("blk-cab", 2, 200), nullptr);
  state.appendChild(lane, nullptr);
  return state;
}

// Different amp and cab per lane, so a lane mix-up changes the sound.
juce::ValueTree stereoTwoLaneState() {
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", true, nullptr);
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeNamBlockTree("blk-amp-l", 1, 100, "a2-amp-test.nam"), nullptr);
  left.appendChild(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), nullptr);
  state.appendChild(left, nullptr);
  juce::ValueTree right("RightChainBlocks");
  right.appendChild(makeNamBlockTree("blk-amp-r", 3, 300, "a2-am-test-2.nam"), nullptr);
  right.appendChild(makeIrBlockTree("blk-cab-r", 4, 400, "cab-ir-test-2.wav"), nullptr);
  state.appendChild(right, nullptr);
  return state;
}

// The left lane is the trunk; the right lane takes its input from the trunk
// after the amp (the snapshot carries the branch, as the real state does).
juce::ValueTree branchedState() {
  juce::ValueTree state("ChainSnapshot");
  state.setProperty("stereoEnabled", true, nullptr);
  state.setProperty("branchSide", "left", nullptr);
  state.setProperty("branchAfterBlockId", "blk-amp", nullptr);
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
  left.appendChild(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), nullptr);
  state.appendChild(left, nullptr);
  juce::ValueTree right("RightChainBlocks");
  right.appendChild(makeIrBlockTree("blk-cab-r", 3, 300, "cab-ir-test-2.wav"), nullptr);
  state.appendChild(right, nullptr);
  return state;
}

const Channels& monoInput48k() {
  static const Channels in{makeGuitarishInput(48000.0, kMonoSeconds)};
  return in;
}

Rig monoRig48k() { return Rig{monoAmpCabState()}; }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(GoldenRenderTest, MonoNamIr48k) {
  checkGolden("mono_nam_ir_48k.wav", monoRig48k(), monoInput48k());
}

TEST(GoldenRenderTest, MonoNamIr44k1) {
  Rig rig = monoRig48k();
  rig.rate = 44100.0;
  checkGolden("mono_nam_ir_44k1.wav", rig, {makeGuitarishInput(44100.0, kMonoSeconds)});
}

TEST(GoldenRenderTest, StereoTwoLane) {
  checkGolden("stereo_two_lane.wav", Rig{stereoTwoLaneState()},
              makeStereoInput(48000.0, kStereoSeconds));
}

TEST(GoldenRenderTest, Branched) {
  checkGolden("branched.wav", Rig{branchedState()}, makeStereoInput(48000.0, kStereoSeconds));
}

TEST(GoldenRenderTest, DualMono) {
  Rig rig = monoRig48k();
  rig.dualMono = true;
  checkGolden("dual_mono.wav", rig, makeStereoInput(48000.0, kStereoSeconds));
}

TEST(GoldenRenderTest, MonoNamIrOversampled2x) {
  Rig rig = monoRig48k();
  rig.oversample = 2;
  checkGolden("mono_nam_ir_os2x.wav", rig, monoInput48k());
}

// Block mix, per-block and faceplate gains and a shaped EQ band, none of which
// the other rigs touch.
Rig mixGainEqRig() {
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  auto amp = makeNamBlockTree("blk-amp", 1, 100);
  amp.setProperty("mix", 0.5f, nullptr);
  amp.setProperty("inputGain", 0.62f, nullptr);
  amp.setProperty("outputGain", 0.4f, nullptr);
  lane.appendChild(amp, nullptr);
  lane.appendChild(makeIrBlockTree("blk-cab", 2, 200), nullptr);
  state.appendChild(lane, nullptr);

  Rig rig{state};
  rig.afterLoad = [](ChainTestProcessor& proc) {
    EXPECT_TRUE(proc.setBlockEqBand("blk-cab", 2, eqBandVar("bell", 1500.0, 4.5, 1.2)));
    proc.parameters.getParameter("inputLevel")->setValueNotifyingHost(0.58f);
    proc.parameters.getParameter("outputLevel")->setValueNotifyingHost(0.45f);
  };
  return rig;
}

TEST(GoldenRenderTest, MonoMixGainAndEq) {
  checkGolden("mono_mix_gain_eq.wav", mixGainEqRig(), monoInput48k());
}

// A registry kind between the two model-based ones: the amp, then
// utility.gain at +6 dB, then the cab. Pins the kind path (restore from a
// KindState node, parameters in real units) inside an ordinary rig.
juce::ValueTree gainBlockState() {
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree lane("ChainBlocks");
  lane.appendChild(makeNamBlockTree("blk-amp", 1, 100), nullptr);
  lane.appendChild(makeKindBlockTree("blk-gain", "utility.gain", "Gain",
                                     {{"gainDb", 6.0}, {"pan", 0.0}, {"phaseInvert", 0.0}}),
                   nullptr);
  lane.appendChild(makeIrBlockTree("blk-cab", 2, 200), nullptr);
  state.appendChild(lane, nullptr);
  return state;
}

TEST(GoldenRenderTest, GainBlock) {
  checkGolden("gain_block.wav", Rig{gainBlockState()}, monoInput48k());
}

// A hosted plugin between the two model-based kinds: the amp, then
// PedaleiraTestFx (the VST3 built with this suite, see test/plugins) at a fixed
// gain, then the cab. Pins the hosted path inside an ordinary rig: the
// plugin's layout and buffer adaptation, its parameter reaching the plugin,
// the base-rate run. The plugin is added by command once the rig has loaded,
// since its identity (the file's path in this build tree) is only known
// after a scan.
Rig hostedPluginRig(const juce::String& identifier) {
  Rig rig = monoRig48k();
  rig.afterLoad = [identifier](ChainTestProcessor& proc) {
    EngineCommands commands(proc);
    const auto run = [&](const char* name, const juce::var& args = {}) {
      const CommandResult result = commands.registry().run(name, args);
      EXPECT_TRUE(result.ok) << name << ": " << result.errorMessage;
      return result.value;
    };
    juce::DynamicObject::Ptr addArgs = new juce::DynamicObject();
    addArgs->setProperty("identifier", identifier);
    const juce::String pluginId = run("chain.addPlugin", juce::var(addArgs.get()))["blockId"];
    ASSERT_TRUE(pluginId.isNotEmpty());
    EXPECT_TRUE(static_cast<bool>(run("state.waitUntilLoaded")["loaded"]));

    // It landed in the first free slot, after the cab; move it between the
    // amp and the cab.
    std::vector<std::string> order{"blk-amp", pluginId.toStdString(), "blk-cab"};
    const juce::var chain = proc.getChainState(-1)["chain"];
    for (const auto& item : *chain.getArray()) {
      const std::string id = item["blockId"].toString().toStdString();
      if (std::find(order.begin(), order.end(), id) == order.end())
        order.push_back(id);
    }
    ASSERT_TRUE(proc.reorderChainBlocks(order));

    // Gain 0.7 (the parameter runs 0..2, normalised 0..1).
    juce::String gainId;
    juce::DynamicObject::Ptr blockArgs = new juce::DynamicObject();
    blockArgs->setProperty("blockId", pluginId);
    const juce::var listed = run("block.listParams", juce::var(blockArgs.get()))["params"];
    for (const auto& param : *listed.getArray())
      if (param["name"].toString() == "Gain")
        gainId = param["id"].toString();
    ASSERT_TRUE(gainId.isNotEmpty());
    ASSERT_TRUE(proc.setBlockParam(pluginId.toStdString(), gainId, 0.35));
    EXPECT_TRUE(static_cast<bool>(run("state.waitUntilLoaded")["loaded"]));
  };
  return rig;
}

TEST(GoldenRenderTest, HostedPlugin) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  checkGolden("hosted_plugin.wav", hostedPluginRig(identifier), monoInput48k());
}

// ---- Promotion -------------------------------------------------------------
// The design's central claim: a legacy rig converted to the grid is the same
// audio, bit for bit. Held against the same legacy rig rendered beside it
// (see checkPromotion), and within the usual tolerance of the committed
// legacy references, which these tests never write.

TEST(GridPromotionGoldenTest, MonoNamIr48k) {
  checkPromotion("mono_nam_ir_48k.wav", monoRig48k(), monoInput48k());
}

TEST(GridPromotionGoldenTest, MonoNamIr44k1) {
  Rig rig = monoRig48k();
  rig.rate = 44100.0;
  checkPromotion("mono_nam_ir_44k1.wav", rig, {makeGuitarishInput(44100.0, kMonoSeconds)});
}

TEST(GridPromotionGoldenTest, MonoNamIrOversampled2x) {
  Rig rig = monoRig48k();
  rig.oversample = 2;
  checkPromotion("mono_nam_ir_os2x.wav", rig, monoInput48k());
}

TEST(GridPromotionGoldenTest, MonoMixGainAndEq) {
  checkPromotion("mono_mix_gain_eq.wav", mixGainEqRig(), monoInput48k());
}

TEST(GridPromotionGoldenTest, GainBlock) {
  checkPromotion("gain_block.wav", Rig{gainBlockState()}, monoInput48k());
}

TEST(GridPromotionGoldenTest, HostedPlugin) {
  ScannerSandbox sandbox;
  const juce::String identifier = scanTestPlugin();
  checkPromotion("hosted_plugin.wav", hostedPluginRig(identifier), monoInput48k());
}

TEST(GridPromotionGoldenTest, DualMono) {
  Rig rig = monoRig48k();
  rig.dualMono = true;
  checkPromotion("dual_mono.wav", rig, makeStereoInput(48000.0, kStereoSeconds));
}

// Two lanes at their default pans (hard left, hard right), balance centred:
// the legacy image matrix is at identity and skips its loop, and so do the
// two rows' sends, so this shape is bit-identical as well.
TEST(GridPromotionGoldenTest, StereoTwoLane) {
  checkPromotion("stereo_two_lane.wav", Rig{stereoTwoLaneState()},
                 makeStereoInput(48000.0, kStereoSeconds));
}

// A branched rig: the branch becomes a split of row 0 into row 1 that never
// rejoins, A/B at unity (section 3.2). The splitter at unity copies, the two
// rows' sends at the default hard pans copy and add to silence, and the two
// pads are the two Delay operations' values, so this shape is bit-identical
// too.
TEST(GridPromotionGoldenTest, Branched) {
  Rig rig = promoted(Rig{branchedState()});
  const auto converted = rig.afterLoad;
  rig.afterLoad = [converted](ChainTestProcessor& proc) {
    converted(proc);
    const juce::var grid = proc.getGridState();
    ASSERT_EQ(grid["splits"].size(), 1);
    EXPECT_EQ(grid["splits"][0]["type"].toString(), "ab");
    EXPECT_TRUE(grid["splits"][0]["mixAt"].isVoid());
    EXPECT_TRUE(static_cast<bool>(grid["legacyProjectable"]));
    std::cout << "[plan] promoted branched rig\n" << proc.describeActivePlanForTesting();
  };
  checkPromotion("branched.wav", Rig{branchedState()}, makeStereoInput(48000.0, kStereoSeconds),
                 &rig);
}

// ---- Grid rigs --------------------------------------------------------------
// Two independent rows built by command on top of the mono amp and cab:
// the guitar row (NAM and IR, from input 1) and a second row from input 2
// with a gain and a delay.

Rig gridTwoRowRig(bool twoOutputs) {
  Rig rig = monoRig48k();
  rig.outChannels = twoOutputs ? 4 : 2;
  rig.afterLoad = [twoOutputs](ChainTestProcessor& proc) {
    EngineCommands commands(proc);
    const auto run = [&](const char* name, const char* argsJson) {
      juce::var args;
      EXPECT_TRUE(juce::JSON::parse(argsJson, args).wasOk()) << argsJson;
      const CommandResult result = commands.registry().run(name, args);
      EXPECT_TRUE(result.ok) << name << ": " << result.errorMessage;
      return result.value;
    };
    run("grid.convert", R"({"to":"grid"})");
    run("row.setInput", R"({"row":0,"source":"device","channel":1})");
    run("row.setInput", R"({"row":1,"source":"device","channel":2})");
    run("row.setFormat", R"({"row":1,"format":"mono"})");
    if (twoOutputs) {
      run("row.setOutput", R"({"row":1,"target":"device","pair":2})");
    } else {
      // Both on Out 1/2, placed apart: the guitar left of centre, the
      // second row to the right and a little down.
      run("row.setOutput", R"({"row":1,"target":"device","pair":1})");
      run("row.setMix", R"({"row":0,"pan":0.3})");
      run("row.setMix", R"({"row":1,"pan":0.8,"volumeDb":-3})");
    }
    const juce::String gain =
        run("chain.addBlock", R"({"kind":"utility.gain","row":1,"slot":0})")["blockId"];
    EXPECT_TRUE(proc.setBlockParam(gain.toStdString(), "gainDb", 4.0));
    const juce::String delay =
        run("chain.addBlock", R"({"kind":"delay.simple","row":1,"slot":1})")["blockId"];
    EXPECT_TRUE(proc.setBlockParam(delay.toStdString(), "feedback", 30.0));
    EXPECT_TRUE(static_cast<bool>(run("state.waitUntilLoaded", "{}")["loaded"]));
    EXPECT_TRUE(proc.isGridMode());
    EXPECT_EQ(proc.validateActivePlanForTesting(), "");
    std::cout << "[plan] grid, two rows, " << (twoOutputs ? "two outputs" : "one bus") << "\n"
              << proc.describeActivePlanForTesting();
  };
  return rig;
}

TEST(GridGoldenTest, TwoRowsTwoOuts) {
  checkGolden("grid_two_rows_two_outs.wav", gridTwoRowRig(true),
              makeStereoInput(48000.0, kStereoSeconds));
}

TEST(GridGoldenTest, TwoRowsOneBus) {
  checkGolden("grid_two_rows_one_bus.wav", gridTwoRowRig(false),
              makeStereoInput(48000.0, kStereoSeconds));
}

TEST(GridGoldenTest, TwoRowsTwoOuts44k1Oversampled2x) {
  Rig rig = gridTwoRowRig(true);
  rig.rate = 44100.0;
  rig.oversample = 2;
  checkGolden("grid_44k1_os2x.wav", rig, makeStereoInput(44100.0, kStereoSeconds));
}

// The root fork over the two rows (and the NAM phase forks under it at 2x)
// only schedules: side by side or one after the other, the same samples.
TEST(GridGoldenTest, SerialAndParallelAgree) {
  for (const bool oversampled : {false, true}) {
    SCOPED_TRACE(oversampled ? "44.1 kHz, 2x" : "48 kHz");
    Rig rig = gridTwoRowRig(true);
    if (oversampled) {
      rig.rate = 44100.0;
      rig.oversample = 2;
    }
    const Channels in = makeStereoInput(rig.rate, kStereoSeconds);
    const Channels serial = render(rig, in);
    rig.multiCore = true;
    const Channels parallel = render(rig, in);
    ASSERT_EQ(serial.size(), 4u);
    EXPECT_GT(peakOf({serial[0]}), 1e-3f);
    EXPECT_GT(peakOf({serial[2]}), 1e-3f);
    EXPECT_EQ(serial, parallel);
  }
}

// ---- Splits and row feeds ---------------------------------------------------
// Rigs with joins, built by command on a state that brings the two amps and
// the two cabs (the second pair in the right lane, which a mono rig keeps
// without playing it, so the conversion leaves them in row 1).

juce::ValueTree twoAmpTwoCabState() {
  juce::ValueTree state("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(makeNamBlockTree("blk-amp-a", 1, 100, "a2-amp-test.nam"), nullptr);
  left.appendChild(makeIrBlockTree("blk-cab-a", 2, 200, "cab-ir-test.wav"), nullptr);
  state.appendChild(left, nullptr);
  juce::ValueTree right("RightChainBlocks");
  right.appendChild(makeNamBlockTree("blk-amp-b", 3, 300, "a2-am-test-2.nam"), nullptr);
  right.appendChild(makeIrBlockTree("blk-cab-b", 4, 400, "cab-ir-test-2.wav"), nullptr);
  state.appendChild(right, nullptr);
  return state;
}

// A grid rig from that state: `script` is a list of commands, one per line,
// as "name {json}".
Rig gridJoinRig(const char* label, int outChannels, std::vector<std::pair<const char*, const char*>> script) {
  Rig rig{twoAmpTwoCabState()};
  rig.outChannels = outChannels;
  rig.afterLoad = [label, script](ChainTestProcessor& proc) {
    EngineCommands commands(proc);
    for (const auto& [name, argsJson] : script) {
      juce::var args;
      ASSERT_TRUE(juce::JSON::parse(argsJson, args).wasOk()) << argsJson;
      const CommandResult result = commands.registry().run(name, args);
      ASSERT_TRUE(result.ok) << name << " " << argsJson << ": " << result.errorMessage;
    }
    EXPECT_TRUE(static_cast<bool>(commands.registry().run("state.waitUntilLoaded", {}).value["loaded"]));
    EXPECT_TRUE(proc.isGridMode());
    EXPECT_EQ(proc.validateActivePlanForTesting(), "");
    EXPECT_EQ(proc.unresolvedPlanRefsForTesting(), 0);
    std::cout << "[plan] " << label << "\n" << proc.describeActivePlanForTesting();
  };
  return rig;
}

// Two amps in parallel, rejoined at -6 dB each, then the cab, a delay and a
// reverb in the row.
Rig splitAbRejoinRig() {
  return gridJoinRig(
      "two amps in parallel, rejoined", 2,
      {{"grid.convert", R"({"to":"grid"})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-a","row":0,"slot":3})"},
       {"grid.moveBlock", R"({"blockId":"blk-amp-a","row":0,"slot":1})"},
       {"grid.moveBlock", R"({"blockId":"blk-amp-b","row":1,"slot":1})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-b","row":3,"slot":7})"},
       {"split.add", R"({"row":0,"splitAt":1,"mixAt":2,"type":"ab"})"},
       {"split.setParam", R"({"row":0,"param":"mix.levelA","value":-6})"},
       {"split.setParam", R"({"row":0,"param":"mix.levelB","value":-6})"},
       {"chain.addBlock", R"({"kind":"delay.simple","row":0,"slot":4})"},
       {"chain.addBlock", R"({"kind":"reverb.room","row":0,"slot":5})"}});
}

// The amp, then a crossover at 400 Hz: the lows go on through a gain, the
// highs through the second cab and a delay in the row below, and the two
// meet again before the first cab.
Rig splitCrossoverRig() {
  return gridJoinRig(
      "crossover split", 2,
      {{"grid.convert", R"({"to":"grid"})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-a","row":0,"slot":5})"},
       {"grid.moveBlock", R"({"blockId":"blk-amp-b","row":3,"slot":7})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-b","row":1,"slot":2})"},
       {"split.add", R"({"row":0,"splitAt":1,"mixAt":4,"type":"crossover"})"},
       {"split.setParam", R"({"row":0,"param":"split.crossoverHz","value":400})"},
       {"split.setParam", R"({"row":0,"param":"mix.levelB","value":3})"},
       {"split.setParam", R"({"row":0,"param":"mix.panA","value":-0.4})"},
       {"split.setParam", R"({"row":0,"param":"mix.panB","value":0.5})"},
       {"chain.addBlock", R"({"kind":"utility.gain","row":0,"slot":2})"},
       {"chain.addBlock", R"({"kind":"delay.simple","row":1,"slot":3})"}});
}

// Three paths: the first amp, the second amp, and a clean path with a
// chorus, nested (row 0 into 1, row 1 into 2), then the cab.
Rig threeWayRig() {
  return gridJoinRig(
      "three-way nest", 2,
      {{"grid.convert", R"({"to":"grid"})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-a","row":0,"slot":6})"},
       {"grid.moveBlock", R"({"blockId":"blk-amp-a","row":0,"slot":2})"},
       {"grid.moveBlock", R"({"blockId":"blk-amp-b","row":1,"slot":3})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-b","row":3,"slot":7})"},
       {"split.add", R"({"row":0,"splitAt":1,"mixAt":5,"type":"ab"})"},
       {"split.add", R"({"row":1,"splitAt":2,"mixAt":4,"type":"ab"})"},
       {"split.setParam", R"({"row":0,"param":"mix.level","value":-8})"},
       {"split.setParam", R"({"row":1,"param":"mix.levelB","value":-10})"},
       {"split.setParam", R"({"row":1,"param":"mix.panB","value":0.6})"},
       {"chain.addBlock", R"({"kind":"modulation.chorus","row":2,"slot":3})"}});
}

// Two inputs, an amp each, fed into a third row that holds the cab and a
// delay.
Rig rowFeedRig() {
  return gridJoinRig(
      "row feed", 2,
      {{"grid.convert", R"({"to":"grid"})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-a","row":2,"slot":1})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-b","row":3,"slot":7})"},
       {"row.setInput", R"({"row":0,"source":"device","channel":1})"},
       {"row.setInput", R"({"row":1,"source":"device","channel":2})"},
       {"row.setOutput", R"({"row":2,"target":"device","pair":1})"},
       {"row.setOutput", R"({"row":0,"target":"row","toRow":2})"},
       {"row.setOutput", R"({"row":1,"target":"row","toRow":2})"},
       {"row.setMix", R"({"row":0,"volumeDb":-5})"},
       {"row.setMix", R"({"row":1,"volumeDb":-7})"},
       {"chain.addBlock", R"({"kind":"delay.simple","row":2,"slot":3})"}});
}

// The amp, then a fork that never rejoins: the first cab on Out 1/2, the
// second cab and a reverb on Out 3/4.
Rig noRejoinTwoOutsRig() {
  return gridJoinRig(
      "no rejoin, two outputs", 4,
      {{"grid.convert", R"({"to":"grid"})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-a","row":0,"slot":2})"},
       {"grid.moveBlock", R"({"blockId":"blk-amp-b","row":3,"slot":7})"},
       {"grid.moveBlock", R"({"blockId":"blk-cab-b","row":1,"slot":2})"},
       {"split.add", R"({"row":0,"splitAt":1})"},
       {"row.setOutput", R"({"row":1,"target":"device","pair":2})"},
       {"row.setMix", R"({"row":1,"volumeDb":-3,"pan":0.35})"},
       {"chain.addBlock", R"({"kind":"reverb.room","row":1,"slot":4})"}});
}

TEST(GridGoldenTest, SplitAbRejoin) {
  checkGolden("grid_split_ab_rejoin.wav", splitAbRejoinRig(), monoInput48k());
}

TEST(GridGoldenTest, SplitCrossover) {
  checkGolden("grid_split_crossover.wav", splitCrossoverRig(), monoInput48k());
}

TEST(GridGoldenTest, ThreeWay) {
  checkGolden("grid_three_way.wav", threeWayRig(), monoInput48k());
}

TEST(GridGoldenTest, RowFeed) {
  checkGolden("grid_row_feed.wav", rowFeedRig(), makeStereoInput(48000.0, kStereoSeconds));
}

TEST(GridGoldenTest, NoRejoinTwoOuts) {
  Channels in = makeStereoInput(48000.0, kStereoSeconds);
  checkGolden("grid_no_rejoin_two_outs.wav", noRejoinTwoOutsRig(), in);
}

// The split forks nest inside the root fork and the NAM phase forks inside
// those: side by side or one after the other, the same samples, at 48 kHz
// and through the boundary at 44.1 kHz with 2x oversampling.
TEST(GridGoldenTest, SplitRigsSerialAndParallelAgree) {
  struct Case {
    const char* name;
    Rig rig;
    bool stereoIn;
  };
  Case cases[] = {{"two amps rejoined", splitAbRejoinRig(), false},
                  {"crossover", splitCrossoverRig(), false},
                  {"three-way", threeWayRig(), false},
                  {"row feed", rowFeedRig(), true},
                  {"no rejoin", noRejoinTwoOutsRig(), true}};
  for (Case& c : cases)
    for (const bool oversampled : {false, true}) {
      SCOPED_TRACE(std::string(c.name) + (oversampled ? ", 44.1 kHz, 2x" : ", 48 kHz"));
      Rig rig = c.rig;
      if (oversampled) {
        rig.rate = 44100.0;
        rig.oversample = 2;
      }
      const double seconds = 0.2;
      const Channels in = c.stereoIn ? makeStereoInput(rig.rate, seconds)
                                     : Channels{makeGuitarishInput(rig.rate, seconds)};
      rig.multiCore = false;
      const Channels serial = render(rig, in);
      rig.multiCore = true;
      const Channels parallel = render(rig, in);
      EXPECT_GT(peakOf(serial), 1e-3f);
      EXPECT_EQ(serial, parallel);
    }
}

// The references only mean something if a render is repeatable: two fresh
// processors in one process must agree to the last bit.
TEST(GoldenRenderTest, RenderIsDeterministic) {
  const Channels first = render(monoRig48k(), monoInput48k());
  const Channels second = render(monoRig48k(), monoInput48k());
  EXPECT_EQ(first, second);
}

}  // namespace
