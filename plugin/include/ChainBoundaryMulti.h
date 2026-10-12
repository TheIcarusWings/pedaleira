#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

#include "ChainDomain.h"

// Multi-port chain boundary: the host-rate to chain-rate round trip of
// ChainBoundaryResampler (see ChainDomain.h), with several stereo ports on one
// frame schedule.
//
//   up-port    host rate -> chain rate, one per input strip
//   down-port  chain rate -> host rate, one per output bus
//
// Every port owns one Lanczos resampler of exactly the type the single-port
// container uses, and process() runs the container's push/pop loop over all
// of them. How many chain frames a callback gets depends only on the two
// rates and on the host frame counts seen so far, never on the audio, so the
// ports cannot drift apart: each callback hands every port the same number of
// chain frames, and every port has the same delay.
//
// With one up-port and one down-port the output is bit-identical to
// dsp::ResamplingContainer<float, 2, 12> for the same rates, block sizes and
// input, including the blocks right after a reset (pinned by
// test/src/chain_boundary_multi_tests.cpp). That holds because the arithmetic
// is the container's own resampler class and the calls are made in the same
// order. Two things are deliberately NOT copied from the container:
//
//  * Its Reset() warm-up reads its silence scratch and writes its chain-rate
//    buffers past their logical size when the block size is small against the
//    warm-up length (for example 32 frames at 96 kHz), so what it warms up
//    with there is whatever the heap held. This class sizes those buffers for
//    the warm-up and always warms up with real silence, which is what the
//    container does whenever it stays in bounds.
//  * It reports a short final pop on std::cerr. That is not realtime-safe, so
//    it is counted instead (underrunCount()).
//
// One container quirk IS copied, because the processor relies on the current
// behaviour: reset() with an unchanged configuration only clears the audio
// history. The resampler phases carry on from where the stream stopped, as
// they do in the container.
//
// Like the container, this is only meant to be instantiated when the host rate
// differs from the chain base rate; at 48 kHz the processor skips the boundary.
class ChainBoundaryMulti {
public:
  static constexpr int kMaxPorts = 4;
  static constexpr int kChannelsPerPort = 2;

  /** The resampler the single-port container is built from. Using its alias
      keeps both on one template instantiation (and one kernel table). */
  using Resampler = ChainBoundaryResampler::LanczosResampler;

  ChainBoundaryMulti() = default;
  ChainBoundaryMulti(const ChainBoundaryMulti&) = delete;
  ChainBoundaryMulti& operator=(const ChainBoundaryMulti&) = delete;

  /** Configure the boundary and warm it up with silence. `maxHostBlock` is the
      largest host frame count per process() call; port counts are 1 to
      kMaxPorts. Calling it again with identical arguments only clears the
      audio history (see the class comment). Returns false, leaving the
      boundary unprepared, if the arguments are out of range.
      Allocates (about 2 MB per port); not realtime-safe. */
  bool reset(double newHostRate, double newChainRate, int newMaxHostBlock, int newNumUpPorts,
             int newNumDownPorts) {
    return configure(newHostRate, newChainRate, newMaxHostBlock, newNumUpPorts, newNumDownPorts,
                     true);
  }

  bool isPrepared() const { return prepared; }
  int numUpPorts() const { return numUp; }
  int numDownPorts() const { return numDown; }

  /** The most chain-rate frames a single callback can be handed. */
  int maxChainFrames() const { return maxChainBlock; }

  /** Physical delay of the round trip in host samples, the same for every
      port: the position of the peak of an impulse sent through an identity
      callback. This is the number the processor reports to the host today
      (measureChainBoundaryLatency in Processor.cpp), measured the same way. */
  int latencySamples() const { return measuredLatency; }

  /** The silent prefill pushed at reset, in host samples: what the
      container's GetLatency() returns. It is short of latencySamples() by the
      group delay of the two Lanczos kernels, so do not report it to a host. */
  int warmUpSamples() const { return warmUpLatency; }

  /** Times a down-port could not fill a host block and its last sample was
      held instead. Stays 0 in correct use; a non-zero value means the
      schedule broke. Written on the audio thread, so read it from there or
      from a test. */
  int underrunCount() const { return underruns; }

