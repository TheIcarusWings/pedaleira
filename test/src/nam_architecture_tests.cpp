// The captures NAM made before A2, through Pedaleira's engine: WaveNet A1,
// LSTM, ConvNet, Linear and slimmable containers of them, and captures
// trained at another rate than the chain runs at.
//
//   NamArchitectureTest   per architecture: bit-identical to
//                         NeuralAmpModelerCore's own model on the same input
//                         (the engine's realtime LSTM and ConvNet included),
//                         calibration metadata carried, no allocation while
//                         processing
//   NamModelRateTest      a capture runs at the rate it was trained at: the
//                         three rate bridges against processing at the native
//                         rate, the resonance of a 44.1 kHz model where it
//                         belongs, the reported latency where the impulse
//                         lands, no allocation through any bridge
//   SincResamplerTest     the resampler's passband, stopband and noise floor
//   NamLegacyLoadTest     every architecture loads and plays through the
//                         command path a user's file takes, and a resampled
//                         capture's latency reaches the chain
//
// Every model is made here, in code: settings of each architecture's real
// shape with deterministic weights, written to a temporary folder where a
// file is needed. Nothing is read from or written to test/files.

#include "NAM/get_dsp.h"
#include "NamEngine.h"
#include "NamModels.h"
#include "SincResampler.h"
#include "agent_test_helpers.h"
#include "effects/allocation_guard.h"
#include "json.hpp"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

using nlohmann::json;

// ####################
// MODELS, MADE IN CODE
// ####################

// Deterministic weights in [-scale, scale).
std::vector<float> weightsFor(size_t count, std::uint32_t seed, float scale) {
  std::vector<float> out(count);
  std::uint32_t state = seed * 2654435761u + 1u;
  for (auto& w : out) {
    state = state * 1664525u + 1013904223u;
    w = scale * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return out;
}

// The fields every file has, the rate when it names one, and calibration
// metadata the engine must carry whatever the architecture.
json capture(const char* architecture, json config, std::vector<float> weights, double rate,
             const char* version = "0.5.4") {
  json model = {{"version", version},
                {"architecture", architecture},
                {"config", std::move(config)},
                {"weights", std::move(weights)},
                {"metadata",
                 {{"name", std::string("Test ") + architecture},
                  {"loudness", -21.5},
                  {"input_level_dbu", 12.25},
                  {"output_level_dbu", 4.5}}}};
  if (rate > 0.0)
    model["sample_rate"] = rate;
  return model;
}

json lstmModel(double rate, int layers = 2, int hidden = 8) {
  const json config = {{"num_layers", layers}, {"input_size", 1}, {"hidden_size", hidden}};
  return capture("LSTM", config,
                 weightsFor(NamModels::RealtimeLstm::weightCount(layers, 1, hidden, 1), 7, 0.45f),
                 rate);
}

json convNetModel(double rate) {
  const int channels = 6;
  const std::vector<int> dilations = {1, 2, 4, 8, 16, 32, 64};
  const json config = {{"channels", channels},
                       {"dilations", dilations},
                       {"batchnorm", true},
                       {"activation", "Tanh"}};
  // Block by block, so the batch norm's variances and epsilon are positive.
  std::vector<float> weights;
  std::uint32_t seed = 11;
  for (size_t b = 0; b < dilations.size(); ++b) {
    const int in = b == 0 ? 1 : channels;
    for (float w : weightsFor(static_cast<size_t>(2 * channels * in), seed++, 0.7f))
      weights.push_back(w);
    for (float m : weightsFor(channels, seed++, 0.1f))
      weights.push_back(m);  // running mean
    for (float v : weightsFor(channels, seed++, 0.3f))
      weights.push_back(1.0f + v);  // running variance
    for (float g : weightsFor(channels, seed++, 0.3f))
      weights.push_back(1.0f + g);  // weight
    for (float c : weightsFor(channels, seed++, 0.1f))
      weights.push_back(c);  // bias
    weights.push_back(1e-5f);  // epsilon
  }
  for (float w : weightsFor(channels + 1, seed, 0.5f))
    weights.push_back(w);  // head weights and bias
  return capture("ConvNet", config, weights, rate);
}

json linearModel(double rate, int receptiveField, bool bias, std::uint32_t seed) {
  const json config = {{"receptive_field", receptiveField}, {"bias", bias}};
  std::vector<float> weights = weightsFor(static_cast<size_t>(receptiveField + (bias ? 1 : 0)), seed,
                                          1.0f / std::sqrt(static_cast<float>(receptiveField)));
  return capture("Linear", config, weights, rate);
}

// A Linear model that passes its input through untouched: one tap of 1.
json identityModel(double rate) {
  return capture("Linear", {{"receptive_field", 1}, {"bias", false}}, {1.0f}, rate);
}

// A Linear model that rings at `resonance` Hz at its own rate: a decaying
// sinusoid. Run at the wrong rate, the ring moves with the ratio.
json resonantModel(double rate, double resonance) {
  // Weight n is the tap n samples back (the core reverses them itself).
  const int taps = 2048;
  std::vector<float> weights(static_cast<size_t>(taps));
  for (int n = 0; n < taps; ++n)
    weights[static_cast<size_t>(n)] =
        0.05f * static_cast<float>(std::pow(0.997, n) * std::sin(2.0 * kPi * resonance * n / rate));
  return capture("Linear", {{"receptive_field", taps}, {"bias", false}}, weights, rate);
}

// The NAM A1 "standard" WaveNet's shape: two layer arrays of ten dilated
// layers, 16 then 8 channels, tanh, no gating. A1 files predate the
// sample_rate field, so `rate` 0 leaves it out.
json wavenetA1Model(double rate) {
  std::vector<int> dilations;
  for (int d = 1; d <= 512; d *= 2)
    dilations.push_back(d);
  const json layers = json::array(
      {{{"input_size", 1}, {"condition_size", 1}, {"head_size", 8}, {"channels", 16},
        {"kernel_size", 3}, {"dilations", dilations}, {"activation", "Tanh"}, {"gated", false},
        {"head_bias", false}},
       {{"input_size", 16}, {"condition_size", 1}, {"head_size", 1}, {"channels", 8},
        {"kernel_size", 3}, {"dilations", dilations}, {"activation", "Tanh"}, {"gated", false},
        {"head_bias", true}}});
  const json config = {{"layers", layers}, {"head", nullptr}, {"head_scale", 0.02}};
  // Per layer array: the input rechannel, per layer the dilated conv (with
  // bias), the condition mix-in and the 1x1 (with bias), then the head
  // rechannel; one head scale at the end. 13802 for this shape, as the
  // core's own A1 standard example has.
  auto arrayWeights = [&](int in, int channels, int head, bool headBias) {
    const int layer = 3 * channels * channels + channels + channels + channels * channels + channels;
    return in * channels + static_cast<int>(dilations.size()) * layer + channels * head +
           (headBias ? head : 0);
  };
  const int count = arrayWeights(1, 16, 8, false) + arrayWeights(16, 8, 1, true) + 1;
  std::vector<float> weights = weightsFor(static_cast<size_t>(count), 23, 0.2f);
  weights.back() = 0.02f;  // the head scale
  return capture("WaveNet", config, weights, rate, "0.5.0");
}

// A slimmable container of two older architectures (as a trainer could pack
// any): an LSTM for the light end and a ConvNet for the full one.
json containerModel(double rate) {
  json lite = lstmModel(rate, 1, 6);
  json full = convNetModel(rate);
  json model = {{"version", "0.7.0"},
                {"architecture", "SlimmableContainer"},
                {"config",
                 {{"submodels", json::array({{{"max_value", 0.5}, {"model", lite}},
                                             {{"max_value", 1.0}, {"model", full}}})}}},
                {"weights", json::array()},
                {"metadata", {{"loudness", -19.0}, {"input_level_dbu", 10.0}}}};
  if (rate > 0.0)
    model["sample_rate"] = rate;
  return model;
}

// ####################
// RUNNING THEM
// ####################

// Built the way the loader builds them (ProcessorModelLoader): one instance
// per phase when the model is phase-safe and the chain oversampled, one per
// voice.
std::unique_ptr<NamEngine> engineFor(const json& model, int factor = 1, int voices = 1) {
  const bool phaseSafe = factor > 1 && NamModels::isPhaseSafe(model);
  const int count = (phaseSafe ? factor : 1) * voices;
  std::vector<std::unique_ptr<nam::DSP>> instances;
  for (int i = 0; i < count; ++i)
    instances.push_back(NamModels::build(model));
  return std::make_unique<NamEngine>(std::move(instances), factor, voices);
}

// A buffer the engine processes in place, cut into blocks by `pattern`
// (repeated). The views over the buffer do not allocate.
std::vector<float> runEngine(NamEngine& engine, const std::vector<float>& in,
                             const std::vector<int>& pattern) {
  std::vector<float> data = in;
  float* channels[1] = {nullptr};
  size_t start = 0;
  for (size_t p = 0; start < data.size(); p = (p + 1) % pattern.size()) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(pattern[p]), data.size() - start));
    channels[0] = data.data() + start;
    juce::AudioBuffer<float> view(channels, 1, count);
    engine.process(view);
    start += static_cast<size_t>(count);
  }
  return data;
}

