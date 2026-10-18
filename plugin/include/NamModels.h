#pragma once

#include <memory>
#include <vector>

#include <Eigen/Dense>

#include "NAM/convnet.h"
#include "NAM/dsp.h"
#include "json.hpp"

// Building a playable model out of a parsed .nam file, for every architecture
// NeuralAmpModelerCore knows: WaveNet (A1 and A2), LSTM, ConvNet, Linear and
// slimmable containers of those.
//
// WaveNet and Linear come from the core as they are: their process() is
// realtime-safe (the core's own allocation tests cover it). The core's LSTM
// and ConvNet are not: the LSTM allocates a vector per layer per sample and
// the ConvNet several matrices per call. Those two are built here instead,
// as RealtimeLstm and RealtimeConvNet: the same weight layout, the same
// arithmetic in the same order (test/src/nam_architecture_tests.cpp pins
// them bit-identical to the core's on the same input), with every buffer
// allocated when the block size is set. A container is assembled here too,
// so its submodels get the same treatment.
//
// The core reads the weights by walking an iterator and checks the count only
// afterwards (and for LSTM and ConvNet not at all), so a file with too few
// weights would be read past its end. The two architectures built here check
// the count before reading anything.
namespace NamModels {

// The rate a file says its model was trained at, or 0 when it does not say
// (files from before the trainer wrote it, or that wrote -1 for unknown).
double declaredSampleRate(const nlohmann::json& model);

// What the engine runs a model at when its file names no rate: 48 kHz, the
// rate NAM has always trained at (the official plugin assumes the same).
constexpr double kAssumedSampleRate = 48000.0;

// The rates a file may declare. Anything outside is a damaged file.
constexpr double kLowestSampleRate = 8000.0;
constexpr double kHighestSampleRate = 768000.0;

// Whether the model is pure (dilated) convolution in time, so the engine can
// run it phase-interleaved when the chain is oversampled (see NamEngine.h).
// WaveNet, ConvNet and Linear are; LSTM is recurrent and is not; a container
// is when every submodel is.
bool isPhaseSafe(const nlohmann::json& model);

// The model, ready for Reset(). Throws std::exception with a reason for a file
// that cannot be built (unknown architecture, unsupported version, settings
// or weights that do not fit together).
std::unique_ptr<nam::DSP> build(const nlohmann::json& model);

// ---- The two realtime-safe architectures ----

// A stack of LSTM cells and a linear head, as NAM trains it. The initial
// hidden and cell states are part of the weights; Reset() puts them back
// before the prewarm, so a reset model always starts from the same place.
class RealtimeLstm final : public nam::DSP {
public:
  RealtimeLstm(int numLayers, int inputSize, int hiddenSize, int inChannels, int outChannels,
               const std::vector<float>& weights, double expectedSampleRate);

  // The weight count the settings imply.
  static size_t weightCount(int numLayers, int inputSize, int hiddenSize, int outChannels);

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override;
  void Reset(double sampleRate, int maxBufferSize) override;
  // Half a second at the trained rate, as the core's LSTM.
  int GetPrewarmSamples() override;

private:
  struct Cell {
    int inputSize = 0;
    int hiddenSize = 0;
    Eigen::MatrixXf w;   // (4h) x (in + h): input, forget, cell and output gates
    Eigen::VectorXf b;   // 4h
    Eigen::VectorXf xh;  // the input, then the hidden state
    Eigen::VectorXf ifgo;
    Eigen::VectorXf c;   // the cell state
    Eigen::VectorXf initialHidden;
    Eigen::VectorXf initialCell;
  };
  void step(const Eigen::Ref<const Eigen::VectorXf>& x, Cell& cell) const;

  std::vector<Cell> cells;
  Eigen::MatrixXf headWeight;  // out x h
  Eigen::VectorXf headBias;
  Eigen::VectorXf input;
  Eigen::VectorXf output;
};

// Dilated kernel-2 convolution blocks (batch norm, activation) and a linear
// head. The blocks are the core's own ConvNetBlock (realtime-safe); what is
// replaced is the per-call plumbing between them.
class RealtimeConvNet final : public nam::DSP {
public:
  RealtimeConvNet(const nam::convnet::ConvNetConfig& config, std::vector<float>& weights,
                  double expectedSampleRate);

  static size_t weightCount(const nam::convnet::ConvNetConfig& config);

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override;
  int GetPrewarmSamples() override { return prewarmSamples; }

protected:
  void SetMaxBufferSize(int maxBufferSize) override;

private:
  void processChunk(NAM_SAMPLE** input, NAM_SAMPLE** output, int offset, int numFrames);

  std::vector<nam::convnet::ConvNetBlock> blocks;
  Eigen::MatrixXf headWeight;  // out x channels
  Eigen::VectorXf headBias;
  Eigen::MatrixXf inputFrames;  // in x max
  Eigen::MatrixXf between;      // channels x max: one block's output, the next one's input
  Eigen::MatrixXf headOutput;   // out x max
  int prewarmSamples = 1;
};

}  // namespace NamModels
