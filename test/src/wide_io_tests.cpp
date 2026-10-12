// Wide I/O tests: input strips, output buses and more than two device
// channels.
//
// processBlock is the input strips, the chain stage behind the multi-port
// boundary and the per-port oversamplers, and the output strips. A legacy
// rig has one strip and one bus, the buffer's first two channels, and none
// of this may change a sample of it. Pinned here:
//
//   - which main-bus layouts each wrapper type takes;
//   - a legacy rig on a wide buffer is the stereo render on channels 0 and 1,
//     bit for bit, and silence on every other channel, at every host rate
//     and oversampling factor (the reference renders in golden_render_tests
//     and plan_exec_tests pin the stereo render itself);
//   - the boundary and oversampler wiring against the single-port container
//     and ChainOversampler::process it replaced;
//   - the chain-edit fade gives every channel the same per-sample gain;
//   - the render tool with wide input files and --out-channels.
#include "ChainBoundaryMulti.h"
#include "ChainDomain.h"
#include "ChainOversampler.h"
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using InputMode = TONE3000Processor::InputMode;
using Channels = std::vector<std::vector<float>>;
using Wrapper = juce::AudioProcessor::WrapperType;

constexpr int kBlock = 128;
constexpr std::int64_t kWobbleSeed = 0x47524944;