// The core's own model on the same input, prepared exactly as NamEngine
// prepares an instance (same rate, same buffer size, the same priming of a
// model that does not prewarm), fed the same blocks.
std::vector<float> runCore(const json& model, double rate, int capacity,
                           const std::vector<float>& in, const std::vector<int>& pattern) {
  std::unique_ptr<nam::DSP> dsp = nam::get_dsp(model);
  dsp->ResetAndPrewarm(rate, capacity);
  std::vector<double> inputs(static_cast<size_t>(capacity)), outputs(static_cast<size_t>(capacity));
  NAM_SAMPLE* ins[] = {inputs.data()};
  NAM_SAMPLE* outs[] = {outputs.data()};
  if (dsp->GetPrewarmSamples() == 0)
    dsp->process(ins, outs, capacity);
  std::vector<float> out(in.size());
  size_t start = 0;
  for (size_t p = 0; start < in.size(); p = (p + 1) % pattern.size()) {
    const int count =
        static_cast<int>(std::min<size_t>(static_cast<size_t>(pattern[p]), in.size() - start));
    for (int i = 0; i < count; ++i)
      inputs[static_cast<size_t>(i)] = static_cast<double>(in[start + static_cast<size_t>(i)]);
    dsp->process(ins, outs, count);
    for (int i = 0; i < count; ++i)
      out[start + static_cast<size_t>(i)] = static_cast<float>(outputs[static_cast<size_t>(i)]);
    start += static_cast<size_t>(count);
  }
  return out;
}

// Guitar-like test input: noise under a slow swell, so the models see quiet
// and loud passages.
std::vector<float> testSignal(int frames, unsigned seed) {
  std::vector<float> x = makeNoise(frames, seed, 0.5f);
  for (int i = 0; i < frames; ++i)
    x[static_cast<size_t>(i)] *= static_cast<float>(0.3 + 0.7 * std::abs(std::sin(kPi * i / 9000.0)));
  return x;
}

