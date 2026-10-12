// Plan executor tests
//
// The chain stage runs a compiled ProcessingPlan (ProcessingPlan.h,
// ProcessorPlanExec.cpp). These tests pin what that must never change: the
// samples.
//
//   - Reference renders. The nine golden rigs (golden_render_tests.cpp)
//     cover mono, stereo lanes, a branch with the trunk on the left, dual
//     mono, 44.1 kHz and 2x. The rigs here are the shapes those leave out:
//     stereo lanes on a mono host buffer (the scratch channel), a branch
//     with the trunk on the right, latent blocks in either parallel section
//     (the alignment delays), and the stereo and branched shapes behind the
//     resampling boundary and the oversampler. Their references in
//     test/files/golden/plan were rendered by the chain stage as it was
//     before the plan existed, after the plan executor had been shown to
//     match it sample for sample on every one of them. Each is rendered
//     with multi-core off and on: a fork is scheduling only.
//   - The plan follows the rig. After every kind of structural edit the
//     active plan validates against the lanes, and no callback ever runs a
//     plan compiled for another topology.
//
// Comparison is a max absolute sample difference of 1e-4, bit-exact on the
// machine that made the references, exactly as for the golden renders.
// Regenerating (after an intentional sound change, on a build you trust):
//
//   PEDALEIRA_UPDATE_PLAN_REFS=1 build/test/DspTests_artefacts/Release/DspTests \
//       --gtest_filter='PlanExecTest.*'
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using InputMode = TONE3000Processor::InputMode;
using Channels = std::vector<std::vector<float>>;

constexpr int kBlock = 128;
constexpr std::int64_t kWobbleSeed = 0x47524944;
constexpr double kMaxDiff = 1e-4;
constexpr double kSeconds = 0.25;

// Two decaying tones and a noise burst, peak about -12 dBFS; the second
// channel is the first delayed 7 samples at 0.7, so a channel or lane mix-up
// shows. A fixed-seed LCG, not <random>: its distributions differ between
// standard libraries.
Channels makeInput(double rate, int channels) {
  const int frames = static_cast<int>(std::llround(kSeconds * rate));
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  std::uint32_t lcg = 0x0BADF00Du;
  for (int i = 0; i < frames; ++i) {
    const double t = i / rate;
    double v = 0.16 * std::sin(2.0 * kPi * 146.83 * t) * std::exp(-t * 6.0);
    if (t > 0.1)
      v += 0.12 * std::sin(2.0 * kPi * 329.63 * (t - 0.1)) * std::exp(-(t - 0.1) * 9.0);
    if (t > 0.19 && t < 0.21) {
      lcg = lcg * 1664525u + 1013904223u;
      v += 0.1 * (static_cast<double>(lcg >> 8) / 8388608.0 - 1.0);
    }
    x[static_cast<size_t>(i)] = static_cast<float>(v);
  }
  Channels in{x};
  if (channels > 1) {
    in.push_back(std::vector<float>(x.size(), 0.0f));
    for (size_t i = 7; i < x.size(); ++i)
      in[1][i] = 0.7f * x[i - 7];
  }
  return in;
}

struct Shape {
  juce::ValueTree state;
  int channels = 2;
  bool dualMono = false;
};

struct Config {
  double rate = 48000.0;
  int oversample = 1;
  bool multiCore = false;
};