// Channel 0: two decaying tones and a noise burst, peak about -12 dBFS.
// Channel 1: channel 0 delayed 7 samples at 0.7, so a channel mix-up shows.
// Every further channel: its own noise at about -14 dBFS, which a legacy rig
// has to ignore. A fixed-seed LCG, not <random>: its distributions differ
// between standard libraries.
Channels makeInput(double rate, int channels, double seconds) {
  const int frames = static_cast<int>(std::llround(seconds * rate));
  std::vector<float> x(static_cast<size_t>(frames), 0.0f);
  std::uint32_t lcg = 0x0BADF00Du;
  for (int i = 0; i < frames; ++i) {
    const double t = i / rate;
    double v = 0.16 * std::sin(2.0 * kPi * 146.83 * t) * std::exp(-t * 6.0);
    if (t > 0.05)
      v += 0.12 * std::sin(2.0 * kPi * 329.63 * (t - 0.05)) * std::exp(-(t - 0.05) * 9.0);
    if (t > 0.08 && t < 0.1) {
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
  for (int ch = 2; ch < channels; ++ch) {
    std::vector<float> noise(x.size());
    std::uint32_t seed = 0xC0FFEE00u + static_cast<std::uint32_t>(ch);
    for (float& sample : noise) {
      seed = seed * 1664525u + 1013904223u;
      sample = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }
    in.push_back(std::move(noise));
  }
  return in;
}

juce::ValueTree withMix(juce::ValueTree block, float mix) {
  block.setProperty("mix", mix, nullptr);
  return block;
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

// The legacy rig shapes: one mono chain, the same in dual mono, two lanes,
// and a branch. Sub-unity mixes and a different amp and cab per lane, so a
// lane or channel mix-up changes the sound.
struct Shape {
  const char* name;
  juce::ValueTree state;
  bool dualMono = false;
};

Shape monoShape() {
  return {"mono",
          lanes({makeNamBlockTree("blk-amp", 1, 100),
                 withMix(makeIrBlockTree("blk-cab", 2, 200, "cab-ir-test.wav"), 0.7f)},
                {}, false)};
}

Shape dualMonoShape() {
  Shape shape = monoShape();
  shape.name = "dual mono";
  shape.dualMono = true;
  return shape;
}

Shape stereoShape() {
  return {"stereo lanes",
          lanes({makeNamBlockTree("blk-amp-l", 1, 100, "a2-amp-test.nam"),
                 withMix(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), 0.7f)},
                {makeNamBlockTree("blk-amp-r", 3, 300, "a2-am-test-2.nam"),
                 withMix(makeIrBlockTree("blk-cab-r", 4, 400, "cab-ir-test-2.wav"), 0.4f)},
                true)};
}

Shape branchedShape() {
  juce::ValueTree state =
      lanes({makeNamBlockTree("blk-amp", 1, 100),
             withMix(makeIrBlockTree("blk-cab-l", 2, 200, "cab-ir-test.wav"), 0.6f)},
            {withMix(makeIrBlockTree("blk-cab-r", 3, 300, "cab-ir-test-2.wav"), 0.8f)}, true);
  state.setProperty("branchSide", "left", nullptr);
  state.setProperty("branchAfterBlockId", "blk-amp", nullptr);
  return {"branched", state};
}

struct Config {
  double rate = 48000.0;
  int oversample = 1;
  int ins = 2;
  int outs = 2;
  // Frames per processBlock call; the processor is always prepared for
  // kBlock, so a larger value is a host exceeding its promise.
  int callFrames = kBlock;
};

struct Rendered {
  Channels out;  // every buffer channel, latency compensated
  int latency = 0;
  int bufferChannels = 0;
};

void setOversampling(TONE3000Processor& proc, int factor) {
  if (factor <= 1)
    return;
  proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
  proc.parameters.getParameter("osFactor")
      ->setValueNotifyingHost((factor == 2 ? 0 : factor == 4 ? 1 : 2) / 2.0f);
}

// Drives a rig the way the golden renders do (prepared, restored with model
// bytes embedded, settled, prepared again like a host starting transport)
// on a buffer of max(ins, outs) channels. `in` is what the buffer carries on
// entry, one entry per buffer channel: also the channels that are no input
// of the layout, where a legacy rig must not care what it finds.
Rendered render(const Shape& shape, const Config& config, const Channels& in) {
  struct WobbleSeed {
    WobbleSeed() { TONE3000Processor::setDeterministicRandomForOfflineRender(kWobbleSeed); }
    ~WobbleSeed() { TONE3000Processor::clearDeterministicRandomForOfflineRender(); }
  } wobbleSeed;

  Rendered result;
  const int channels = std::max(config.ins, config.outs);
  result.bufferChannels = channels;
  EXPECT_EQ(static_cast<int>(in.size()), channels);
  const int frames = static_cast<int>(in[0].size());

  ChainTestProcessor proc;
  proc.setMultiCoreEnabled(false, /*persist=*/false);
  proc.setPlayConfigDetails(config.ins, config.outs, config.rate, kBlock);
  EXPECT_EQ(proc.getMainBusNumInputChannels(), config.ins);
  EXPECT_EQ(proc.getMainBusNumOutputChannels(), config.outs);
  setOversampling(proc, config.oversample);
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

  result.latency = proc.getLatencySamples();
  const int total = frames + result.latency;
  result.out.assign(static_cast<size_t>(channels),
                    std::vector<float>(static_cast<size_t>(frames), 0.0f));
  juce::AudioBuffer<float> buffer(channels, config.callFrames);
  juce::MidiBuffer midi;
  for (int start = 0; start < total; start += config.callFrames) {
    buffer.clear();
    const int fromInput = std::clamp(frames - start, 0, config.callFrames);
    for (int ch = 0; ch < channels; ++ch)
      if (fromInput > 0)
        buffer.copyFrom(ch, 0, in[static_cast<size_t>(ch)].data() + start, fromInput);

    proc.processBlock(buffer, midi);

    for (int i = 0; i < config.callFrames; ++i) {
      const int dest = start + i - result.latency;
      if (dest < 0 || dest >= frames)
        continue;
      for (int ch = 0; ch < channels; ++ch)
        result.out[static_cast<size_t>(ch)][static_cast<size_t>(dest)] = buffer.getSample(ch, i);
    }
  }
  EXPECT_EQ(proc.stalePlanCallbacksForTesting(), 0);
  return result;
}

float peakOf(const std::vector<float>& channel) {
  float peak = 0.0f;
  for (const float v : channel)
    peak = std::max(peak, std::abs(v));
  return peak;
}

// Bit for bit: every sample compares equal, and the first that does not is
// named.
void expectSameSamples(const std::vector<float>& actual, const std::vector<float>& expected,
                       const std::string& what) {
  ASSERT_EQ(actual.size(), expected.size()) << what;
  for (size_t i = 0; i < actual.size(); ++i)
    if (actual[i] != expected[i]) {
      ADD_FAILURE() << what << ": differs at frame " << i << " (" << actual[i] << " against "
                    << expected[i] << ")";
      return;
    }
}

// Channels 0 and 1 of the wide render are the stereo render and every other
// channel is silent.
void expectStereoRenderOnAWideBuffer(const Rendered& wide, const Rendered& stereo,
                                     const std::string& what) {
  EXPECT_EQ(wide.latency, stereo.latency) << what;
  ASSERT_GE(wide.out.size(), 2u) << what;
  ASSERT_EQ(stereo.out.size(), 2u) << what;
  EXPECT_GT(peakOf(stereo.out[0]), 1.0e-3f) << what << ": the stereo render is silent";
  expectSameSamples(wide.out[0], stereo.out[0], what + ", channel 0");
  expectSameSamples(wide.out[1], stereo.out[1], what + ", channel 1");
  for (size_t ch = 2; ch < wide.out.size(); ++ch)
    EXPECT_EQ(peakOf(wide.out[ch]), 0.0f) << what << ": channel " << ch << " is not silent";
}

std::string label(const Shape& shape, const Config& config) {
  return std::string(shape.name) + " " + std::to_string(config.ins) + " in / " +
         std::to_string(config.outs) + " out at " + std::to_string(static_cast<int>(config.rate)) +
         " Hz x" + std::to_string(config.oversample);
}

// The first `count` entries of `in`.
Channels firstChannels(const Channels& in, int count) {
  return Channels(in.begin(), in.begin() + count);
}

juce::AudioProcessor::BusesLayout layoutOf(int ins, int outs) {
  juce::AudioProcessor::BusesLayout layout;
  layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(ins));
  layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(outs));
  return layout;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

TEST(WideIoTest, MainBusLayoutsPerWrapperType) {
  // The standalone app and a processor outside any wrapper: 1 to 8 inputs
  // and 1 to 8 outputs, each on its own.
  for (const Wrapper wrapper : {Wrapper::wrapperType_Standalone, Wrapper::wrapperType_Undefined})
    for (int ins = 0; ins <= 10; ++ins)
      for (int outs = 0; outs <= 10; ++outs)
        EXPECT_EQ(TONE3000Processor::isMainBusLayoutSupported(wrapper, layoutOf(ins, outs)),
                  ins >= 1 && ins <= 8 && outs >= 1 && outs <= 8)
            << "wrapper " << wrapper << ", " << ins << " in, " << outs << " out";

  // Every plugin format: mono or stereo, as many inputs as outputs. That is
  // the rule they always had.
  for (const Wrapper wrapper :
       {Wrapper::wrapperType_VST, Wrapper::wrapperType_VST3, Wrapper::wrapperType_AudioUnit,
        Wrapper::wrapperType_AudioUnitv3, Wrapper::wrapperType_AAX, Wrapper::wrapperType_Unity,
        Wrapper::wrapperType_LV2})
    for (int ins = 0; ins <= 10; ++ins)
      for (int outs = 0; outs <= 10; ++outs)
        EXPECT_EQ(TONE3000Processor::isMainBusLayoutSupported(wrapper, layoutOf(ins, outs)),
                  ins == outs && (ins == 1 || ins == 2))
            << "wrapper " << wrapper << ", " << ins << " in, " << outs << " out";

  // Not a channel count alone for a plugin: a stereo-sized set that is not
  // stereo is refused, as before.
  juce::AudioProcessor::BusesLayout discretePair;
  discretePair.inputBuses.add(juce::AudioChannelSet::discreteChannels(2));
  discretePair.outputBuses.add(juce::AudioChannelSet::discreteChannels(2));
  EXPECT_FALSE(
      TONE3000Processor::isMainBusLayoutSupported(Wrapper::wrapperType_VST3, discretePair));
  EXPECT_TRUE(
      TONE3000Processor::isMainBusLayoutSupported(Wrapper::wrapperType_Standalone, discretePair));
}

TEST(WideIoTest, TheProcessorAppliesItsOwnWrapperTypesRule) {
  // A processor made here has no wrapper: wide.
  {
    TONE3000Processor proc;
    EXPECT_EQ(proc.wrapperType, Wrapper::wrapperType_Undefined);
    EXPECT_TRUE(proc.checkBusesLayoutSupported(layoutOf(8, 8)));
    EXPECT_TRUE(proc.checkBusesLayoutSupported(layoutOf(1, 6)));
    EXPECT_FALSE(proc.checkBusesLayoutSupported(layoutOf(9, 2)));
  }
  // The wrapper type is fixed at construction, from the type JUCE's plugin
  // clients announce for the next processor.
  struct NextWrapper {
    explicit NextWrapper(Wrapper type) { juce::AudioProcessor::setTypeOfNextNewPlugin(type); }
    ~NextWrapper() {
      juce::AudioProcessor::setTypeOfNextNewPlugin(Wrapper::wrapperType_Undefined);
    }
  };
  {
    NextWrapper next(Wrapper::wrapperType_VST3);
    TONE3000Processor proc;
    EXPECT_EQ(proc.wrapperType, Wrapper::wrapperType_VST3);
    EXPECT_TRUE(proc.checkBusesLayoutSupported(layoutOf(2, 2)));
    EXPECT_TRUE(proc.checkBusesLayoutSupported(layoutOf(1, 1)));
    EXPECT_FALSE(proc.checkBusesLayoutSupported(layoutOf(1, 2)));
    EXPECT_FALSE(proc.checkBusesLayoutSupported(layoutOf(2, 1)));
    EXPECT_FALSE(proc.checkBusesLayoutSupported(layoutOf(8, 8)));
  }
  {
    NextWrapper next(Wrapper::wrapperType_Standalone);
    TONE3000Processor proc;
    EXPECT_EQ(proc.wrapperType, Wrapper::wrapperType_Standalone);
    EXPECT_TRUE(proc.checkBusesLayoutSupported(layoutOf(8, 2)));
    EXPECT_TRUE(proc.checkBusesLayoutSupported(layoutOf(2, 8)));
  }
  TONE3000Processor after;
  EXPECT_EQ(after.wrapperType, Wrapper::wrapperType_Undefined);
}

TEST(WideIoTest, WithoutADeviceBufferChannelsAreMainBusChannels) {
  TONE3000Processor proc;
  proc.setPlayConfigDetails(6, 3, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  const TONE3000Processor::DeviceChannelMap map = proc.getDeviceChannelMap();
  EXPECT_FALSE(map.fromDevice);
  EXPECT_EQ(map.numInputs, 6);
  EXPECT_EQ(map.numOutputs, 3);
  EXPECT_EQ(map.inputNames.size(), 6);
  EXPECT_EQ(map.outputNames.size(), 3);
  for (int ch = 0; ch < kMaxIoChannels; ++ch) {
    EXPECT_EQ(map.inputPhysical[static_cast<size_t>(ch)], ch < 6 ? ch : -1);
    EXPECT_EQ(map.outputPhysical[static_cast<size_t>(ch)], ch < 3 ? ch : -1);
    EXPECT_EQ(map.inputBufferChannel(ch), ch < 6 ? ch : -1);
    EXPECT_EQ(map.outputBufferChannel(ch), ch < 3 ? ch : -1);
  }
  EXPECT_EQ(map.inputBufferChannel(-1), -1);

  // A legacy rig is the main pair on both sides whatever the width.
  const std::string plan = proc.describeActivePlanForTesting();
  EXPECT_NE(plan.find("strip 0: the host buffer's main pair in place"), std::string::npos) << plan;
  EXPECT_NE(plan.find("bus 0: the host buffer's main pair in place, main output strip"),
            std::string::npos)
      << plan;
  EXPECT_EQ(plan.find("strip 1"), std::string::npos) << plan;
  EXPECT_EQ(plan.find("bus 1"), std::string::npos) << plan;
}

// ---------------------------------------------------------------------------
// A legacy rig on a wide buffer
// ---------------------------------------------------------------------------

// Every legacy shape with 8 inputs and 8 outputs, 2 and 8, 8 and 2: the
// stereo render on channels 0 and 1, silence elsewhere. The six extra
// channels carry noise on the way in.
TEST(WideIoTest, LegacyRigOnAWideBufferIsTheStereoRender) {
  const Channels in = makeInput(48000.0, 8, 0.2);
  for (const Shape& shape : {monoShape(), dualMonoShape(), stereoShape(), branchedShape()}) {
    const Rendered stereo = render(shape, Config{}, firstChannels(in, 2));
    for (const auto& [ins, outs] : {std::pair{8, 8}, std::pair{2, 8}, std::pair{8, 2}}) {
      Config config;
      config.ins = ins;
      config.outs = outs;
      const Rendered wide = render(shape, config, in);
      EXPECT_EQ(wide.bufferChannels, 8);
      expectStereoRenderOnAWideBuffer(wide, stereo, label(shape, config));
    }
  }
}

// One input, two outputs: the buffer's second channel is no input, whatever
// it carries, and the main strip mirrors the first onto it. That is the
// stereo render of the same signal on both inputs.
TEST(WideIoTest, OneInputTwoOutputsIsTheSourceOnBothInputs) {
  const Channels in = makeInput(48000.0, 3, 0.2);
  // The channel that is no input carries noise on the way in.
  const Channels monoSourceBuffer{in[0], in[2]};
  const Channels sameOnBoth{in[0], in[0]};
  for (const Shape& shape : {monoShape(), stereoShape(), branchedShape()}) {
    const Rendered stereo = render(shape, Config{}, sameOnBoth);
    Config config;
    config.ins = 1;
    config.outs = 2;
    const Rendered mono = render(shape, config, monoSourceBuffer);
    EXPECT_EQ(mono.bufferChannels, 2);
    expectStereoRenderOnAWideBuffer(mono, stereo, label(shape, config));
  }
}

// The multi-port boundary (host rates off 48 kHz) and the per-port
// oversamplers, at every combination: wide and stereo agree to the bit, and
// so does the latency reported. The stereo renders at 44.1 kHz, at 2x and
// at 8x are themselves pinned by the reference files.
TEST(WideIoTest, WideMatchesStereoBehindTheBoundaryAndTheOversamplers) {
  for (const double rate : {44100.0, 96000.0})
    for (const int oversample : {1, 2, 8}) {
      const Channels in = makeInput(rate, 8, 0.12);
      for (const Shape& shape : {stereoShape(), branchedShape()}) {
        Config config;
        config.rate = rate;
        config.oversample = oversample;
        const Rendered stereo = render(shape, config, firstChannels(in, 2));
        EXPECT_GT(stereo.latency, 0) << "the boundary reports its delay";
        config.ins = 8;
        config.outs = 8;
        const Rendered wide = render(shape, config, in);
        expectStereoRenderOnAWideBuffer(wide, stereo, label(shape, config));
      }
    }
}

// A host that hands over more frames than it promised: the chain stage runs
// in prepared-size slices, on a wide buffer as on a stereo one.
TEST(WideIoTest, OversizedCallbacksOnAWideBuffer) {
  const Shape shape = stereoShape();
  for (const double rate : {48000.0, 44100.0}) {
    const Channels in = makeInput(rate, 8, 0.12);
    Config config;
    config.rate = rate;
    config.callFrames = 3 * kBlock + 17;
    const Rendered stereo = render(shape, config, firstChannels(in, 2));
    config.ins = 8;
    config.outs = 8;
    const Rendered wide = render(shape, config, in);
    expectStereoRenderOnAWideBuffer(wide, stereo, label(shape, config) + ", oversized callbacks");
  }
}

// ---------------------------------------------------------------------------
// The boundary and the oversamplers as the processor wires them
// ---------------------------------------------------------------------------

// The processor used to run the chain stage inside one stereo
// ResamplingContainer and one ChainOversampler::process call. It now runs
// one up-port and one down-port of ChainBoundaryMulti with the oversampler's
// two halves around the chain stage. With an empty rig and every host-rate
// stage that is not exactly neutral switched off, the processor's output is
// that round trip followed by the DC blocker, so the old path can be built
// here from the same parts and compared sample for sample.
TEST(WideIoTest, PortWiseBoundaryAndOversamplerMatchTheSinglePortPath) {
  constexpr int block = 256;
  constexpr int blocks = 40;
  for (const double rate : {44100.0, 96000.0})
    for (const int factor : {1, 2, 8}) {
      const std::string what =
          std::to_string(static_cast<int>(rate)) + " Hz x" + std::to_string(factor);
      const Channels in = makeInput(rate, 8, static_cast<double>(block * blocks) / rate);
      ASSERT_GE(static_cast<int>(in[0].size()), block * blocks - 1);
      const int frames = static_cast<int>(in[0].size()) / block * block;

      // The old path. The chain function is the empty chain: the stage moves
      // its input to its output and runs nothing.
      Channels expected(2, std::vector<float>(static_cast<size_t>(frames), 0.0f));
      {
        // As in processBlock: the first samples out of the resamplers are
        // small enough to be denormal otherwise.
        juce::ScopedNoDenormals noDenormals;
        ChainBoundaryResampler boundary(kChainBaseSampleRate);
        boundary.Reset(rate, block);
        ChainOversampler oversampler;
        const int baseBlock = std::max(
            block, static_cast<int>(std::ceil(block * kChainBaseSampleRate / rate)));
        oversampler.prepare(factor, baseBlock);
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                       juce::dsp::IIR::Coefficients<float>>
            dcBlocker;
        dcBlocker.prepare({rate, static_cast<juce::uint32>(block), 2});
        *dcBlocker.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass(rate, 5.0f);
        dcBlocker.reset();

        const ChainBoundaryResampler::BlockProcessFunc stage = [&](float** ins, float** outs,
                                                                   int numFrames) {
          oversampler.process(ins, outs, numFrames,
                              [](float** chainIns, float** chainOuts, int chainFrames) {
                                for (int ch = 0; ch < 2; ++ch)
                                  if (chainOuts[ch] != chainIns[ch])
                                    std::memcpy(chainOuts[ch], chainIns[ch],
                                                sizeof(float) * static_cast<size_t>(chainFrames));
                              });
        };
        juce::AudioBuffer<float> buffer(2, block);
        for (int start = 0; start < frames; start += block) {
          // The same source on both channels, as the processor below gets it.
          buffer.copyFrom(0, 0, in[0].data() + start, block);
          buffer.copyFrom(1, 0, in[0].data() + start, block);
          float* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
          boundary.ProcessBlock(channels, channels, block, stage);
          juce::dsp::AudioBlock<float> audio(buffer);
          juce::dsp::ProcessContextReplacing<float> context(audio);
          dcBlocker.process(context);
          for (int ch = 0; ch < 2; ++ch)
            std::copy_n(buffer.getReadPointer(ch), block,
                        expected[static_cast<size_t>(ch)].begin() + start);
        }
      }
      EXPECT_GT(peakOf(expected[0]), 1.0e-3f) << what;

      for (const int width : {2, 8}) {
        TONE3000Processor proc;
        proc.setMultiCoreEnabled(false, /*persist=*/false);
        proc.setPlayConfigDetails(width, width, rate, block);
        // Off: the gate (it is not linear) and the tone stack (flat is not
        // bit-transparent). Input and output level at unity multiply by
        // exactly one, and the same source on both inputs folds to itself.
        proc.parameters.getParameter("gateEnabled")->setValueNotifyingHost(0.0f);
        proc.parameters.getParameter("toneEqEnabled")->setValueNotifyingHost(0.0f);
        setOversampling(proc, factor);
        proc.prepareToPlay(rate, block);
        EXPECT_EQ(proc.getLatencySamples(),
                  ChainBoundaryMulti::measureLatency(rate, kChainBaseSampleRate, block))
            << what;

        juce::AudioBuffer<float> buffer(width, block);
        juce::MidiBuffer midi;
        Channels out(2, std::vector<float>(static_cast<size_t>(frames), 0.0f));
        for (int start = 0; start < frames; start += block) {
          buffer.copyFrom(0, 0, in[0].data() + start, block);
          buffer.copyFrom(1, 0, in[0].data() + start, block);
          for (int ch = 2; ch < width; ++ch)
            buffer.copyFrom(ch, 0, in[static_cast<size_t>(ch)].data() + start, block);
          proc.processBlock(buffer, midi);
          for (int ch = 0; ch < 2; ++ch)
            std::copy_n(buffer.getReadPointer(ch), block,
                        out[static_cast<size_t>(ch)].begin() + start);
          for (int ch = 2; ch < width; ++ch)
            ASSERT_EQ(buffer.getMagnitude(ch, 0, block), 0.0f) << what << ", channel " << ch;
        }
        const std::string wide = what + ", " + std::to_string(width) + " channels";
        expectSameSamples(out[0], expected[0], wide + ", channel 0");
        expectSameSamples(out[1], expected[1], wide + ", channel 1");
      }
    }
}

// ---------------------------------------------------------------------------
// The chain-edit fade
// ---------------------------------------------------------------------------

// Structural edits under running audio on an 8-channel buffer. The fade gain
// is advanced once per callback and every channel is multiplied by the same
// values, so with the same source on both inputs the two outputs stay equal
// to the bit through every glide down, hold and glide back; a channel that
// got the gain of another callback, or none, would differ. The other six
// channels stay silent throughout.
TEST(WideIoTest, ChainEditFadeGivesEveryChannelTheSameGains) {
  constexpr int block = 256;
  ChainTestProcessor proc;
  proc.setMultiCoreEnabled(false, /*persist=*/false);
  proc.setPlayConfigDetails(8, 8, kFs, block);
  proc.parameters.getParameter("gateEnabled")->setValueNotifyingHost(0.0f);
  proc.prepareToPlay(kFs, block);

  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0}, loudExtraChannels{0}, silentBlocks{0}, glidingBlocks{0},
      fullBlocks{0};
  std::thread pump([&] {
    // 250 Hz loops seamlessly over 93 blocks of 256 (124 whole cycles).
    const int loopFrames = 93 * block;
    const auto sine = makeSine(loopFrames, 250.0, 0.1f);
    const Channels noise = makeInput(kFs, 8, static_cast<double>(loopFrames) / kFs);
    juce::AudioBuffer<float> buffer(8, block);
    juce::MidiBuffer midi;
    int offset = 0;
    while (!stop.load()) {
      buffer.copyFrom(0, 0, sine.data() + offset, block);
      buffer.copyFrom(1, 0, sine.data() + offset, block);
      for (int ch = 2; ch < 8; ++ch)
        buffer.copyFrom(ch, 0, noise[static_cast<size_t>(ch)].data() + offset, block);
      offset = (offset + block) % loopFrames;
      proc.processBlock(buffer, midi);

      const float* left = buffer.getReadPointer(0);
      const float* right = buffer.getReadPointer(1);
      float peak = 0.0f;
      for (int i = 0; i < block; ++i) {
        if (left[i] != right[i])
          mismatches.fetch_add(1);
        peak = std::max(peak, std::abs(left[i]));
      }
      for (int ch = 2; ch < 8; ++ch)
        if (buffer.getMagnitude(ch, 0, block) != 0.0f)
          loudExtraChannels.fetch_add(1);
      // A block holds at least one whole cycle, so its peak is the gain.
      if (peak == 0.0f)
        silentBlocks.fetch_add(1);
      else if (peak < 0.08f)
        glidingBlocks.fetch_add(1);
      else
        fullBlocks.fetch_add(1);
      // A few times real time keeps the mutex uncontended. While an edit
      // waits for silence the pump runs flat out: the edit's wait is
      // bounded, and a slow machine could otherwise let it run out before
      // the glide lands.
      if (!proc.isChainEditFadeHeld())
        juce::Thread::sleep(1);
    }
  });
  const auto settle = [&proc](int ms) {
    juce::Thread::sleep(ms);
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (proc.isChainEditFadeHeld() && juce::Time::getMillisecondCounter() < deadline)
      juce::Thread::sleep(10);
    juce::Thread::sleep(ms);
  };

  settle(100);
  // Each of these splices the chain inside a chain-edit fade.
  const juce::var added = proc.addKindBlock("utility.gain");
  const std::string id = added["blockId"].toString().toStdString();
  ASSERT_FALSE(id.empty()) << added["error"].toString().toStdString();
  settle(100);
  ASSERT_TRUE(proc.removeChainBlock(id));
  settle(100);
  ASSERT_TRUE(proc.undoChain());
  settle(150);
  ASSERT_TRUE(proc.redoChain());
  settle(150);

  stop.store(true);
  pump.join();

  EXPECT_GT(fullBlocks.load(), 0) << "the rig was never heard";
  EXPECT_GT(glidingBlocks.load(), 0) << "no fade was caught gliding";
  EXPECT_GT(silentBlocks.load(), 0) << "no fade was caught holding silence";
  EXPECT_EQ(mismatches.load(), 0) << "the two channels of the main bus were faded differently";
  EXPECT_EQ(loudExtraChannels.load(), 0) << "a channel past the main pair was not silent";
}

// ---------------------------------------------------------------------------
// The render tool
// ---------------------------------------------------------------------------

namespace {

struct ScratchDir {
  ScratchDir()
      : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("t3k-wide-io-" + juce::Uuid().toString())) {
    dir.createDirectory();
  }
  ~ScratchDir() { dir.deleteRecursively(); }
  juce::File dir;
};

bool writeWav(const juce::File& file, const Channels& audio, double rate) {
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

Channels readWav(const juce::File& file) {
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      file.existsAsFile() ? wav.createReaderFor(file.createInputStream().release(), true)
                          : nullptr);
  if (reader == nullptr)
    return {};
  juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels),
                                  static_cast<int>(reader->lengthInSamples));
  reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
  Channels audio;
  for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    audio.emplace_back(buffer.getReadPointer(ch),
                       buffer.getReadPointer(ch) + buffer.getNumSamples());
  return audio;
}