double rms(const std::vector<float>& x, size_t start = 0) {
  double sum = 0.0;
  for (size_t i = start; i < x.size(); ++i)
    sum += static_cast<double>(x[i]) * x[i];
  return std::sqrt(sum / static_cast<double>(std::max<size_t>(1, x.size() - start)));
}

bool allFinite(const std::vector<float>& x) {
  for (float s : x)
    if (!std::isfinite(s))
      return false;
  return true;
}

struct ArchitectureCase {
  const char* label;
  std::function<json()> make;
};

class NamArchitectureTest : public ::testing::TestWithParam<ArchitectureCase> {};

INSTANTIATE_TEST_SUITE_P(
    Architectures, NamArchitectureTest,
    ::testing::Values(ArchitectureCase{"Lstm", [] { return lstmModel(48000.0); }},
                      ArchitectureCase{"ConvNet", [] { return convNetModel(48000.0); }},
                      ArchitectureCase{"LinearDirect", [] { return linearModel(48000.0, 200, true, 5); }},
                      ArchitectureCase{"LinearFft", [] { return linearModel(48000.0, 1500, false, 6); }},
                      ArchitectureCase{"WaveNetA1", [] { return wavenetA1Model(0.0); }},
                      ArchitectureCase{"Container", [] { return containerModel(48000.0); }}),
    [](const auto& info) { return std::string(info.param.label); });

}  // namespace

// ####################
// EACH ARCHITECTURE
// ####################

TEST_P(NamArchitectureTest, MatchesTheCoreBitForBit) {
  const json model = GetParam().make();
  // Ragged blocks: whole, partial, single frames.
  const std::vector<int> pattern = {256, 100, 1, 155, 256, 37, 219, 64};
  const auto in = testSignal(48000, 31);

  auto engine = engineFor(model);
  EXPECT_EQ(engine->getRateBridge(), NamEngine::RateBridge::none);
  EXPECT_EQ(engine->latencySamples(), 0);
  engine->prepare(256);
  const auto out = runEngine(*engine, in, pattern);

  // NamEngine::prepare sizes each instance for its block plus one frame.
  const auto reference = runCore(model, 48000.0, 257, in, pattern);
  ASSERT_EQ(out.size(), reference.size());
  float worst = 0.0f;
  size_t firstMismatch = out.size();
  for (size_t i = 0; i < out.size(); ++i) {
    const float diff = std::abs(out[i] - reference[i]);
    if (diff > 0.0f && firstMismatch == out.size())
      firstMismatch = i;
    worst = std::max(worst, diff);
  }
  // Exact, except the FFT Linear: the priming call moves its partitions, so
  // its sums round differently (still the same convolution).
  const bool fft = std::string(GetParam().label) == "LinearFft";
  if (fft)
    EXPECT_LE(worst, 1e-6f);
  else
    EXPECT_EQ(worst, 0.0f) << "first mismatch at " << firstMismatch;
  std::printf("[ match ] %-12s max |engine - core| = %.3g\n", GetParam().label,
              static_cast<double>(worst));

  EXPECT_TRUE(allFinite(out));
  EXPECT_GT(rms(out), 1e-3) << "silent";
  float changed = 0.0f;
  for (size_t i = 0; i < out.size(); ++i)
    changed = std::max(changed, std::abs(out[i] - in[i]));
  EXPECT_GT(changed, 1e-3f) << "passed through untouched";
}

TEST_P(NamArchitectureTest, CarriesTheCalibrationMetadata) {
  const json model = GetParam().make();
  auto engine = engineFor(model);
  ASSERT_TRUE(engine->hasLoudness());
  EXPECT_DOUBLE_EQ(engine->getLoudness(), model["metadata"]["loudness"].get<double>());
  ASSERT_TRUE(engine->hasInputLevel());
  EXPECT_FLOAT_EQ(static_cast<float>(engine->getInputLevel()),
                  model["metadata"]["input_level_dbu"].get<float>());
  EXPECT_EQ(engine->hasOutputLevel(), model["metadata"].contains("output_level_dbu"));
}

TEST_P(NamArchitectureTest, DoesNotAllocateWhileProcessing) {
  const json model = GetParam().make();
  for (const int voices : {1, 2}) {
    auto engine = engineFor(model, 1, voices);
    engine->prepare(128);
    juce::AudioBuffer<float> buffer(2, 128);
    const auto in = testSignal(128 * 60, 5);
    auto run = [&](int block) {
      for (int ch = 0; ch < 2; ++ch)
        buffer.copyFrom(ch, 0, in.data() + block * 128, 128);
      engine->process(buffer, nullptr, voices > 1);
    };
    for (int block = 0; block < 4; ++block)
      run(block);
    fxtest::AllocationGuard guard;
    for (int block = 4; block < 60; ++block)
      run(block);
    EXPECT_EQ(guard.count(), 0) << voices << " voices";
    EXPECT_EQ(guard.heapCount(), 0) << voices << " voices";
  }
}

TEST(NamArchitecture, TheGuardSeesEigenAllocate) {
  // The allocation checks above lean on heapCount() seeing Eigen's own
  // allocations (they go to malloc, not operator new).
  if (!fxtest::AllocationGuard::heapCounted())
    GTEST_SKIP() << "malloc is only counted on glibc";
  fxtest::AllocationGuard guard;
  Eigen::VectorXf* volatile v = new Eigen::VectorXf(Eigen::VectorXf::Zero(4096));
  delete v;
  EXPECT_GE(guard.heapCount(), 2) << "the object and its storage";
}