// Drives a rig the way the golden renders do: prepared, restored with model
// bytes embedded, settled, prepared again like a host starting transport,
// then processed in fixed blocks with the reported latency compensated.
Channels render(const Shape& shape, const Config& config) {
  struct WobbleSeed {
    WobbleSeed() { TONE3000Processor::setDeterministicRandomForOfflineRender(kWobbleSeed); }
    ~WobbleSeed() { TONE3000Processor::clearDeterministicRandomForOfflineRender(); }
  } wobbleSeed;

  const Channels in = makeInput(config.rate, shape.channels);
  const int channels = shape.channels;
  const int frames = static_cast<int>(in[0].size());

  ChainTestProcessor proc;
  // Never persisted: the test must not touch the user's machine-wide setting.
  proc.setMultiCoreEnabled(config.multiCore, /*persist=*/false);
  proc.setPlayConfigDetails(channels, channels, config.rate, kBlock);
  if (config.oversample > 1) {
    proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
    proc.parameters.getParameter("osFactor")
        ->setValueNotifyingHost((config.oversample == 2 ? 0 : config.oversample == 4 ? 1 : 2) /
                                2.0f);
  }
  proc.prepareToPlay(config.rate, kBlock);

  proc.restoreFromTree(shape.state);
  EXPECT_TRUE(waitForChainLoaded(proc)) << "blocks never finished loading from cache";
  if (shape.dualMono) {
    proc.setInputMode(InputMode::DualMono);
    EXPECT_TRUE(waitForChainLoaded(proc)) << "engines never settled after the mode change";
  }
  proc.prepareToPlay(config.rate, kBlock);
  EXPECT_TRUE(waitForChainLoaded(proc));
  EXPECT_EQ(proc.validateActivePlanForTesting(), "") << proc.describeActivePlanForTesting();

  const int latency = proc.getLatencySamples();
  const int total = frames + latency;
  Channels out(static_cast<size_t>(channels),
               std::vector<float>(static_cast<size_t>(frames), 0.0f));
  juce::AudioBuffer<float> buffer(channels, kBlock);
  juce::MidiBuffer midi;
  for (int start = 0; start < total; start += kBlock) {
    buffer.clear();
    const int fromInput = std::clamp(frames - start, 0, kBlock);
    for (int ch = 0; ch < channels; ++ch)
      if (fromInput > 0)
        buffer.copyFrom(ch, 0, in[static_cast<size_t>(ch)].data() + start, fromInput);

    proc.processBlock(buffer, midi);

    for (int i = 0; i < kBlock; ++i) {
      const int dest = start + i - latency;
      if (dest < 0 || dest >= frames)
        continue;
      for (int ch = 0; ch < channels; ++ch)
        out[static_cast<size_t>(ch)][static_cast<size_t>(dest)] = buffer.getSample(ch, i);
    }
  }
  EXPECT_EQ(proc.stalePlanCallbacksForTesting(), 0);
  return out;
}

float peakOf(const Channels& audio) {
  float peak = 0.0f;
  for (const auto& channel : audio)
    for (const float v : channel)
      peak = std::max(peak, std::abs(v));
  return peak;
}

// ---------------------------------------------------------------------------
// Rigs
// ---------------------------------------------------------------------------

juce::ValueTree withMix(juce::ValueTree block, float mix) {
  block.setProperty("mix", mix, nullptr);
  return block;
}

// test.latency (test_block_kinds.h): delays its wet path by `samples` and
// reports it. At a mix below 100% the dry share has to be delayed with it.
juce::ValueTree latentBlock(const juce::String& id, int samples, float mix = 1.0f) {
  return withMix(makeKindBlockTree(id, "test.latency", "Latency",
                                   {{"latencySamples", static_cast<double>(samples)},
                                    {"tailMs", 0.0},
                                    {"fail", 0.0}}),
                 mix);
}

juce::ValueTree lanes(const std::vector<juce::ValueTree>& left,
                      const std::vector<juce::ValueTree>& right, bool stereo) {
  juce::ValueTree state("ChainSnapshot");
  if (stereo)
    state.setProperty("stereoEnabled", true, nullptr);
  juce::ValueTree leftLane("ChainBlocks");
  for (const auto& block : left)
    leftLane.appendChild(block, nullptr);
  state.appendChild(leftLane, nullptr);
  juce::ValueTree rightLane("RightChainBlocks");
  for (const auto& block : right)
    rightLane.appendChild(block, nullptr);
  state.appendChild(rightLane, nullptr);
  return state;
}