// Runs PedaleiraRender on a rig built by `script` and returns its exit code,
// or -1 if it could not be run.
int runRenderTool(const juce::File& script, const juce::File& input, const juce::File& output,
                  const juce::StringArray& extra = {}) {
  juce::StringArray command{T3K_RENDER_TOOL_FILE,        "--run", script.getFullPathName(), "--in",
                            input.getFullPathName(), "--out", output.getFullPathName()};
  command.addArray(extra);
  juce::ChildProcess tool;
  if (!tool.start(command, 0) || !tool.waitForProcessToFinish(120000))
    return -1;
  return static_cast<int>(tool.getExitCode());
}

}  // namespace

// A command script can build a grid rig with several inputs and outputs: a
// stereo file in, four channels out, the second row on Out 3/4.
TEST(RenderToolTest, ScriptsBuildGridRigs) {
  ASSERT_TRUE(juce::File(T3K_RENDER_TOOL_FILE).existsAsFile())
      << "PedaleiraRender was not built at " << T3K_RENDER_TOOL_FILE;
  ScratchDir scratch;
  const juce::File script = scratch.dir.getChildFile("grid.json");
  ASSERT_TRUE(script.replaceWithText(R"([
    {"command": "chain.addBlock", "args": {"kind": "utility.gain"}},
    {"command": "grid.convert", "args": {"to": "grid"}},
    {"command": "row.setInput", "args": {"row": 0, "source": "device", "channel": 1}},
    {"command": "row.setInput", "args": {"row": 1, "source": "device", "channel": 2}},
    {"command": "row.setOutput", "args": {"row": 1, "target": "device", "pair": 2}},
    {"command": "row.setMix", "args": {"row": 1, "invert": true}},
    {"command": "chain.addBlock", "args": {"kind": "utility.gain", "row": 1, "slot": 0}},
    {"command": "state.waitUntilLoaded"}
  ])"));

  Channels in = firstChannels(makeInput(48000.0, 8, 0.1), 2);
  const juce::File input = scratch.dir.getChildFile("in2.wav");
  ASSERT_TRUE(writeWav(input, in, 48000.0));
  const juce::File output = scratch.dir.getChildFile("out4.wav");
  const juce::File stateFile = scratch.dir.getChildFile("state.json");
  ASSERT_EQ(runRenderTool(script, input, output,
                          {"--out-channels", "4", "--state-out", stateFile.getFullPathName()}),
            0);
  const Channels out = readWav(output);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_GT(peakOf(out[0]), 1.0e-3f);
  EXPECT_GT(peakOf(out[2]), 1.0e-3f) << "row 1 on Out 3/4";
  EXPECT_NE(out[0], out[2]) << "two inputs, two rows";

  const juce::var state = juce::JSON::parse(stateFile.loadFileAsString());
  EXPECT_EQ(state["grid"]["mode"].toString(), "grid");
  EXPECT_EQ(static_cast<int>(state["grid"]["rows"][1]["output"]["pair"]), 2);
  EXPECT_TRUE(static_cast<bool>(state["grid"]["rows"][1]["routed"]));

  // Without the extra outputs the same script still renders: the pair is
  // not there, so row 1 is heard on Out 1/2.
  const juce::File folded = scratch.dir.getChildFile("out2.wav");
  ASSERT_EQ(runRenderTool(script, input, folded), 0);
  ASSERT_EQ(readWav(folded).size(), 2u);
}