TEST(NamArchitecture, TheRealtimeModelsMatchTheCoresAtDoublePrecision) {
  // Underneath the engine's float buffers: the two architectures Pedaleira
  // builds itself, against the core's, sample for sample in double.
  for (const json& model : {lstmModel(48000.0, 3, 12), convNetModel(48000.0)}) {
    auto ours = NamModels::build(model);
    auto core = nam::get_dsp(model);
    ours->ResetAndPrewarm(48000.0, 512);
    core->ResetAndPrewarm(48000.0, 512);
    const auto in = testSignal(512 * 20, 77);
    std::vector<double> a(512), b(512), x(512);
    NAM_SAMPLE* xs[] = {x.data()};
    NAM_SAMPLE* as[] = {a.data()};
    NAM_SAMPLE* bs[] = {b.data()};
    for (int block = 0; block < 20; ++block) {
      for (int i = 0; i < 512; ++i)
        x[static_cast<size_t>(i)] = in[static_cast<size_t>(block * 512 + i)];
      ours->process(xs, as, 512);
      core->process(xs, bs, 512);
      for (int i = 0; i < 512; ++i)
        ASSERT_EQ(a[static_cast<size_t>(i)], b[static_cast<size_t>(i)])
            << model["architecture"].get<std::string>() << " block " << block << " frame " << i;
    }
  }
}

TEST(NamArchitecture, AnLstmResetStartsFromItsTrainedState) {
  const json model = lstmModel(48000.0);
  auto engine = engineFor(model);
  const auto in = testSignal(4096, 3);
  engine->prepare(512);
  const auto first = runEngine(*engine, in, {512});
  engine->prepare(512);
  const auto second = runEngine(*engine, in, {512});
  EXPECT_EQ(first, second);
}

TEST(NamArchitecture, RefusesWhatItCannotBuild) {
  json lstm = lstmModel(48000.0);
  lstm["weights"].erase(lstm["weights"].begin());
  EXPECT_THROW(NamModels::build(lstm), std::exception);
  json conv = convNetModel(48000.0);
  conv["weights"].push_back(0.5);
  EXPECT_THROW(NamModels::build(conv), std::exception);
  json unknown = lstmModel(48000.0);
  unknown["architecture"] = "Transformer";
  EXPECT_THROW(NamModels::build(unknown), std::exception);
  json absurd = lstmModel(48000.0);
  absurd["sample_rate"] = 1.0e7;
  EXPECT_THROW(NamModels::build(absurd), std::exception);
  json huge = lstmModel(48000.0);
  huge["config"]["hidden_size"] = 1 << 20;
  EXPECT_THROW(NamModels::build(huge), std::exception);
}

// ####################
// THE MODEL'S RATE
// ####################

namespace {

// One harmonic's complex amplitude, by a plain DFT over a window of whole
// periods (1 s of a 1 kHz tone at either rate), so there is no leakage.
std::complex<double> harmonic(const std::vector<float>& x, size_t start, size_t count,
                              double frequency, double rate) {
  std::complex<double> sum{0.0, 0.0};
  for (size_t i = 0; i < count; ++i) {
    const double phase = -2.0 * kPi * frequency * static_cast<double>(i) / rate;
    sum += static_cast<double>(x[start + i]) * std::complex<double>(std::cos(phase), std::sin(phase));
  }
  return sum * (2.0 / static_cast<double>(count));
}

std::vector<float> toneAt(double rate, double seconds, double frequency, float amplitude) {
  return makeSine(static_cast<int>(rate * seconds), frequency, amplitude, rate);
}

// The model run by the core at its own rate on the tone sampled at that rate:
// what it should sound like.
std::vector<float> nativeRun(const json& model, double rate, const std::vector<float>& tone) {
  return runCore(model, rate, 513, tone, {512});
}

struct HarmonicMatch {
  double worstComplex = 0.0;  // |H_k e^{i w_k tau} - 1|, the delay taken out
  double worstMagnitudeDb = 0.0;
};

// The harmonics of a 1 kHz tone up to 18 kHz, engine (chain rate) against the
// native run, over one second from `fromSeconds`. Harmonics 60 dB or more
// under the fundamental are left out (their ratio is noise). With
// `compareDelay`, the phases must agree after one pure delay, which the
// fundamental fixes (starting from the reported latency).
HarmonicMatch compareHarmonics(const std::vector<float>& engineOut, double chainRate,
                               const std::vector<float>& nativeOut, double nativeRate,
                               int latency, double fromSeconds, bool compareDelay) {
  const auto eStart = static_cast<size_t>(fromSeconds * chainRate);
  const auto nStart = static_cast<size_t>(fromSeconds * nativeRate);
  const auto eCount = static_cast<size_t>(chainRate);
  const auto nCount = static_cast<size_t>(nativeRate);
  const double omega1 = 2.0 * kPi * 1000.0;
  const std::complex<double> fundamental = harmonic(nativeOut, nStart, nCount, 1000.0, nativeRate);
  double tau = static_cast<double>(latency) / chainRate;
  {
    const std::complex<double> ratio =
        harmonic(engineOut, eStart, eCount, 1000.0, chainRate) / fundamental;
    tau -= std::arg(ratio * std::polar(1.0, omega1 * tau)) / omega1;
  }
  HarmonicMatch match;
  for (int k = 1; k <= 18; ++k) {
    const double f = 1000.0 * k;
    const std::complex<double> nat = harmonic(nativeOut, nStart, nCount, f, nativeRate);
    if (std::abs(nat) < 1e-3 * std::abs(fundamental))
      continue;
    const std::complex<double> ratio = harmonic(engineOut, eStart, eCount, f, chainRate) / nat;
    match.worstMagnitudeDb =
        std::max(match.worstMagnitudeDb, std::abs(20.0 * std::log10(std::abs(ratio))));
    if (compareDelay)
      match.worstComplex =
          std::max(match.worstComplex, std::abs(ratio * std::polar(1.0, 2.0 * kPi * f * tau) - 1.0));
  }
  return match;
}

}  // namespace