juce::ValueTree branched(juce::ValueTree state, const char* trunkSide, const char* afterBlockId) {
  state.setProperty("branchSide", trunkSide, nullptr);
  state.setProperty("branchAfterBlockId", afterBlockId, nullptr);
  return state;
}

// Different amp and cab per lane and sub-unity mixes, so a lane or scratch
// mix-up changes the sound.
juce::ValueTree stereoState() {
  return lanes({makeNamBlockTree("blk-amp-l", 1, 100, "a2-amp-test.nam"),
                withMix(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), 0.7f)},
               {makeNamBlockTree("blk-amp-r", 3, 300, "a2-am-test-2.nam"),
                withMix(makeIrBlockTree("blk-cab-r", 4, 400, "cab-ir-test-2.wav"), 0.4f)},
               true);
}

// The right lane is the trunk; the left lane takes the trunk's signal after
// the amp.
juce::ValueTree branchedTrunkRightState() {
  return branched(lanes({withMix(makeIrBlockTree("blk-cab-l", 3, 300, "cab-ir-test-2.wav"), 0.8f)},
                        {makeNamBlockTree("blk-amp", 1, 100),
                         withMix(makeIrBlockTree("blk-cab-r", 2, 200, "cab-ir-test.wav"), 0.6f)},
                        true),
                  "right", "blk-amp");
}

juce::ValueTree branchedTrunkLeftState() {
  return branched(lanes({makeNamBlockTree("blk-amp", 1, 100),
                         withMix(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), 0.6f)},
                        {withMix(makeIrBlockTree("blk-cab-r", 3, 300, "cab-ir-test-2.wav"), 0.8f)},
                        true),
                  "left", "blk-amp");
}

// Latency in the left lane only (48 samples, half wet): the right lane has
// to wait for it.
juce::ValueTree stereoLatentState() {
  return lanes({makeNamBlockTree("blk-amp-l", 1, 100), latentBlock("blk-lat-l", 48, 0.5f),
                withMix(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), 0.7f)},
               {withMix(makeIrBlockTree("blk-cab-r", 4, 400, "cab-ir-test-2.wav"), 0.4f)}, true);
}

// Latency before the tap (common to both paths), in the trunk's remainder
// and, more of it, in the branch lane: the trunk's remainder waits.
juce::ValueTree branchedLatentState() {
  return branched(
      lanes({latentBlock("blk-lat-pre", 16), makeNamBlockTree("blk-amp", 1, 100),
             latentBlock("blk-lat-trunk", 40, 0.5f),
             withMix(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), 0.6f)},
            {latentBlock("blk-lat-branch", 96, 0.75f),
             withMix(makeIrBlockTree("blk-cab-r", 3, 300, "cab-ir-test-2.wav"), 0.8f)},
            true),
      "left", "blk-amp");
}

// ---------------------------------------------------------------------------
// Reference files
// ---------------------------------------------------------------------------

juce::File referenceFile(const char* name) {
  return juce::File(T3K_TEST_FILES_DIR)
      .getChildFile("golden")
      .getChildFile("plan")
      .getChildFile(name);
}

bool updatingReferences() {
  const char* flag = std::getenv("PEDALEIRA_UPDATE_PLAN_REFS");
  return flag != nullptr && std::string(flag) == "1";
}