  /** Push `numHostFrames` from every up-port, run `callback` on each chain-rate
      block that becomes available, and pop `numHostFrames` into every
      down-port.

        hostInputs[upPort][channel]     numHostFrames each, read only
        hostOutputs[downPort][channel]  numHostFrames each
        callback(float* const* const* chainInputs,   // [upPort][channel]
                 float* const* const* chainOutputs,  // [downPort][channel]
                 int numChainFrames)

      The callback runs zero or more times per call, each time with at most
      maxChainFrames() frames, and must write every down-port (the chain
      buffers keep their previous contents otherwise). It may scribble on the
      chain inputs. Host inputs and outputs may be the same buffers: all
      inputs are consumed before any output is written.

      A host block longer than the prepared maximum is fed in prepared-size
      slices instead of overrunning the chain buffers.
      Realtime-safe after reset(): no allocation, no locks. */
  template <typename Fn>
  void process(const float* const* const* hostInputs, float* const* const* hostOutputs,
               int numHostFrames, Fn&& callback) {
    assert(prepared);
    if (!prepared || numHostFrames <= 0)
      return;

    PortPointers ins{};
    PortPointers outs{};
    for (int offset = 0; offset < numHostFrames; offset += maxHostBlock) {
      const int remaining = numHostFrames - offset;
      const int sliceLen = remaining < maxHostBlock ? remaining : maxHostBlock;
      for (int p = 0; p < numUp; ++p)
        for (int ch = 0; ch < kChannelsPerPort; ++ch)
          // The resampler's push takes non-const pointers but only reads.
          ins[idx(p)][idx(ch)] = const_cast<float*>(hostInputs[p][ch]) + offset;
      for (int p = 0; p < numDown; ++p)
        for (int ch = 0; ch < kChannelsPerPort; ++ch)
          outs[idx(p)][idx(ch)] = hostOutputs[p][ch] + offset;
      processSlice(ins, outs, sliceLen, callback);
    }
  }

  /** The measurement behind latencySamples(), usable without an instance.
      Allocates and runs audio; not realtime-safe. Returns 0 on bad arguments. */
  static int measureLatency(double hostRate, double chainRate, int blockSize) {
    ChainBoundaryMulti probe;
    if (!probe.configure(hostRate, chainRate, blockSize, 1, 1, false))
      return 0;

    // Long enough to hold the impulse response whatever the block size, in
    // whole blocks so every process() call is a full one.
    const int total = ((probe.warmUpSamples() + 2 * blockSize) / blockSize + 1) * blockSize;
    std::vector<float> in(static_cast<size_t>(total) * kChannelsPerPort, 0.0f);
    std::vector<float> out(static_cast<size_t>(total) * kChannelsPerPort, 0.0f);
    float* const inChannels[kChannelsPerPort] = {in.data(), in.data() + total};
    float* const outChannels[kChannelsPerPort] = {out.data(), out.data() + total};
    inChannels[0][0] = 1.0f;
    inChannels[1][0] = 1.0f;

    auto identity = [](float* const* const* chainIn, float* const* const* chainOut, int frames) {
      for (int ch = 0; ch < kChannelsPerPort; ++ch)
        for (int i = 0; i < frames; ++i)
          chainOut[0][ch][i] = chainIn[0][ch][i];
    };
    for (int offset = 0; offset < total; offset += blockSize) {
      const float* const blockIn[kChannelsPerPort] = {inChannels[0] + offset,
                                                      inChannels[1] + offset};
      float* const blockOut[kChannelsPerPort] = {outChannels[0] + offset, outChannels[1] + offset};
      const float* const* const inPorts[1] = {blockIn};
      float* const* const outPorts[1] = {blockOut};
      probe.process(inPorts, outPorts, blockSize, identity);
    }

    int peak = 0;
    float best = 0.0f;
    for (int i = 0; i < total; ++i) {
      const float magnitude = std::abs(outChannels[0][i]);
      if (magnitude > best) {
        best = magnitude;
        peak = i;
      }
    }
    return peak;
  }

private:
  using ChannelPointers = std::array<float*, kChannelsPerPort>;
  using PortPointers = std::array<ChannelPointers, kMaxPorts>;