TEST(NamModelRateTest, EachRatioGetsItsBridge) {
  struct Case {
    double rate;
    int factor;
    bool lstm;
    NamEngine::RateBridge bridge;
  };
  using B = NamEngine::RateBridge;
  for (const Case& c : {Case{48000.0, 1, false, B::none}, Case{0.0, 4, false, B::none},
                        Case{48000.0, 4, true, B::lower}, Case{96000.0, 4, true, B::lower},
                        Case{96000.0, 2, true, B::none}, Case{96000.0, 1, false, B::raise},
                        Case{192000.0, 2, false, B::raise}, Case{44100.0, 1, false, B::resample},
                        Case{44100.0, 2, true, B::resample}, Case{24000.0, 1, true, B::resample}}) {
    const json model = c.lstm ? lstmModel(c.rate, 1, 4) : linearModel(c.rate, 8, false, 1);
    auto engine = engineFor(model, c.factor);
    EXPECT_EQ(engine->getRateBridge(), c.bridge) << c.rate << " Hz at x" << c.factor;
    EXPECT_EQ(engine->getModelSampleRate(), c.rate > 0.0 ? c.rate : 48000.0);
    EXPECT_EQ(engine->modelRateDeclared(), c.rate > 0.0);
  }
}

TEST(NamModelRateTest, AResampledCaptureMatchesItsNativeRate) {
  // A WaveNet and an LSTM trained at 44.1 kHz, driven into distortion by a
  // 1 kHz tone in a 48 kHz chain: every harmonic to 18 kHz matches the core
  // running the model at 44.1 kHz on the tone sampled at 44.1 kHz, in level
  // and (after the reported delay) in phase.
  for (const json& model : {wavenetA1Model(44100.0), lstmModel(44100.0)}) {
    auto engine = engineFor(model);
    ASSERT_EQ(engine->getRateBridge(), NamEngine::RateBridge::resample);
    engine->prepare(512);
    EXPECT_GT(engine->latencySamples(), 0);
    const auto chainTone = toneAt(48000.0, 2.2, 1000.0, 0.5f);
    const auto out = runEngine(*engine, chainTone, {512, 300, 512, 7});
    EXPECT_EQ(engine->bridgeUnderruns(), 0);
    ASSERT_TRUE(allFinite(out));

    const auto native = nativeRun(model, 44100.0, toneAt(44100.0, 2.2, 1000.0, 0.5f));
    const HarmonicMatch match =
        compareHarmonics(out, 48000.0, native, 44100.0, engine->latencySamples(), 1.0, true);
    EXPECT_LT(match.worstComplex, 2e-3) << model["architecture"].get<std::string>();
    EXPECT_LT(match.worstMagnitudeDb, 0.01) << model["architecture"].get<std::string>();

    // What the chain played before: the model run at 48 kHz unresampled.
    const auto wrong = runCore(model, 48000.0, 513, chainTone, {512});
    const HarmonicMatch before = compareHarmonics(wrong, 48000.0, native, 44100.0, 0, 1.0, false);
    std::printf("[ rate  ] %-8s 44.1k model at 48k: harmonics within %.5f dB (complex %.2e), "
                "latency %d; unresampled was off by up to %.3f dB\n",
                model["architecture"].get<std::string>().c_str(), match.worstMagnitudeDb,
                match.worstComplex, engine->latencySamples(), before.worstMagnitudeDb);
  }
}

TEST(NamModelRateTest, ANinetySixKilohertzCaptureRunsRaised) {
  // Raised by the chain oversampler's half-band stages (minimum phase, so
  // levels are compared, not phases).
  const json model = wavenetA1Model(96000.0);
  auto engine = engineFor(model);
  ASSERT_EQ(engine->getRateBridge(), NamEngine::RateBridge::raise);
  engine->prepare(512);
  EXPECT_EQ(engine->latencySamples(), 0);
  const auto out = runEngine(*engine, toneAt(48000.0, 2.2, 1000.0, 0.5f), {512, 96});
  const auto native = nativeRun(model, 96000.0, toneAt(96000.0, 2.2, 1000.0, 0.5f));
  const HarmonicMatch match = compareHarmonics(out, 48000.0, native, 96000.0, 0, 1.0, false);
  EXPECT_LT(match.worstMagnitudeDb, 0.02);
  std::printf("[ rate  ] 96k model at 48k: harmonics within %.5f dB\n", match.worstMagnitudeDb);
}