bool writeReference(const juce::File& file, const Channels& audio, double rate) {
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

void compareWithReference(const char* name, const char* schedule, const Channels& out,
                          double rate) {
  const juce::File file = referenceFile(name);
  ASSERT_TRUE(file.existsAsFile())
      << "missing reference " << file.getFullPathName()
      << "; generate it with PEDALEIRA_UPDATE_PLAN_REFS=1";
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  ASSERT_NE(reader, nullptr) << "unreadable reference " << name;
  ASSERT_EQ(static_cast<size_t>(reader->numChannels), out.size()) << "channel count differs";
  ASSERT_EQ(static_cast<size_t>(reader->lengthInSamples), out[0].size()) << "length differs";
  EXPECT_DOUBLE_EQ(reader->sampleRate, rate);

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
  std::cout << "[plan-ref] " << name << " (" << schedule << "): max abs diff " << maxDiff
            << (bitExact ? " (bit-exact)" : " (not bit-exact)") << "\n";
  EXPECT_LE(maxDiff, kMaxDiff) << name << " (" << schedule
                               << ") drifted from its reference; worst at frame " << worst;
}

// Renders the rig on one core and forked, and compares both with the stored
// reference (or rewrites it from the single-core render).
void checkReference(const char* name, const Shape& shape, Config config) {
  config.multiCore = false;
  const Channels serial = render(shape, config);
  const float peak = peakOf(serial);
  ASSERT_TRUE(std::isfinite(peak));
  ASSERT_GT(peak, 1e-3f) << "render is silent";

  if (updatingReferences()) {
    const juce::File file = referenceFile(name);
    ASSERT_TRUE(writeReference(file, serial, config.rate))
        << "cannot write " << file.getFullPathName();
    std::cout << "[plan-ref] wrote " << file.getFullPathName() << " (peak " << peak << ")\n";
    GTEST_SKIP() << "PEDALEIRA_UPDATE_PLAN_REFS=1: wrote " << name << " instead of comparing";
  }

  compareWithReference(name, "one core", serial, config.rate);
  config.multiCore = true;
  compareWithReference(name, "multi-core", render(shape, config), config.rate);
}

// ---------------------------------------------------------------------------
// Reference renders
// ---------------------------------------------------------------------------

// A mono host buffer under stereo chains: the right lane runs on the scratch
// channel and the image stage folds it back.
TEST(PlanExecTest, StereoLanesOnAMonoHostBuffer) {
  checkReference("stereo_mono_host.wav", Shape{stereoState(), 1}, {});
}

TEST(PlanExecTest, BranchedWithTheTrunkOnTheRight) {
  checkReference("branched_trunk_right.wav", Shape{branchedTrunkRightState()}, {});
}

TEST(PlanExecTest, StereoLanesAroundALatentBlock) {
  checkReference("stereo_latent.wav", Shape{stereoLatentState()}, {});
}

TEST(PlanExecTest, BranchedAroundLatentBlocks) {
  checkReference("branched_latent.wav", Shape{branchedLatentState()}, {});
}

// Behind the resampling boundary and the oversampler at once: the chain
// stage is then called with other frame counts than the host's.
TEST(PlanExecTest, StereoLanesAt44k1Oversampled2x) {
  checkReference("stereo_44k1_os2x.wav", Shape{stereoState()}, {44100.0, 2});
}

TEST(PlanExecTest, BranchedOversampled8x) {
  checkReference("branched_os8x.wav", Shape{branchedTrunkLeftState()}, {48000.0, 8});
}

// ---------------------------------------------------------------------------
// The plan follows the rig
// ---------------------------------------------------------------------------

// One callback, then: the plan it ran matched the lanes, and the plan in
// place now holds against them.
void expectPlanCurrent(ChainTestProcessor& proc, const char* after) {
  juce::AudioBuffer<float> buffer(2, kBlock);
  juce::MidiBuffer midi;
  buffer.clear();
  proc.processBlock(buffer, midi);
  EXPECT_EQ(proc.stalePlanCallbacksForTesting(), 0) << "after " << after;
  EXPECT_EQ(proc.validateActivePlanForTesting(), "")
      << "after " << after << "\n"
      << proc.describeActivePlanForTesting();
  // Rows 2 and 3 exist and stay empty: no blocks, no insert placeholders.
  EXPECT_NE(proc.describeActivePlanForTesting().find(", 0, 0)"), std::string::npos)
      << "after " << after << "\n"
      << proc.describeActivePlanForTesting();
}

std::vector<std::string> laneIds(const TONE3000Processor& proc, const char* key) {
  std::vector<std::string> ids;
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state[key].getArray())
    for (const auto& item : *lane)
      ids.push_back(item["blockId"].toString().toStdString());
  return ids;
}

