#include "NamModels.h"

#include "NAM/activations.h"
#include "NAM/container.h"
#include "NAM/get_dsp.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace NamModels {

namespace {

// Bounds on the settings a file can ask for. A capture is a few thousand to a
// few hundred thousand weights; these only keep a damaged or hostile file from
// asking for gigabytes before its weights are even counted.
constexpr int kMaxLstmLayers = 64;
constexpr int kMaxWidth = 4096;
constexpr int kMaxConvNetBlocks = 1024;
constexpr int kMaxDilation = 1 << 20;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

int intSetting(const nlohmann::json& config, const char* key, int fallback, bool required) {
  const auto found = config.find(key);
  if (found == config.end()) {
    if (required)
      fail(std::string("its settings have no ") + key);
    return fallback;
  }
  if (!found->is_number_integer())
    fail(std::string("its setting ") + key + " is not a whole number");
  return found->get<int>();
}

void requireRange(int value, int lowest, int highest, const char* what) {
  if (value < lowest || value > highest)
    fail(std::string("its ") + what + " (" + std::to_string(value) + ") is out of range");
}

std::vector<float> weightsOf(const nlohmann::json& model) {
  const auto found = model.find("weights");
  if (found == model.end() || !found->is_array())
    fail("it has no weights");
  std::vector<float> weights;
  weights.reserve(found->size());
  for (const auto& value : *found) {
    if (!value.is_number())
      fail("its weights are not all numbers");
    weights.push_back(value.get<float>());
  }
  return weights;
}

void requireWeightCount(const std::vector<float>& weights, size_t expected) {
  if (weights.size() != expected)
    fail("it holds " + std::to_string(weights.size()) + " weights where its settings need " +
         std::to_string(expected));
}

// The calibration and loudness the core's loader applies from the metadata.
void applyMetadata(nam::DSP& dsp, const nlohmann::json& model) {
  const auto metadata = model.find("metadata");
  if (metadata == model.end() || !metadata->is_object())
    return;
  auto number = [&](const char* key, double& value) {
    const auto found = metadata->find(key);
    if (found == metadata->end() || !found->is_number())
      return false;
    value = found->get<double>();
    return true;
  };
  double value = 0.0;
  if (number("loudness", value))
    dsp.SetLoudness(value);
  if (number("input_level_dbu", value))
    dsp.SetInputLevel(value);
  if (number("output_level_dbu", value))
    dsp.SetOutputLevel(value);
}

const nlohmann::json& configOf(const nlohmann::json& model) {
  const auto found = model.find("config");
  if (found == model.end() || !found->is_object())
    fail("it has no settings");
  return *found;
}

// The core builds these two itself and runs them without allocating.
bool coreBuildsRealtimeSafe(const std::string& architecture) {
  return architecture == "WaveNet" || architecture == "Linear";
}

// A container whose models (at any depth) the core already runs safely is
// handed to the core whole, exactly as before this file existed.
bool containerIsCoreSafe(const nlohmann::json& model) {
  const auto config = model.find("config");
  if (config == model.end() || !config->is_object())
    return false;
  const auto submodels = config->find("submodels");
  if (submodels == config->end() || !submodels->is_array())
    return false;
  for (const auto& entry : *submodels) {
    if (!entry.is_object() || !entry.contains("model") || !entry["model"].is_object())
      return false;
    const auto& sub = entry["model"];
    const std::string architecture = sub.value("architecture", "");
    if (architecture == "SlimmableContainer" ? !containerIsCoreSafe(sub)
                                             : !coreBuildsRealtimeSafe(architecture))
      return false;
  }
  return true;
}

double coreSampleRate(const nlohmann::json& model) {
  const auto found = model.find("sample_rate");
  return found != model.end() && found->is_number() ? found->get<double>()
                                                    : NAM_UNKNOWN_EXPECTED_SAMPLE_RATE;
}

std::unique_ptr<nam::DSP> buildLstm(const nlohmann::json& model) {
  const nlohmann::json& config = configOf(model);
  const int layers = intSetting(config, "num_layers", 0, true);
  const int inputSize = intSetting(config, "input_size", 0, true);
  const int hiddenSize = intSetting(config, "hidden_size", 0, true);
  const int inChannels = intSetting(config, "in_channels", 1, false);
  const int outChannels = intSetting(config, "out_channels", 1, false);
  requireRange(layers, 1, kMaxLstmLayers, "layer count");
  requireRange(inputSize, 1, kMaxWidth, "input size");
  requireRange(hiddenSize, 1, kMaxWidth, "hidden size");
  requireRange(outChannels, 1, kMaxWidth, "output count");
  if (inChannels != inputSize)
    fail("its input count does not match its input size");
  const std::vector<float> weights = weightsOf(model);
  requireWeightCount(weights,
                     RealtimeLstm::weightCount(layers, inputSize, hiddenSize, outChannels));
  return std::make_unique<RealtimeLstm>(layers, inputSize, hiddenSize, inChannels, outChannels,
                                        weights, coreSampleRate(model));
}

std::unique_ptr<nam::DSP> buildConvNet(const nlohmann::json& model) {
  const nlohmann::json& config = configOf(model);
  // Type-check before the core's parser reads them (it would throw a bare
  // JSON error, or take a fraction for a count).
  intSetting(config, "channels", 0, true);
  intSetting(config, "groups", 1, false);
  intSetting(config, "in_channels", 1, false);
  intSetting(config, "out_channels", 1, false);
  if (!config.contains("dilations") || !config["dilations"].is_array() ||
      config["dilations"].empty())
    fail("its settings have no dilations");
  for (const auto& dilation : config["dilations"])
    if (!dilation.is_number_integer())
      fail("its dilations are not whole numbers");
  if (!config.contains("batchnorm") || !config["batchnorm"].is_boolean())
    fail("its settings do not say whether it uses batch norm");
  if (!config.contains("activation"))
    fail("its settings name no activation");

  const nam::convnet::ConvNetConfig parsed = nam::convnet::parse_config_json(config);
  requireRange(parsed.channels, 1, kMaxWidth, "channel count");
  requireRange(parsed.in_channels, 1, kMaxWidth, "input count");
  requireRange(parsed.out_channels, 1, kMaxWidth, "output count");
  requireRange(static_cast<int>(parsed.dilations.size()), 1, kMaxConvNetBlocks, "block count");
  for (const int dilation : parsed.dilations)
    requireRange(dilation, 1, kMaxDilation, "dilation");
  requireRange(parsed.groups, 1, kMaxWidth, "group count");
  if (parsed.channels % parsed.groups != 0 || parsed.in_channels % parsed.groups != 0)
    fail("its channel counts do not divide into its groups");

  std::vector<float> weights = weightsOf(model);
  requireWeightCount(weights, RealtimeConvNet::weightCount(parsed));
  return std::make_unique<RealtimeConvNet>(parsed, weights, coreSampleRate(model));
}

std::unique_ptr<nam::DSP> buildContainer(const nlohmann::json& model) {
  const nlohmann::json& config = configOf(model);
  const auto submodels = config.find("submodels");
  if (submodels == config.end() || !submodels->is_array() || submodels->empty())
    fail("it holds no models");
  std::vector<nam::container::Submodel> built;
  built.reserve(submodels->size());
  for (const auto& entry : *submodels) {
    if (!entry.is_object() || !entry.contains("max_value") || !entry["max_value"].is_number() ||
        !entry.contains("model") || !entry["model"].is_object())
      fail("one of its models is incomplete");
    built.push_back({entry["max_value"].get<double>(), build(entry["model"])});
  }
  // Same checks as the core's container (order, coverage, one rate).
  return std::make_unique<nam::container::ContainerModel>(std::move(built), coreSampleRate(model));
}

}  // namespace