TEST(NamModelRateTest, AnLstmInAnOversampledChainRunsAtItsOwnRate) {
  // A recurrent model can't be phase-split, so in a x2 chain the engine
  // lowers the rate to 48 kHz around its one instance, exactly as an IR
  // island does: the same half-band stages around the same model.
  const json model = lstmModel(48000.0);
  auto engine = engineFor(model, 2);
  ASSERT_EQ(engine->getPhaseCount(), 1);
  ASSERT_EQ(engine->getRateBridge(), NamEngine::RateBridge::lower);
  engine->prepare(512);
  const auto in = testSignal(96000, 8);
  const std::vector<int> pattern = {512, 64, 128, 2};
  const auto out = runEngine(*engine, in, pattern);

  ChainOversampler island;
  island.prepare(2, 256);
  auto core = nam::get_dsp(model);
  core->ResetAndPrewarm(48000.0, 257);
  std::vector<double> x(256), y(256);
  std::vector<float> reference = in;
  size_t start = 0;
  for (size_t p = 0; start < reference.size(); p = (p + 1) % pattern.size()) {
    const int count = static_cast<int>(
        std::min<size_t>(static_cast<size_t>(pattern[p]), reference.size() - start));
    float* channels[1] = {reference.data() + start};
    island.processBaseRateIsland(channels, 1, count, [&](float* const* base, int frames) {
      for (int i = 0; i < frames; ++i)
        x[static_cast<size_t>(i)] = base[0][i];
      NAM_SAMPLE* xs[] = {x.data()};
      NAM_SAMPLE* ys[] = {y.data()};
      core->process(xs, ys, frames);
      for (int i = 0; i < frames; ++i)
        base[0][i] = static_cast<float>(y[static_cast<size_t>(i)]);
    });
    start += static_cast<size_t>(count);
  }
  for (size_t i = 0; i < out.size(); ++i)
    ASSERT_EQ(out[i], reference[i]) << "at " << i;

  // A phase-safe ConvNet in the same chain is phase-split instead.
  auto conv = engineFor(convNetModel(48000.0), 2);
  EXPECT_EQ(conv->getPhaseCount(), 2);
  EXPECT_EQ(conv->getRateBridge(), NamEngine::RateBridge::none);
}

TEST(NamModelRateTest, ResonanceLandsWhereTheModelPutsIt) {
  // A 44.1 kHz model that rings at 3 kHz. Run at its rate it rings at 3 kHz
  // in the 48 kHz chain; run unresampled it rang 48/44.1 higher.
  const json model = resonantModel(44100.0, 3000.0);
  auto peakOf = [](const std::vector<float>& response) {
    double best = 0.0, at = 0.0;
    for (double f = 2500.0; f <= 3600.0; f += 0.5) {
      const double power = std::norm(harmonic(response, 0, response.size(), f, 48000.0));
      if (power > best) {
        best = power;
        at = f;
      }
    }
    return at;
  };
  std::vector<float> impulse(48000, 0.0f);
  impulse[100] = 1.0f;

  auto engine = engineFor(model);
  engine->prepare(512);
  const double resampled = peakOf(runEngine(*engine, impulse, {512}));
  const double unresampled = peakOf(runCore(model, 48000.0, 513, impulse, {512}));
  EXPECT_NEAR(resampled, 3000.0, 1.0);
  EXPECT_NEAR(unresampled, 3000.0 * 48000.0 / 44100.0, 2.0);
  std::printf("[ pitch ] 3 kHz resonance of a 44.1k model: %.1f Hz resampled, %.1f Hz unresampled\n",
              resampled, unresampled);

  // A tone keeps its pitch through the bridge: the 1 kHz fundamental comes
  // out at 1 kHz, not a fraction of a hertz off.
  auto tone = engineFor(wavenetA1Model(44100.0));
  tone->prepare(512);
  const auto out = runEngine(*tone, toneAt(48000.0, 2.0, 1000.0, 0.5f), {512});
  double best = 0.0, at = 0.0;
  for (double f = 995.0; f <= 1005.0; f += 0.05) {
    const double power = std::norm(harmonic(out, 48000, 48000, f, 48000.0));
    if (power > best) {
      best = power;
      at = f;
    }
  }
  EXPECT_NEAR(at, 1000.0, 0.05);
}

TEST(NamModelRateTest, ReportedLatencyIsWhereTheImpulseArrives) {
  // An identity model trained at another rate: the engine's delay is all
  // bridge, and an impulse's peak lands exactly that many samples later,
  // whatever the block size. The resampler's phases repeat every few dozen
  // chain samples; the impulse sits where they start over (4800 is a whole
  // number of every period here), as the measurement's does.
  const size_t at = 4800;
  for (const double rate : {44100.0, 88200.0, 32000.0}) {
    for (const int factor : {1, 2}) {
      int reported = -1;
      for (const int block : {32, 128, 512, 4096}) {
        auto engine = engineFor(identityModel(rate), factor);
        engine->prepare(block);
        if (engine->getRateBridge() != NamEngine::RateBridge::resample)
          continue;
        if (reported < 0)
          reported = engine->latencySamples();
        EXPECT_EQ(engine->latencySamples(), reported) << rate << " x" << factor << " block " << block;
        std::vector<float> in(at + 2 * 4096, 0.0f);
        in[at] = 1.0f;
        const auto out = runEngine(*engine, in, {block});
        size_t peak = 0;
        for (size_t i = 0; i < out.size(); ++i)
          if (std::abs(out[i]) > std::abs(out[peak]))
            peak = i;
        EXPECT_EQ(static_cast<int>(peak - at), reported) << rate << " x" << factor << " block " << block;
        // Band-limited to the lower rate, so a little under 1.
        EXPECT_GT(out[peak], 0.5f);
      }
      if (reported >= 0)
        std::printf("[ delay ] %.0f Hz identity model in a x%d chain: %d samples (%.3f ms)\n",
                    rate, factor, reported, 1000.0 * reported / (48000.0 * factor));
    }
  }
}