TEST(PlanExecTest, EveryStructuralEditLeavesAMatchingPlan) {
  ChainTestProcessor proc;
  proc.setMultiCoreEnabled(false, /*persist=*/false);
  proc.setPlayConfigDetails(2, 2, 48000.0, kBlock);
  proc.prepareToPlay(48000.0, kBlock);
  expectPlanCurrent(proc, "construction");

  // No audio between the edits below, so their fades do not wait.
  const auto idle = [] { letAudioGoIdle(); };
  const auto addGain = [&proc] {
    const juce::var added = proc.addKindBlock("utility.gain");
    EXPECT_TRUE(added["error"].toString().isEmpty()) << added["error"].toString();
    return added["blockId"].toString().toStdString();
  };

  idle();
  const std::string first = addGain();
  expectPlanCurrent(proc, "add");
  idle();
  const std::string latent = proc.addKindBlock("test.latency")["blockId"].toString().toStdString();
  ASSERT_FALSE(latent.empty());
  expectPlanCurrent(proc, "add a latent block");

  // Fill the lane past its baseline, so it grows and its ranges move.
  for (int i = 0; i < 5; ++i) {
    idle();
    addGain();
  }
  expectPlanCurrent(proc, "grow the lane");

  idle();
  proc.setStereoMode(true);
  expectPlanCurrent(proc, "stereo on");

  idle();
  proc.setActiveEditChain("right");
  const std::string rightBlock = addGain();
  expectPlanCurrent(proc, "add to the right lane");

  idle();
  EXPECT_TRUE(proc.setChainBranch("left", first));
  expectPlanCurrent(proc, "branch");
  EXPECT_NE(proc.describeActivePlanForTesting().find("Copy"), std::string::npos);

  idle();
  EXPECT_TRUE(proc.setChainBranch("left", latent));
  expectPlanCurrent(proc, "re-point the branch");

  idle();
  EXPECT_TRUE(proc.moveBlockToChain(rightBlock, "left", 0));
  expectPlanCurrent(proc, "move across lanes");

  idle();
  EXPECT_FALSE(proc.duplicateChainBlock(first, "right", 0).empty());
  expectPlanCurrent(proc, "duplicate");

  idle();
  auto order = laneIds(proc, "chain");
  std::reverse(order.begin(), order.end());
  EXPECT_TRUE(proc.reorderChainBlocks(order));
  expectPlanCurrent(proc, "reorder");

  idle();
  EXPECT_TRUE(proc.swapChains());
  expectPlanCurrent(proc, "swap chains");

  idle();
  EXPECT_TRUE(proc.removeChainBlock(latent));
  expectPlanCurrent(proc, "remove the tapped block");
  EXPECT_EQ(proc.describeActivePlanForTesting().find("Copy"), std::string::npos)
      << "the branch should have gone with its tap";

  idle();
  proc.setStereoMode(false);
  expectPlanCurrent(proc, "stereo off");

  for (int i = 0; i < 6; ++i) {
    idle();
    if (!proc.undoChain())
      break;
    expectPlanCurrent(proc, "undo");
  }
  for (int i = 0; i < 3; ++i) {
    idle();
    if (!proc.redoChain())
      break;
    expectPlanCurrent(proc, "redo");
  }

  idle();
  proc.restoreFromTree(branchedTrunkRightState());
  EXPECT_TRUE(waitForChainLoaded(proc));
  expectPlanCurrent(proc, "state restore");

  idle();
  EXPECT_TRUE(proc.resetToDefault());
  EXPECT_TRUE(waitForChainLoaded(proc));
  expectPlanCurrent(proc, "reset to default");
}