double declaredSampleRate(const nlohmann::json& model) {
  const double rate = coreSampleRate(model);
  return std::isfinite(rate) && rate > 0.0 ? rate : 0.0;
}

bool isPhaseSafe(const nlohmann::json& model) {
  const std::string architecture = model.value("architecture", "");
  if (architecture == "WaveNet" || architecture == "ConvNet" || architecture == "Linear")
    return true;
  if (architecture == "SlimmableContainer") {
    // Safe only when every submodel is (containers switch tiers at runtime).
    if (!model.contains("config") || !model["config"].is_object() ||
        !model["config"].contains("submodels"))
      return false;
    const auto& submodels = model["config"]["submodels"];
    if (!submodels.is_array() || submodels.empty())
      return false;
    for (const auto& entry : submodels)
      if (!entry.is_object() || !entry.contains("model") || !isPhaseSafe(entry["model"]))
        return false;
    return true;
  }
  return false;  // LSTM and anything unknown
}

std::unique_ptr<nam::DSP> build(const nlohmann::json& model) {
  if (!model.is_object())
    fail("it is not a NAM model");
  const auto version = model.find("version");
  if (version == model.end() || !version->is_string())
    fail("it does not say which NAM version saved it");
  nam::verify_config_version(version->get<std::string>());
  const auto rate = model.find("sample_rate");
  if (rate != model.end() && !rate->is_number())
    fail("its sample rate is not a number");
  if (const double declared = declaredSampleRate(model);
      declared > 0.0 && (declared < kLowestSampleRate || declared > kHighestSampleRate))
    fail("its sample rate (" + std::to_string(static_cast<long long>(declared)) +
         " Hz) is out of range");

  const std::string architecture = model.value("architecture", "");
  std::unique_ptr<nam::DSP> dsp;
  if (coreBuildsRealtimeSafe(architecture) ||
      (architecture == "SlimmableContainer" && containerIsCoreSafe(model))) {
    dsp = nam::get_dsp(model);
    if (dsp == nullptr)
      fail("the NAM core could not build it");
    return dsp;  // the core applied the metadata
  }
  if (architecture == "LSTM")
    dsp = buildLstm(model);
  else if (architecture == "ConvNet")
    dsp = buildConvNet(model);
  else if (architecture == "SlimmableContainer")
    dsp = buildContainer(model);
  else
    fail("its architecture (" + architecture + ") is not one NAM knows");
  applyMetadata(*dsp, model);
  return dsp;
}