TEST(NamModelRateTest, NoBridgeAllocatesWhileProcessing) {
  struct Case {
    json model;
    int factor;
    int voices;
  };
  const std::vector<Case> cases = {{wavenetA1Model(44100.0), 1, 1}, {lstmModel(44100.0), 1, 2},
                                   {lstmModel(44100.0), 2, 1},      {convNetModel(88200.0), 2, 2},
                                   {lstmModel(96000.0), 1, 1},      {lstmModel(48000.0), 4, 2}};
  for (const Case& c : cases) {
    auto engine = engineFor(c.model, c.factor, c.voices);
    const int block = 128 * c.factor;
    engine->prepare(block);
    juce::AudioBuffer<float> buffer(2, block);
    const auto in = testSignal(block * 40, 9);
    auto run = [&](int n) {
      for (int ch = 0; ch < 2; ++ch)
        buffer.copyFrom(ch, 0, in.data() + n * block, block);
      engine->process(buffer, nullptr, c.voices > 1);
    };
    for (int n = 0; n < 4; ++n)
      run(n);
    fxtest::AllocationGuard guard;
    for (int n = 4; n < 40; ++n)
      run(n);
    EXPECT_EQ(guard.heapCount(), 0) << c.model["architecture"].get<std::string>() << " x" << c.factor;
    EXPECT_EQ(guard.count(), 0);
    EXPECT_EQ(engine->bridgeUnderruns(), 0);
  }
}

// ####################
// THE RESAMPLER
// ####################

namespace {

// `in` through one SincResampler direction, on the boundary's schedule with
// nothing in the middle: from `rateIn` to `rateOut`.
std::vector<float> resampleOnce(const std::vector<float>& in, double rateIn, double rateOut) {
  BasicChainBoundaryMulti<SincResampler<2>> boundary;
  // The boundary's "host" side is the input; what the callback sees is the
  // other rate. Capture it there.
  EXPECT_TRUE(boundary.reset(rateIn, rateOut, 512, 1, 1));
  std::vector<float> captured;
  std::vector<float> data = in, scratch(in.size(), 0.0f);
  for (size_t start = 0; start + 512 <= data.size(); start += 512) {
    const float* const inCh[2] = {data.data() + start, data.data() + start};
    float* const outCh[2] = {scratch.data() + start, scratch.data() + start};
    const float* const* const ins[1] = {inCh};
    float* const* const outs[1] = {outCh};
    boundary.process(ins, outs, 512,
                     [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
                       captured.insert(captured.end(), chainIn[0][0], chainIn[0][0] + frames);
                       for (int ch = 0; ch < 2; ++ch)
                         std::copy(chainIn[0][ch], chainIn[0][ch] + frames, chainOut[0][ch]);
                     });
  }
  return captured;
}

double levelDb(const std::vector<float>& x, size_t start, size_t count, double f, double rate) {
  return 20.0 * std::log10(std::max(1e-12, std::abs(harmonic(x, start, count, f, rate))));
}

// What is left of a tone once the tone itself is taken out (least squares
// on its sine and cosine), relative to the tone, in dB.
double residualDb(const std::vector<float>& x, size_t start, size_t count, double f, double rate) {
  const std::complex<double> a = harmonic(x, start, count, f, rate);
  double residual = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double phase = 2.0 * kPi * f * static_cast<double>(i) / rate;
    const double fit = a.real() * std::cos(phase) - a.imag() * std::sin(phase);
    const double r = x[start + i] - fit;
    residual += r * r;
  }
  const double tone = 0.5 * std::norm(a) * static_cast<double>(count);
  return 10.0 * std::log10(std::max(1e-30, residual / tone));
}

}  // namespace

TEST(SincResamplerTest, PassbandStopbandAndNoise) {
  // 48 kHz to 44.1 kHz and back the other way, and 96 kHz down to 44.1 kHz.
  // Windows of one second at the output rate hold whole periods of every
  // tone here, so the DFT reads them without leakage.
  double worstPassband = 0.0;
  for (const double f : {100.0, 1000.0, 10000.0, 19000.0, 20000.0}) {
    const auto down = resampleOnce(toneAt(48000.0, 3.0, f, 0.5f), 48000.0, 44100.0);
    const auto up = resampleOnce(toneAt(44100.0, 3.0, f, 0.5f), 44100.0, 48000.0);
    worstPassband = std::max(worstPassband,
                             std::abs(levelDb(down, 44100, 44100, f, 44100.0) - 20.0 * std::log10(0.5)));
    worstPassband = std::max(worstPassband,
                             std::abs(levelDb(up, 48000, 48000, f, 48000.0) - 20.0 * std::log10(0.5)));
  }
  EXPECT_LT(worstPassband, 0.001);

  // Lowering the rate: a tone above the new Nyquist must not fold back. At
  // 30 kHz into 44.1 kHz it would land on 14.1 kHz.
  const auto folded = resampleOnce(toneAt(96000.0, 3.0, 30000.0, 0.5f), 96000.0, 44100.0);
  const double alias = levelDb(folded, 44100, 44100, 14100.0, 44100.0) - 20.0 * std::log10(0.5);
  EXPECT_LT(alias, -95.0);

  // Raising it: a 15 kHz tone at 44.1 kHz has its image at 29.1 kHz, which
  // a 48 kHz output would show at 18.9 kHz.
  const auto raised = resampleOnce(toneAt(44100.0, 3.0, 15000.0, 0.5f), 44100.0, 48000.0);
  const double image = levelDb(raised, 48000, 48000, 18900.0, 48000.0) - 20.0 * std::log10(0.5);
  EXPECT_LT(image, -95.0);

  // Everything that is not the tone, after a round trip 48 to 44.1 to 48.
  const auto there = resampleOnce(toneAt(48000.0, 4.0, 1000.0, 0.5f), 48000.0, 44100.0);
  const auto back = resampleOnce(there, 44100.0, 48000.0);
  const double noise = residualDb(back, 48000, 48000, 1000.0, 48000.0);
  EXPECT_LT(noise, -100.0);

  std::printf("[ sinc  ] passband error %.5f dB to 20 kHz, alias %.1f dB, image %.1f dB, "
              "round-trip residual %.1f dB\n",
              worstPassband, alias, image, noise);
}