// What the three legacy shapes compile to on a real processor (the pure
// compiler has its own tests in plan_compile_tests.cpp): the plan is built
// from the lanes as restored, inserts and all.
TEST(PlanExecTest, RestoredRigsCompileToTheirShape) {
  const auto planOf = [](const juce::ValueTree& state) {
    ChainTestProcessor proc;
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPlayConfigDetails(2, 2, 48000.0, kBlock);
    proc.prepareToPlay(48000.0, kBlock);
    proc.restoreFromTree(state);
    EXPECT_TRUE(waitForChainLoaded(proc));
    EXPECT_EQ(proc.validateActivePlanForTesting(), "");
    return proc.describeActivePlanForTesting();
  };

  const std::string mono = planOf(lanes(
      {makeNamBlockTree("blk-amp", 1, 100), makeIrBlockTree("blk-cab", 2, 200)}, {}, false));
  std::cout << "[plan] mono rig\n" << mono;
  EXPECT_NE(mono.find("RunBlocks row 0 [0, 5)"), std::string::npos) << mono;
  EXPECT_NE(mono.find("slots per row (5, 5, 0, 0)"), std::string::npos) << mono;
  EXPECT_EQ(mono.find("Fork"), std::string::npos) << mono;
  EXPECT_EQ(mono.find("row 1"), std::string::npos) << mono;

  const std::string stereo = planOf(stereoState());
  std::cout << "[plan] stereo rig\n" << stereo;
  EXPECT_NE(stereo.find("Fork"), std::string::npos) << stereo;
  EXPECT_NE(stereo.find("RunBlocks row 0 [0, 5) on port0 ch0"), std::string::npos) << stereo;
  EXPECT_NE(stereo.find("RunBlocks row 1 [0, 5) on port0 ch1"), std::string::npos) << stereo;

  const std::string branch = planOf(branchedLatentState());
  std::cout << "[plan] branched rig\n" << branch;
  EXPECT_NE(branch.find("RunBlocks row 0 [0, 2) on port0 ch0"), std::string::npos) << branch;
  EXPECT_NE(branch.find("Copy port0 ch0 to port0 ch1"), std::string::npos) << branch;
  EXPECT_NE(branch.find("RunBlocks row 0 [2, 5) on port0 ch0"), std::string::npos) << branch;
  // 16 before the tap, then 40 against 96: the trunk's remainder waits 56.
  EXPECT_NE(branch.find("Delay row 0 on port0 ch0 by 56"), std::string::npos) << branch;
  EXPECT_NE(branch.find("Delay row 1 on port0 ch1 by 0"), std::string::npos) << branch;
  EXPECT_NE(branch.find("chain latency 112"), std::string::npos) << branch;
}

// ---------------------------------------------------------------------------
// Saved state
// ---------------------------------------------------------------------------
// Four lanes, the grid's types and the plan are all runtime structure: a
// legacy rig saves exactly the bytes it always did. (The shape of the block
// nodes themselves is pinned by BlockProcessorTest.StateRoundTripKeepsTheSavedShape.)

juce::MemoryBlock savedBytes(TONE3000Processor& proc) {
  juce::MemoryBlock saved;
  proc.getStateInformation(saved);
  return saved;
}

juce::ValueTree parseSaved(const juce::MemoryBlock& saved) {
  // "T3KB", then a binary ValueTree stream.
  if (saved.getSize() <= 4 || std::string(static_cast<const char*>(saved.getData()), 4) != "T3KB")
    return {};
  return juce::ValueTree::readFromData(static_cast<const char*>(saved.getData()) + 4,
                                       saved.getSize() - 4);
}

std::vector<std::string> childTypesOf(const juce::ValueTree& node) {
  std::vector<std::string> types;
  for (int i = 0; i < node.getNumChildren(); ++i)
    types.push_back(node.getChild(i).getType().toString().toStdString());
  return types;
}

std::vector<std::string> propertyNamesOf(const juce::ValueTree& node) {
  std::vector<std::string> names;
  for (int i = 0; i < node.getNumProperties(); ++i)
    names.push_back(node.getPropertyName(i).toString().toStdString());
  return names;
}