  struct Port {
    std::unique_ptr<Resampler> resampler;
    // Chain-rate block for this port, channel-major.
    std::vector<float> chainData;
    ChannelPointers chainChannels{};
  };

  static size_t idx(int i) { return static_cast<size_t>(i); }

  bool configure(double newHostRate, double newChainRate, int newMaxHostBlock, int newNumUpPorts,
                 int newNumDownPorts, bool measure) {
    const bool valid = newHostRate > 0.0 && newChainRate > 0.0 && newMaxHostBlock >= 1 &&
                       newNumUpPorts >= 1 && newNumUpPorts <= kMaxPorts && newNumDownPorts >= 1 &&
                       newNumDownPorts <= kMaxPorts;
    assert(valid);
    if (!valid) {
      prepared = false;
      return false;
    }

    // Same early-out as the container's Reset(): the phases are kept.
    if (prepared && newHostRate == hostRate && newChainRate == chainRate &&
        newMaxHostBlock == maxHostBlock && newNumUpPorts == numUp && newNumDownPorts == numDown) {
      clearBuffers();
      return true;
    }

    prepared = false;
    hostRate = newHostRate;
    chainRate = newChainRate;
    maxHostBlock = newMaxHostBlock;
    numUp = newNumUpPorts;
    numDown = newNumDownPorts;
    hostFramesPerChainFrame = hostRate / chainRate;
    maxChainBlock = chainFramesFor(maxHostBlock);
    underruns = 0;

    for (int p = 0; p < kMaxPorts; ++p) {
      upPorts[idx(p)].resampler.reset();
      downPorts[idx(p)].resampler.reset();
    }
    // The resampler takes its rates as float, as in the container.
    for (int p = 0; p < numUp; ++p)
      upPorts[idx(p)].resampler = std::make_unique<Resampler>(static_cast<float>(hostRate),
                                                              static_cast<float>(chainRate));
    for (int p = 0; p < numDown; ++p)
      downPorts[idx(p)].resampler = std::make_unique<Resampler>(static_cast<float>(chainRate),
                                                                static_cast<float>(hostRate));

    // Warm-up, as in the container: enough silence through the up side that
    // the down side can already yield one host sample, so the first real
    // block comes back full length.
    const size_t midSamples = downPorts[0].resampler->GetNumSamplesRequiredFor(1);
    warmUpLatency = static_cast<int>(upPorts[0].resampler->GetNumSamplesRequiredFor(midSamples));

    // The warm-up can be longer than one block (see the class comment), so
    // the buffers it uses are sized for whichever is larger.
    const size_t chainCapacity =
        midSamples > static_cast<size_t>(maxChainBlock) ? midSamples : idx(maxChainBlock);
    const size_t silenceFrames = warmUpLatency > maxHostBlock ? idx(warmUpLatency)
                                                              : idx(maxHostBlock);
    silence.assign(silenceFrames, 0.0f);
    for (int p = 0; p < kMaxPorts; ++p) {
      const bool upUsed = p < numUp;
      const bool downUsed = p < numDown;
      allocatePort(upPorts[idx(p)], upUsed ? chainCapacity : 0);
      allocatePort(downPorts[idx(p)], downUsed ? chainCapacity : 0);
      chainInputPorts[idx(p)] = upUsed ? upPorts[idx(p)].chainChannels.data() : nullptr;
      chainOutputPorts[idx(p)] = downUsed ? downPorts[idx(p)].chainChannels.data() : nullptr;
    }

    ChannelPointers silencePointers{};
    silencePointers.fill(silence.data());
    size_t populated = midSamples;
    for (int p = 0; p < numUp; ++p) {
      Port& port = upPorts[idx(p)];
      port.resampler->PushBlock(silencePointers.data(), idx(warmUpLatency));
      const size_t got = port.resampler->PopBlock(port.chainChannels.data(), midSamples);
      if (got < populated)
        populated = got;
    }
    assert(populated == midSamples);
    if (populated < midSamples)
      return false;

    // The chain is causal and silent on silence, so the warm-up skips the
    // callback and feeds the down side silence directly.
    for (int p = 0; p < numDown; ++p)
      downPorts[idx(p)].resampler->PushBlock(silencePointers.data(), populated);

    prepared = true;
    measuredLatency = measure ? measureLatency(hostRate, chainRate, maxHostBlock) : 0;
    return true;
  }