TEST(RenderToolTest, WideInputFilesAndOutChannels) {
  ASSERT_TRUE(juce::File(T3K_RENDER_TOOL_FILE).existsAsFile())
      << "PedaleiraRender was not built at " << T3K_RENDER_TOOL_FILE;
  ScratchDir scratch;
  const juce::File script = scratch.dir.getChildFile("rig.json");
  ASSERT_TRUE(script.replaceWithText(
      R"([{"command": "chain.addBlock", "args": {"kind": "utility.gain"}}])"));

  const Channels in = makeInput(48000.0, 8, 0.1);
  const juce::File monoIn = scratch.dir.getChildFile("in1.wav");
  const juce::File stereoIn = scratch.dir.getChildFile("in2.wav");
  const juce::File wideIn = scratch.dir.getChildFile("in8.wav");
  ASSERT_TRUE(writeWav(monoIn, firstChannels(in, 1), 48000.0));
  ASSERT_TRUE(writeWav(stereoIn, firstChannels(in, 2), 48000.0));
  ASSERT_TRUE(writeWav(wideIn, in, 48000.0));

  // The invocation that always worked: stereo in, stereo out.
  const juce::File stereoOut = scratch.dir.getChildFile("out2.wav");
  ASSERT_EQ(runRenderTool(script, stereoIn, stereoOut), 0);
  const Channels stereo = readWav(stereoOut);
  ASSERT_EQ(stereo.size(), 2u);
  ASSERT_EQ(stereo[0].size(), in[0].size());
  EXPECT_GT(peakOf(stereo[0]), 1.0e-3f);

  const auto expectStereoOnWide = [&](const juce::File& file, const std::string& what) {
    const Channels wide = readWav(file);
    ASSERT_EQ(wide.size(), 8u) << what;
    expectSameSamples(wide[0], stereo[0], what + ", channel 0");
    expectSameSamples(wide[1], stereo[1], what + ", channel 1");
    for (size_t ch = 2; ch < wide.size(); ++ch)
      EXPECT_EQ(peakOf(wide[ch]), 0.0f) << what << ": channel " << ch << " is not silent";
  };

  // An 8-channel file in: 8 channels out by default, the rig on the first
  // two.
  const juce::File wideOut = scratch.dir.getChildFile("out8.wav");
  ASSERT_EQ(runRenderTool(script, wideIn, wideOut), 0);
  expectStereoOnWide(wideOut, "8 in, 8 out");

  // Stereo in, --out-channels 8.
  const juce::File widenedOut = scratch.dir.getChildFile("out2to8.wav");
  ASSERT_EQ(runRenderTool(script, stereoIn, widenedOut, {"--out-channels", "8"}), 0);
  expectStereoOnWide(widenedOut, "2 in, 8 out");

  // 8 in, --out-channels 2: the very file the stereo invocation wrote.
  const juce::File narrowedOut = scratch.dir.getChildFile("out8to2.wav");
  ASSERT_EQ(runRenderTool(script, wideIn, narrowedOut, {"--out-channels", "2"}), 0);
  juce::MemoryBlock stereoBytes, narrowedBytes;
  ASSERT_TRUE(stereoOut.loadFileAsData(stereoBytes));
  ASSERT_TRUE(narrowedOut.loadFileAsData(narrowedBytes));
  EXPECT_TRUE(stereoBytes == narrowedBytes) << "8 in, 2 out is not the stereo render's file";

  // Mono in: a mono file by default, and with --out-channels 2 that signal
  // on both channels.
  const juce::File monoOut = scratch.dir.getChildFile("out1.wav");
  ASSERT_EQ(runRenderTool(script, monoIn, monoOut), 0);
  const Channels mono = readWav(monoOut);
  ASSERT_EQ(mono.size(), 1u);
  const juce::File monoToStereoOut = scratch.dir.getChildFile("out1to2.wav");
  ASSERT_EQ(runRenderTool(script, monoIn, monoToStereoOut, {"--out-channels", "2"}), 0);
  const Channels monoToStereo = readWav(monoToStereoOut);
  ASSERT_EQ(monoToStereo.size(), 2u);
  expectSameSamples(monoToStereo[0], mono[0], "1 in, 2 out, channel 0");
  expectSameSamples(monoToStereo[1], mono[0], "1 in, 2 out, channel 1");

  // Out of range.
  EXPECT_EQ(runRenderTool(script, stereoIn, scratch.dir.getChildFile("bad.wav"),
                          {"--out-channels", "9"}),
            1);
  EXPECT_EQ(runRenderTool(script, stereoIn, scratch.dir.getChildFile("bad.wav"),
                          {"--out-channels", "0"}),
            1);
}