bool hasNodeNamed(const juce::ValueTree& node, const char* type) {
  if (node.hasType(type))
    return true;
  for (int i = 0; i < node.getNumChildren(); ++i)
    if (hasNodeNamed(node.getChild(i), type))
      return true;
  return false;
}

TEST(LegacyStateTest, SavedBytesSurviveARoundTripUnchanged) {
  struct Case {
    const char* name;
    juce::ValueTree state;
    bool dualMono;
  };
  const Case cases[] = {
      {"mono",
       lanes({makeNamBlockTree("blk-amp", 1, 100), makeIrBlockTree("blk-cab", 2, 200)}, {}, false),
       false},
      {"dual mono",
       lanes({makeNamBlockTree("blk-amp", 1, 100), makeIrBlockTree("blk-cab", 2, 200)}, {}, false),
       true},
      {"stereo", stereoState(), false},
      {"branched, trunk left, latent kinds", branchedLatentState(), false},
      {"branched, trunk right", branchedTrunkRightState(), false},
  };

  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const auto open = [](ChainTestProcessor& proc) {
      proc.setMultiCoreEnabled(false, /*persist=*/false);
      proc.setPlayConfigDetails(2, 2, 48000.0, kBlock);
      proc.prepareToPlay(48000.0, kBlock);
    };

    ChainTestProcessor first;
    open(first);
    first.restoreFromTree(c.state);
    ASSERT_TRUE(waitForChainLoaded(first));
    if (c.dualMono) {
      first.setInputMode(InputMode::DualMono);
      ASSERT_TRUE(waitForChainLoaded(first));
    }
    const juce::MemoryBlock saved = savedBytes(first);

    // Saving twice in a row gives the same bytes: nothing in the state
    // depends on when it is taken.
    EXPECT_TRUE(saved == savedBytes(first));

    // The saved shape: schema 1, the two legacy lanes and nothing of the
    // grid. Rows 2 and 3 are never written.
    const juce::ValueTree tree = parseSaved(saved);
    ASSERT_TRUE(tree.isValid());
    EXPECT_TRUE(tree.hasType("TONE3000State"));
    EXPECT_EQ(static_cast<int>(tree.getProperty("schemaVersion")), 1);
    EXPECT_FALSE(hasNodeNamed(tree, "Grid"));
    EXPECT_FALSE(hasNodeNamed(tree, "Row"));
    EXPECT_FALSE(hasNodeNamed(tree, "Split"));
    const juce::ValueTree snapshot = tree.getChildWithName("ChainSnapshot");
    ASSERT_TRUE(snapshot.isValid());
    EXPECT_EQ(propertyNamesOf(snapshot),
              (std::vector<std::string>{"stereoEnabled", "branchSide", "branchAfterBlockId"}));
    EXPECT_EQ(childTypesOf(snapshot),
              (std::vector<std::string>{"ChainBlocks", "RightChainBlocks"}));
    EXPECT_EQ(childTypesOf(tree),
              (std::vector<std::string>{"PARAMETERS", "MidiMappings", "ChainSnapshot"}))
        << "a new top-level node";

    // Through a restore into a fresh processor (every lane rebuilt, the
    // plan compiled, the engines loaded) and out again: the same bytes.
    ChainTestProcessor second;
    open(second);
    second.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_TRUE(waitForChainLoaded(second));
    const juce::MemoryBlock again = savedBytes(second);
    EXPECT_EQ(saved.getSize(), again.getSize());
    EXPECT_TRUE(saved == again) << "the state changed across save, restore, save";
    EXPECT_EQ(second.validateActivePlanForTesting(), "");

    // And once more over the same processor, which reconciles against the
    // lanes it already has.
    second.setStateInformation(again.getData(), static_cast<int>(again.getSize()));
    ASSERT_TRUE(waitForChainLoaded(second));
    EXPECT_TRUE(saved == savedBytes(second));
  }
}

}  // namespace