// ####################
// LSTM
// ####################

size_t RealtimeLstm::weightCount(int numLayers, int inputSize, int hiddenSize, int outChannels) {
  const size_t h = static_cast<size_t>(hiddenSize);
  size_t count = 0;
  for (int layer = 0; layer < numLayers; ++layer) {
    const size_t in = layer == 0 ? static_cast<size_t>(inputSize) : h;
    count += 4 * h * (in + h)  // gate weights
             + 4 * h           // gate biases
             + h + h;          // initial hidden and cell states
  }
  return count + static_cast<size_t>(outChannels) * h + static_cast<size_t>(outChannels);
}

RealtimeLstm::RealtimeLstm(int numLayers, int inputSize, int hiddenSize, int inChannels,
                           int outChannels, const std::vector<float>& weights,
                           double expectedSampleRate)
    : nam::DSP(inChannels, outChannels, expectedSampleRate) {
  if (weights.size() != weightCount(numLayers, inputSize, hiddenSize, outChannels))
    throw std::runtime_error("LSTM weight count does not match its settings");
  input.setZero(inputSize);
  output.setZero(outChannels);

  // The core's layout: per layer the gate matrix row by row, the biases, the
  // initial hidden state and the initial cell state; then the head.
  auto it = weights.begin();
  cells.resize(static_cast<size_t>(numLayers));
  for (int layer = 0; layer < numLayers; ++layer) {
    Cell& cell = cells[static_cast<size_t>(layer)];
    cell.inputSize = layer == 0 ? inputSize : hiddenSize;
    cell.hiddenSize = hiddenSize;
    cell.w.resize(4 * hiddenSize, cell.inputSize + hiddenSize);
    cell.b.resize(4 * hiddenSize);
    cell.xh.setZero(cell.inputSize + hiddenSize);
    cell.ifgo.setZero(4 * hiddenSize);
    cell.c.resize(hiddenSize);
    cell.initialHidden.resize(hiddenSize);
    cell.initialCell.resize(hiddenSize);
    for (Eigen::Index i = 0; i < cell.w.rows(); ++i)
      for (Eigen::Index j = 0; j < cell.w.cols(); ++j)
        cell.w(i, j) = *it++;
    for (Eigen::Index i = 0; i < cell.b.size(); ++i)
      cell.b(i) = *it++;
    for (int i = 0; i < hiddenSize; ++i)
      cell.initialHidden(i) = *it++;
    for (int i = 0; i < hiddenSize; ++i)
      cell.initialCell(i) = *it++;
    cell.xh.tail(hiddenSize) = cell.initialHidden;
    cell.c = cell.initialCell;
  }
  headWeight.resize(outChannels, hiddenSize);
  for (int o = 0; o < outChannels; ++o)
    for (int h = 0; h < hiddenSize; ++h)
      headWeight(o, h) = *it++;
  headBias.resize(outChannels);
  for (int o = 0; o < outChannels; ++o)
    headBias(o) = *it++;
}