// ####################
// THROUGH THE COMMAND PATH
// ####################

TEST(NamLegacyLoadTest, EveryArchitectureLoadsAndPlays) {
  struct File {
    const char* name;
    json model;
  };
  const std::vector<File> files = {{"Lstm.nam", lstmModel(48000.0)},
                                   {"ConvNet.nam", convNetModel(48000.0)},
                                   {"Linear.nam", linearModel(48000.0, 300, true, 4)},
                                   {"Wavenet A1.nam", wavenetA1Model(0.0)},
                                   {"Packed.nam", containerModel(48000.0)},
                                   {"Wavenet 44k.nam", wavenetA1Model(44100.0)},
                                   {"Lstm 96k.nam", lstmModel(96000.0)}};
  for (const File& file : files) {
    // A rig of its own per capture, loaded the way a user's file loads.
    AgentRig rig;
    const juce::File path = rig.dirs.library.getChildFile(file.name);
    ASSERT_TRUE(path.replaceWithText(file.model.dump()));
    const juce::var info = rig.mustLocal("nam.inspect", "{\"path\":" + agentQuoted(path.getFullPathName()) + "}");
    EXPECT_TRUE(static_cast<bool>(info["supported"])) << file.name << ": " << info["unsupportedReason"].toString();

    rig.mustLocal("chain.addToneFromFiles", "{\"paths\":[" + agentQuoted(path.getFullPathName()) + "]}");
    rig.mustLocal("state.waitUntilLoaded");

    const auto out = rig.processLive(toneAt(48000.0, 1.0, 220.0, 0.3f));
    EXPECT_TRUE(allFinite(out)) << file.name;
    EXPECT_GT(rms(out, out.size() / 2), 1e-4) << file.name << " is silent";

    // A capture resampled at its own rate brings its delay to the chain.
    const int expected = [&] {
      auto engine = engineFor(file.model);
      engine->prepare(rig.block);
      return engine->latencySamples();
    }();
    const int chain = static_cast<int>(rig.proc.getGridState()["chainLatencySamples"]);
    EXPECT_EQ(chain, expected) << file.name;
    if (expected > 0)
      std::printf("[ load  ] %s: chain latency %d samples\n", file.name, chain);
  }
}

// ####################
// CPU (run by hand)
// ####################

// --gtest_also_run_disabled_tests --gtest_filter='*CpuPerArchitecture*'
TEST(DISABLED_NamArchitectureBench, CpuPerArchitectureAt128) {
  struct Case {
    const char* label;
    json model;
    bool core = false;  // the core's own model, for comparison
  };
  const std::vector<Case> cases = {{"LSTM 2x8 (48k)", lstmModel(48000.0)},
                                   {"LSTM 2x8 (48k), core's", lstmModel(48000.0), true},
                                   {"ConvNet 6ch (48k), core's", convNetModel(48000.0), true},
                                   {"LSTM 1x24 (48k)", lstmModel(48000.0, 1, 24)},
                                   {"ConvNet 6ch (48k)", convNetModel(48000.0)},
                                   {"Linear 200 (48k)", linearModel(48000.0, 200, true, 5)},
                                   {"Linear 4096 FFT (48k)", linearModel(48000.0, 4096, false, 5)},
                                   {"WaveNet A1 std (48k)", wavenetA1Model(0.0)},
                                   {"WaveNet A1 std (44.1k)", wavenetA1Model(44100.0)},
                                   {"WaveNet A1 std (96k)", wavenetA1Model(96000.0)},
                                   {"LSTM 1x24 (44.1k)", lstmModel(44100.0, 1, 24)},
                                   {"Identity (44.1k)", identityModel(44100.0)}};
  for (const Case& c : cases) {
    std::unique_ptr<NamEngine> engine;
    if (c.core) {
      std::vector<std::unique_ptr<nam::DSP>> one;
      one.push_back(nam::get_dsp(c.model));
      engine = std::make_unique<NamEngine>(std::move(one), 1);
    } else {
      engine = engineFor(c.model);
    }
    engine->prepare(128);
    juce::AudioBuffer<float> buffer(1, 128);
    const auto in = testSignal(128 * 100, 1);
    const int blocks = 3000;
    for (int n = 0; n < 50; ++n) {
      buffer.copyFrom(0, 0, in.data() + (n % 100) * 128, 128);
      engine->process(buffer);
    }
    const auto begin = std::chrono::steady_clock::now();
    for (int n = 0; n < blocks; ++n) {
      buffer.copyFrom(0, 0, in.data() + (n % 100) * 128, 128);
      engine->process(buffer);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("[ cpu   ] %-24s %6.2f %% of one core (%.1f us per 128-sample block)\n", c.label,
                100.0 * seconds / (blocks * 128 / 48000.0), 1e6 * seconds / blocks);
  }
}