  static void allocatePort(Port& port, size_t capacity) {
    port.chainData.assign(capacity * kChannelsPerPort, 0.0f);
    for (int ch = 0; ch < kChannelsPerPort; ++ch)
      port.chainChannels[idx(ch)] = capacity > 0 ? port.chainData.data() + idx(ch) * capacity
                                                 : nullptr;
  }

  void clearBuffers() {
    for (int p = 0; p < kMaxPorts; ++p) {
      for (Port* port : {&upPorts[idx(p)], &downPorts[idx(p)]}) {
        for (float& sample : port->chainData)
          sample = 0.0f;
        if (port->resampler != nullptr)
          port->resampler->ClearBuffer();
      }
    }
  }

  // The most chain frames `hostFrames` host frames can turn into.
  int chainFramesFor(int hostFrames) const {
    return static_cast<int>(std::ceil(static_cast<double>(hostFrames) / hostFramesPerChainFrame));
  }

  template <typename Fn>
  void processSlice(PortPointers& ins, PortPointers& outs, int numHostFrames, Fn& callback) {
    for (int p = 0; p < numUp; ++p)
      upPorts[idx(p)].resampler->PushBlock(ins[idx(p)].data(), idx(numHostFrames));

    const size_t maxChainLen = idx(chainFramesFor(numHostFrames));

    // Up-port 0 answers "is there more to process" for everyone: the phase
    // arithmetic is integer and identical in every up-resampler.
    Resampler& clock = *upPorts[0].resampler;
    while (clock.GetNumSamplesRequiredFor(1) == 0) {
      const size_t populated = clock.PopBlock(upPorts[0].chainChannels.data(), maxChainLen);
      for (int p = 1; p < numUp; ++p) {
        Port& port = upPorts[idx(p)];
        [[maybe_unused]] const size_t got =
            port.resampler->PopBlock(port.chainChannels.data(), maxChainLen);
        assert(got == populated);
      }
      // Cannot happen while the condition above holds; guards the loop
      // against spinning on the audio thread if it ever did.
      if (populated == 0)
        break;

      callback(static_cast<float* const* const*>(chainInputPorts.data()),
               static_cast<float* const* const*>(chainOutputPorts.data()),
               static_cast<int>(populated));

      for (int p = 0; p < numDown; ++p) {
        Port& port = downPorts[idx(p)];
        port.resampler->PushBlock(port.chainChannels.data(), populated);
      }
    }

    for (int p = 0; p < numDown; ++p) {
      const int populated = static_cast<int>(
          downPorts[idx(p)].resampler->PopBlock(outs[idx(p)].data(), idx(numHostFrames)));
      if (populated < numHostFrames) {
        // Same recovery as the container: hold the last sample.
        ++underruns;
        for (int ch = 0; ch < kChannelsPerPort; ++ch) {
          float* out = outs[idx(p)][idx(ch)];
          const float lastSample = populated > 0 ? out[populated - 1] : 0.0f;
          for (int i = populated; i < numHostFrames; ++i)
            out[i] = lastSample;
        }
      }
    }

    for (int p = 0; p < numUp; ++p)
      upPorts[idx(p)].resampler->RenormalizePhases();
    for (int p = 0; p < numDown; ++p)
      downPorts[idx(p)].resampler->RenormalizePhases();
  }

  bool prepared = false;
  double hostRate = 0.0;
  double chainRate = 0.0;
  // Host frames per chain frame (the container's mRatio1).
  double hostFramesPerChainFrame = 1.0;
  int maxHostBlock = 0;
  int maxChainBlock = 0;
  int numUp = 0;
  int numDown = 0;
  int warmUpLatency = 0;
  int measuredLatency = 0;
  int underruns = 0;

  std::array<Port, kMaxPorts> upPorts;
  std::array<Port, kMaxPorts> downPorts;
  // What the callback receives: [port][channel] over the ports' chain blocks.
  std::array<float**, kMaxPorts> chainInputPorts{};
  std::array<float**, kMaxPorts> chainOutputPorts{};
  // Zeros for the warm-up push.
  std::vector<float> silence;
};