void RealtimeLstm::Reset(double sampleRate, int maxBufferSize) {
  for (Cell& cell : cells) {
    cell.xh.setZero();
    cell.xh.tail(cell.hiddenSize) = cell.initialHidden;
    cell.c = cell.initialCell;
  }
  nam::DSP::Reset(sampleRate, maxBufferSize);
}

int RealtimeLstm::GetPrewarmSamples() {
  const int samples = static_cast<int>(0.5 * mExpectedSampleRate);
  return samples <= 0 ? 1 : samples;
}

// One cell, one sample: the core's LSTMCell::process_, expression for
// expression, with its temporaries in preallocated storage.
void RealtimeLstm::step(const Eigen::Ref<const Eigen::VectorXf>& x, Cell& cell) const {
  const Eigen::Index h = cell.hiddenSize;
  cell.xh.head(cell.inputSize) = x;
  cell.ifgo.noalias() = cell.w * cell.xh;
  cell.ifgo += cell.b;
  const Eigen::Index iOffset = 0;
  const Eigen::Index fOffset = h;
  const Eigen::Index gOffset = 2 * h;
  const Eigen::Index oOffset = 3 * h;
  const Eigen::Index hOffset = cell.inputSize;
  if (nam::activations::Activation::using_fast_tanh) {
    for (Eigen::Index i = 0; i < h; ++i)
      cell.c[i] = nam::activations::fast_sigmoid(cell.ifgo[i + fOffset]) * cell.c[i] +
                  nam::activations::fast_sigmoid(cell.ifgo[i + iOffset]) *
                      nam::activations::fast_tanh(cell.ifgo[i + gOffset]);
    for (Eigen::Index i = 0; i < h; ++i)
      cell.xh[i + hOffset] = nam::activations::fast_sigmoid(cell.ifgo[i + oOffset]) *
                             nam::activations::fast_tanh(cell.c[i]);
  } else {
    for (Eigen::Index i = 0; i < h; ++i)
      cell.c[i] = nam::activations::sigmoid(cell.ifgo[i + fOffset]) * cell.c[i] +
                  nam::activations::sigmoid(cell.ifgo[i + iOffset]) * tanhf(cell.ifgo[i + gOffset]);
    for (Eigen::Index i = 0; i < h; ++i)
      cell.xh[i + hOffset] =
          nam::activations::sigmoid(cell.ifgo[i + oOffset]) * tanhf(cell.c[i]);
  }
}

void RealtimeLstm::process(NAM_SAMPLE** in, NAM_SAMPLE** out, int numFrames) {
  const int inChannels = NumInputChannels();
  const int outChannels = NumOutputChannels();
  for (int i = 0; i < numFrames; ++i) {
    for (int ch = 0; ch < inChannels; ++ch)
      input(ch) = static_cast<float>(in[ch][i]);
    step(input, cells.front());
    for (size_t layer = 1; layer < cells.size(); ++layer) {
      const Cell& below = cells[layer - 1];
      step(below.xh.tail(below.hiddenSize), cells[layer]);
    }
    const Cell& top = cells.back();
    output.noalias() = headWeight * top.xh.tail(top.hiddenSize);
    output += headBias;
    for (int ch = 0; ch < outChannels; ++ch)
      out[ch][i] = output(ch);
  }
}

// ####################
// ConvNet
// ####################

size_t RealtimeConvNet::weightCount(const nam::convnet::ConvNetConfig& config) {
  const size_t channels = static_cast<size_t>(config.channels);
  const size_t groups = static_cast<size_t>(config.groups);
  size_t count = 0;
  for (size_t i = 0; i < config.dilations.size(); ++i) {
    const size_t in = i == 0 ? static_cast<size_t>(config.in_channels) : channels;
    count += 2 * channels * (in / groups);  // kernel size 2
    if (config.batchnorm)
      count += 4 * channels + 1;  // mean, variance, weight, bias, epsilon
    else
      count += channels;  // the convolution's bias
  }
  const size_t out = static_cast<size_t>(config.out_channels);
  return count + out * channels + out;
}

RealtimeConvNet::RealtimeConvNet(const nam::convnet::ConvNetConfig& config,
                                 std::vector<float>& weights, double expectedSampleRate)
    : nam::DSP(config.in_channels, config.out_channels, expectedSampleRate) {
  if (weights.size() != weightCount(config))
    throw std::runtime_error("ConvNet weight count does not match its settings");
  auto it = weights.begin();
  blocks.resize(config.dilations.size());
  for (size_t i = 0; i < config.dilations.size(); ++i)
    blocks[i].set_weights_(i == 0 ? config.in_channels : config.channels, config.channels,
                           config.dilations[i], config.batchnorm, config.activation, config.groups,
                           it);
  headWeight.resize(config.out_channels, config.channels);
  for (int o = 0; o < config.out_channels; ++o)
    for (int c = 0; c < config.channels; ++c)
      headWeight(o, c) = *it++;
  headBias.resize(config.out_channels);
  for (int o = 0; o < config.out_channels; ++o)
    headBias(o) = *it++;

  prewarmSamples = 1;
  for (const int dilation : config.dilations)
    prewarmSamples += dilation;
}

void RealtimeConvNet::SetMaxBufferSize(int maxBufferSize) {
  nam::DSP::SetMaxBufferSize(maxBufferSize);
  for (auto& block : blocks)
    block.SetMaxBufferSize(maxBufferSize);
  inputFrames.setZero(NumInputChannels(), maxBufferSize);
  between.setZero(headWeight.cols(), maxBufferSize);
  headOutput.setZero(NumOutputChannels(), maxBufferSize);
}

void RealtimeConvNet::process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) {
  if (mMaxBufferSize <= 0)
    SetMaxBufferSize(numFrames);  // used without a Reset: not on the audio thread
  // Longer calls go through in prepared-size pieces; the blocks keep their
  // own history, so this is the same as one call.
  for (int offset = 0; offset < numFrames; offset += mMaxBufferSize)
    processChunk(input, output, offset, std::min(mMaxBufferSize, numFrames - offset));
}

// The core's ConvNet::process, with its per-call matrices replaced by the
// ones SetMaxBufferSize made.
void RealtimeConvNet::processChunk(NAM_SAMPLE** input, NAM_SAMPLE** output, int offset,
                                   int numFrames) {
  const int inChannels = NumInputChannels();
  const int outChannels = NumOutputChannels();
  for (int ch = 0; ch < inChannels; ++ch)
    for (int i = 0; i < numFrames; ++i)
      inputFrames(ch, i) = static_cast<float>(input[ch][offset + i]);

  blocks.front().Process(inputFrames, numFrames);
  for (size_t i = 1; i < blocks.size(); ++i) {
    between.leftCols(numFrames) = blocks[i - 1].GetOutput(numFrames);
    blocks[i].Process(between, numFrames);
  }

  headOutput.leftCols(numFrames).noalias() = headWeight * blocks.back().GetOutput(numFrames);
  headOutput.leftCols(numFrames).colwise() += headBias;
  for (int ch = 0; ch < outChannels; ++ch)
    for (int i = 0; i < numFrames; ++i)
      output[ch][offset + i] = headOutput(ch, i);
}

}  // namespace NamModels
