// Multi-port chain boundary and oversampler halves (grid design stage 3a,
// spike S0b).
//
//   ChainBoundaryMultiTest   one up-port and one down-port are bit-identical
//                            to the single-port container the processor uses
//                            today (output, callback schedule, latency), and
//                            several ports neither interact nor drift.
//   ChainOversamplerHalves   processUp + chain + processDown is process(),
//                            bit for bit, and instances sharing one callback
//                            stay aligned.
//
// "Bit-identical" means memcmp on the float samples; no tolerance anywhere.
// The reference is always the existing code (dsp::ResamplingContainer and
// ChainOversampler::process), never a second copy of the new code.
#include "ChainBoundaryMulti.h"
#include "ChainDomain.h"
#include "ChainOversampler.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double kChainRate = kChainBaseSampleRate;
constexpr double kHostRates[] = {44100.0, 88200.0, 96000.0, 192000.0};
constexpr int kTotalFrames = 6000;

using Stereo = std::array<std::vector<float>, 2>;

// Noise plus a sine, different per seed and per channel, so a swapped port or
// channel can never compare equal by accident.
Stereo makeSignal(int numFrames, std::uint32_t seed) {
  Stereo s;
  for (int ch = 0; ch < 2; ++ch) {
    std::uint32_t state = seed * 2654435761u + static_cast<std::uint32_t>(ch) * 40503u + 1u;
    const double freq = 0.011 + 0.007 * static_cast<double>(seed) + 0.003 * ch;
    s[static_cast<size_t>(ch)].resize(static_cast<size_t>(numFrames));
    for (int i = 0; i < numFrames; ++i) {
      state = state * 1664525u + 1013904223u;
      const float noise = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
      const float tone = static_cast<float>(std::sin(6.283185307179586 * freq * i));
      s[static_cast<size_t>(ch)][static_cast<size_t>(i)] = 0.4f * noise + 0.5f * tone;
    }
  }
  return s;
}

Stereo makeSilence(int numFrames) {
  Stereo s;
  for (auto& ch : s)
    ch.assign(static_cast<size_t>(numFrames), 0.0f);
  return s;
}

bool bitIdentical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool bitIdentical(const Stereo& a, const Stereo& b) {
  return bitIdentical(a[0], b[0]) && bitIdentical(a[1], b[1]);
}

float maxAbsDifference(const Stereo& a, const Stereo& b) {
  float worst = 0.0f;
  for (size_t ch = 0; ch < 2; ++ch) {
    if (a[ch].size() != b[ch].size())
      return 1.0e30f;
    for (size_t i = 0; i < a[ch].size(); ++i) {
      const float d = std::abs(a[ch][i] - b[ch][i]);
      if (d > worst)
        worst = d;
    }
  }
  return worst;
}

int peakIndex(const std::vector<float>& x) {
  int peak = 0;
  float best = 0.0f;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::abs(x[i]) > best) {
      best = std::abs(x[i]);
      peak = static_cast<int>(i);
    }
  return peak;
}

// A stand-in for the chain: a waveshaper into a one-pole filter. It is
// nonlinear and stateful on purpose, so an output only matches the reference
// if the chain saw the same samples in the same order and block boundaries
// never dropped or repeated a frame. Kept out of line so every caller runs
// the very same machine code.
struct ChainFx {
  explicit ChainFx(float driveIn = 1.3f) : drive(driveIn) {}
  float drive;
  float z[2] = {0.0f, 0.0f};
};

void applyFx(ChainFx& fx, const float* const* in, float* const* out, int frames) {
  for (int ch = 0; ch < 2; ++ch) {
    float z = fx.z[ch];
    for (int i = 0; i < frames; ++i) {
      const float s = in[ch][i] * fx.drive;
      const float shaped = s - 0.15f * s * s * s;
      z += 0.35f * (shaped - z);
      out[ch][i] = z + 0.25f * shaped;
    }
    fx.z[ch] = z;
  }
}

// Block sizes summing to `total`, cycling through `pattern`.
std::vector<int> blockSchedule(int total, const std::vector<int>& pattern) {
  std::vector<int> blocks;
  size_t next = 0;
  for (int done = 0; done < total;) {
    int n = pattern[next++ % pattern.size()];
    if (n > total - done)
      n = total - done;
    blocks.push_back(n);
    done += n;
  }
  return blocks;
}

struct BlockCase {
  std::string name;
  int maxBlock;
  std::vector<int> pattern;
};

std::vector<BlockCase> blockCases() {
  return {{"32", 32, {32}},
          {"100", 100, {100}},
          {"128", 128, {128}},
          {"512", 512, {512}},
          {"varying", 512, {64, 1, 512, 7, 100, 33, 128, 2, 480, 256, 31}}};
}

// The container's Reset() warm-up pushes GetLatency() frames from a scratch
// buffer of `block` frames per channel and pops the warm-up into chain buffers
// sized for one block. When the warm-up is longer than a block it runs past
// both, so what it warms up with is uninitialised heap and the reference is
// not a defined signal at all. This says whether a Reset(hostRate, block)
// stays inside its buffers.
bool containerWarmUpStaysInBounds(double hostRate, int block) {
  auto down = std::make_unique<ChainBoundaryMulti::Resampler>(static_cast<float>(kChainRate),
                                                              static_cast<float>(hostRate));
  auto up = std::make_unique<ChainBoundaryMulti::Resampler>(static_cast<float>(hostRate),
                                                            static_cast<float>(kChainRate));
  const size_t mid = down->GetNumSamplesRequiredFor(1);
  const size_t warmUp = up->GetNumSamplesRequiredFor(mid);
  const size_t chainBlock =
      static_cast<size_t>(std::ceil(static_cast<double>(block) / (hostRate / kChainRate)));
  return warmUp <= static_cast<size_t>(block) && mid <= chainBlock;
}

// The block size to Reset() the reference container with. Its processing does
// not depend on that size beyond buffer capacity (the warm-up length and the
// per-block schedule come from the rates and the frame counts), so where the
// real size would send the warm-up out of bounds a larger one gives the same
// computation with defined inputs.
int referenceResetBlock(double hostRate, int block) {
  return containerWarmUpStaysInBounds(hostRate, block) ? block : 4096;
}

using ContainerFn = std::function<void(float**, float**, int)>;

// Streams `in` through the container in place, the way processBlock does
// (ProcessBlock(channels, channels, ...)).
Stereo runContainer(ChainBoundaryResampler& container, const Stereo& in,
                    const std::vector<int>& blocks, const ContainerFn& fn) {
  Stereo io = in;
  int offset = 0;
  for (int n : blocks) {
    float* channels[2] = {io[0].data() + offset, io[1].data() + offset};
    container.ProcessBlock(channels, channels, n, fn);
    offset += n;
  }
  return io;
}

// Streams one signal per up-port through the multi-port boundary. With
// `inPlace` the host outputs are the host inputs (needs as many down-ports
// as up-ports).
template <typename Fn>
std::vector<Stereo> runMulti(ChainBoundaryMulti& boundary, const std::vector<Stereo>& ins,
                             int numDown, const std::vector<int>& blocks, bool inPlace, Fn&& fn) {
  const int numUp = static_cast<int>(ins.size());
  const int total = static_cast<int>(ins[0][0].size());
  std::vector<Stereo> inCopy = ins;
  std::vector<Stereo> separate(static_cast<size_t>(numDown), makeSilence(total));
  std::vector<Stereo>& outs = inPlace ? inCopy : separate;

  int offset = 0;
  for (int n : blocks) {
    std::array<std::array<const float*, 2>, ChainBoundaryMulti::kMaxPorts> inChannels{};
    std::array<std::array<float*, 2>, ChainBoundaryMulti::kMaxPorts> outChannels{};
    std::array<const float* const*, ChainBoundaryMulti::kMaxPorts> inPorts{};
    std::array<float* const*, ChainBoundaryMulti::kMaxPorts> outPorts{};
    for (int p = 0; p < numUp; ++p) {
      const auto port = static_cast<size_t>(p);
      inChannels[port] = {inCopy[port][0].data() + offset, inCopy[port][1].data() + offset};
      inPorts[port] = inChannels[port].data();
    }
    for (int p = 0; p < numDown; ++p) {
      const auto port = static_cast<size_t>(p);
      outChannels[port] = {outs[port][0].data() + offset, outs[port][1].data() + offset};
      outPorts[port] = outChannels[port].data();
    }
    boundary.process(inPorts.data(), outPorts.data(), n, fn);
    offset += n;
  }
  return outs;
}

// measureChainBoundaryLatency from Processor.cpp, on the container: the
// number the processor reports to the host today.
int referenceMeasuredLatency(double hostRate, int blockSize) {
  ChainBoundaryResampler probe(kChainRate);
  probe.Reset(hostRate, referenceResetBlock(hostRate, blockSize));

  const int total = ((probe.GetLatency() + 2 * blockSize) / blockSize + 1) * blockSize;
  Stereo in = makeSilence(total);
  in[0][0] = 1.0f;
  in[1][0] = 1.0f;
  const ContainerFn identity = [](float** inputs, float** outputs, int frames) {
    std::memcpy(outputs[0], inputs[0], sizeof(float) * static_cast<size_t>(frames));
    std::memcpy(outputs[1], inputs[1], sizeof(float) * static_cast<size_t>(frames));
  };
  Stereo out = makeSilence(total);
  for (int offset = 0; offset < total; offset += blockSize) {
    float* ins[2] = {in[0].data() + offset, in[1].data() + offset};
    float* outs[2] = {out[0].data() + offset, out[1].data() + offset};
    probe.ProcessBlock(ins, outs, blockSize, identity);
  }
  return peakIndex(out[0]);
}

// What one port of the boundary must produce for `in` when the chain is a
// ChainFx with this drive: the existing container, on its own.
Stereo containerReference(double hostRate, int maxBlock, const std::vector<int>& blocks,
                          const Stereo& in, float drive,
                          std::vector<int>* schedule = nullptr) {
  ChainBoundaryResampler container(kChainRate);
  container.Reset(hostRate, referenceResetBlock(hostRate, maxBlock));
  ChainFx fx(drive);
  const ContainerFn fn = [&fx, schedule](float** inputs, float** outputs, int frames) {
    applyFx(fx, inputs, outputs, frames);
    if (schedule != nullptr)
      schedule->push_back(frames);
  };
  return runContainer(container, in, blocks, fn);
}

float driveForPort(int port) { return 0.9f + 0.4f * static_cast<float>(port); }

} // namespace

// ============================================================================
// Port 0 alone is the existing container
// ============================================================================

TEST(ChainBoundaryMultiTest, SinglePortIsBitIdenticalToTheContainer) {
  for (double hostRate : kHostRates) {
    for (const BlockCase& bc : blockCases()) {
      SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", block " + bc.name);
      const std::vector<int> blocks = blockSchedule(kTotalFrames, bc.pattern);
      const Stereo in = makeSignal(kTotalFrames, 1);

      std::vector<int> refSchedule;
      const Stereo ref = containerReference(hostRate, bc.maxBlock, blocks, in, 1.3f, &refSchedule);

      ChainBoundaryMulti boundary;
      ASSERT_TRUE(boundary.reset(hostRate, kChainRate, bc.maxBlock, 1, 1));
      ChainFx fx(1.3f);
      std::vector<int> schedule;
      schedule.reserve(refSchedule.size());
      const std::vector<Stereo> got = runMulti(
          boundary, {in}, 1, blocks, true,
          [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
            EXPECT_LE(frames, boundary.maxChainFrames());
            applyFx(fx, chainIn[0], chainOut[0], frames);
            schedule.push_back(frames);
          });

      const float worst = maxAbsDifference(got[0], ref);
      EXPECT_TRUE(bitIdentical(got[0], ref)) << "max difference " << worst;
      // The first blocks after the reset are where a warm-up mistake shows.
      EXPECT_EQ(0, std::memcmp(got[0][0].data(), ref[0].data(), sizeof(float) * 256));
      EXPECT_EQ(schedule, refSchedule);
      EXPECT_EQ(boundary.underrunCount(), 0);

      std::cout << "[ evidence ] boundary " << hostRate << " Hz, block " << bc.name
                << (containerWarmUpStaysInBounds(hostRate, bc.maxBlock)
                        ? ""
                        : " (reference reset with a larger block, see test)")
                << ": " << kTotalFrames << " frames x 2 ch, " << schedule.size()
                << " callbacks, max difference " << worst << "\n";
    }
  }
}

TEST(ChainBoundaryMultiTest, LatencyMatchesTheContainer) {
  for (double hostRate : kHostRates) {
    for (int block : {32, 100, 128, 512}) {
      SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", block " + std::to_string(block));
      ChainBoundaryResampler container(kChainRate);
      container.Reset(hostRate, referenceResetBlock(hostRate, block));

      ChainBoundaryMulti boundary;
      ASSERT_TRUE(boundary.reset(hostRate, kChainRate, block, 1, 1));
      EXPECT_EQ(boundary.warmUpSamples(), container.GetLatency());
      const int reference = referenceMeasuredLatency(hostRate, block);
      EXPECT_EQ(boundary.latencySamples(), reference);
      EXPECT_EQ(ChainBoundaryMulti::measureLatency(hostRate, kChainRate, block), reference);

      // More ports do not change the delay.
      ChainBoundaryMulti wide;
      ASSERT_TRUE(wide.reset(hostRate, kChainRate, block, 4, 3));
      EXPECT_EQ(wide.latencySamples(), reference);

      if (block == 128)
        std::cout << "[ evidence ] latency " << hostRate << " Hz: measured "
                  << boundary.latencySamples() << " (container " << reference << "), warm-up "
                  << boundary.warmUpSamples() << " (container " << container.GetLatency() << ")\n";
    }
  }
}

// Reset() with an unchanged configuration only clears the audio history in
// the container (the phases carry on). The processor does exactly that on
// every prepareToPlay at an unchanged rate, so it has to match too, as does
// a real reconfiguration to another rate and back.
TEST(ChainBoundaryMultiTest, RepeatedAndChangedResetsMatchTheContainer) {
  const int block = 128;
  const std::vector<int> blocks = blockSchedule(1000, {block, 37});
  const Stereo in = makeSignal(1000, 5);

  ChainBoundaryResampler container(kChainRate);
  ChainBoundaryMulti boundary;
  // 1000 frames is not a whole number of resampler periods, so the second
  // pass at each rate starts from a non-zero phase.
  const double rates[] = {44100.0, 44100.0, 96000.0, 96000.0, 44100.0, 192000.0, 192000.0};
  int step = 0;
  for (double hostRate : rates) {
    SCOPED_TRACE("step " + std::to_string(step++) + ", host rate " + std::to_string(hostRate));
    container.Reset(hostRate, block);
    ASSERT_TRUE(boundary.reset(hostRate, kChainRate, block, 1, 1));
    ASSERT_TRUE(containerWarmUpStaysInBounds(hostRate, block));

    ChainFx refFx, fx;
    const ContainerFn refFn = [&refFx](float** inputs, float** outputs, int frames) {
      applyFx(refFx, inputs, outputs, frames);
    };
    const Stereo ref = runContainer(container, in, blocks, refFn);
    const std::vector<Stereo> got = runMulti(
        boundary, {in}, 1, blocks, true,
        [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
          applyFx(fx, chainIn[0], chainOut[0], frames);
        });
    EXPECT_TRUE(bitIdentical(got[0], ref)) << "max difference " << maxAbsDifference(got[0], ref);
  }
}

TEST(ChainBoundaryMultiTest, OversizedHostBlockIsFedInSlices) {
  const Stereo in = makeSignal(900, 3);
  auto run = [&](const std::vector<int>& blocks) {
    ChainBoundaryMulti boundary;
    EXPECT_TRUE(boundary.reset(88200.0, kChainRate, 128, 1, 1));
    ChainFx fx;
    return runMulti(boundary, {in}, 1, blocks, false,
                    [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
                      EXPECT_LE(frames, boundary.maxChainFrames());
                      applyFx(fx, chainIn[0], chainOut[0], frames);
                    })[0];
  };
  // 300 is over the prepared 128 and must behave as 128 + 128 + 44.
  const Stereo whole = run({300, 300, 300});
  const Stereo sliced = run({128, 128, 44, 128, 128, 44, 128, 128, 44});
  EXPECT_TRUE(bitIdentical(whole, sliced));
}

TEST(ChainBoundaryMultiTest, RejectsBadConfigurations) {
#ifdef NDEBUG // the same cases assert in a debug build
  ChainBoundaryMulti boundary;
  EXPECT_FALSE(boundary.reset(44100.0, kChainRate, 128, 0, 1));
  EXPECT_FALSE(boundary.reset(44100.0, kChainRate, 128, 1, ChainBoundaryMulti::kMaxPorts + 1));
  EXPECT_FALSE(boundary.reset(44100.0, kChainRate, 0, 1, 1));
  EXPECT_FALSE(boundary.isPrepared());
#endif
}

// ============================================================================
// Several ports
// ============================================================================

// Every port carries its own signal through its own chain. Each must come out
// exactly as the existing single-port container would have produced it alone.
TEST(ChainBoundaryMultiTest, PortsDoNotInteract) {
  for (double hostRate : kHostRates) {
    for (const BlockCase& bc : blockCases()) {
      if (bc.name != "100" && bc.name != "varying")
        continue;
      for (int numPorts : {2, 4}) {
        SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", block " + bc.name + ", ports " +
                     std::to_string(numPorts));
        const std::vector<int> blocks = blockSchedule(kTotalFrames, bc.pattern);
        std::vector<Stereo> ins;
        std::vector<ChainFx> fx;
        for (int p = 0; p < numPorts; ++p) {
          ins.push_back(makeSignal(kTotalFrames, 10 + static_cast<std::uint32_t>(p)));
          fx.emplace_back(driveForPort(p));
        }

        ChainBoundaryMulti boundary;
        ASSERT_TRUE(boundary.reset(hostRate, kChainRate, bc.maxBlock, numPorts, numPorts));
        const std::vector<Stereo> got = runMulti(
            boundary, ins, numPorts, blocks, true,
            [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
              for (int p = 0; p < numPorts; ++p)
                applyFx(fx[static_cast<size_t>(p)], chainIn[p], chainOut[p], frames);
            });

        float worst = 0.0f;
        for (int p = 0; p < numPorts; ++p) {
          const auto port = static_cast<size_t>(p);
          const Stereo ref =
              containerReference(hostRate, bc.maxBlock, blocks, ins[port], driveForPort(p));
          EXPECT_TRUE(bitIdentical(got[port], ref)) << "port " << p;
          const float d = maxAbsDifference(got[port], ref);
          if (d > worst)
            worst = d;
        }
        EXPECT_EQ(boundary.underrunCount(), 0);
        std::cout << "[ evidence ] " << numPorts << " ports " << hostRate << " Hz, block "
                  << bc.name << ": every port equals its own container, max difference " << worst
                  << "\n";
      }
    }
  }
}

// A signal on one port leaves the others at exact digital silence.
TEST(ChainBoundaryMultiTest, SilentPortsStaySilent) {
  const std::vector<int> blocks = blockSchedule(2000, {100});
  std::vector<Stereo> ins(4, makeSilence(2000));
  ins[2] = makeSignal(2000, 7);

  ChainBoundaryMulti boundary;
  ASSERT_TRUE(boundary.reset(44100.0, kChainRate, 100, 4, 4));
  const std::vector<Stereo> got =
      runMulti(boundary, ins, 4, blocks, false,
               [](float* const* const* chainIn, float* const* const* chainOut, int frames) {
                 for (int p = 0; p < 4; ++p)
                   for (int ch = 0; ch < 2; ++ch)
                     std::memcpy(chainOut[p][ch], chainIn[p][ch],
                                 sizeof(float) * static_cast<size_t>(frames));
               });
  for (int p : {0, 1, 3})
    for (int ch = 0; ch < 2; ++ch)
      for (float sample : got[static_cast<size_t>(p)][static_cast<size_t>(ch)])
        ASSERT_EQ(sample, 0.0f) << "port " << p;
  EXPECT_GT(std::abs(got[2][0][static_cast<size_t>(peakIndex(got[2][0]))]), 0.1f);
}

// The same audio on every port comes out the same on every port, and an
// impulse lands on the reported latency on all of them.
TEST(ChainBoundaryMultiTest, PortsStaySampleAligned) {
  for (double hostRate : kHostRates) {
    for (const BlockCase& bc : blockCases()) {
      SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", block " + bc.name);
      const int total = 3000;
      const std::vector<int> blocks = blockSchedule(total, bc.pattern);
      // A whole number of resampler periods (147 host frames at 44.1k and
      // 88.2k, 2 at 96k, 4 at 192k), so the impulse meets the same fractional
      // phase as the frame-0 impulse the latency was measured with. Off that
      // grid the peak of this (and of the container's) round trip can sit one
      // sample later: the true delay is not a whole number of host samples.
      const int impulseAt = 588;

      for (bool impulse : {true, false}) {
        Stereo signal = makeSignal(total, 21);
        if (impulse) {
          signal = makeSilence(total);
          signal[0][static_cast<size_t>(impulseAt)] = 1.0f;
          signal[1][static_cast<size_t>(impulseAt)] = 1.0f;
        }
        const std::vector<Stereo> ins(4, signal);

        ChainBoundaryMulti boundary;
        ASSERT_TRUE(boundary.reset(hostRate, kChainRate, bc.maxBlock, 4, 4));
        const std::vector<Stereo> got = runMulti(
            boundary, ins, 4, blocks, false,
            [](float* const* const* chainIn, float* const* const* chainOut, int frames) {
              // Crossed over, so alignment is checked across port pairs
              // rather than only within each port.
              for (int p = 0; p < 4; ++p)
                for (int ch = 0; ch < 2; ++ch)
                  std::memcpy(chainOut[3 - p][ch], chainIn[p][ch],
                              sizeof(float) * static_cast<size_t>(frames));
            });

        for (size_t p = 1; p < 4; ++p)
          EXPECT_TRUE(bitIdentical(got[p], got[0])) << "port " << p;
        if (impulse)
          for (size_t p = 0; p < 4; ++p)
            EXPECT_EQ(peakIndex(got[p][0]), impulseAt + boundary.latencySamples()) << "port " << p;
      }
    }
  }
}

// One input strip feeding three output buses: each bus is what the container
// gives for that bus's chain on the one input.
TEST(ChainBoundaryMultiTest, OneUpPortThreeDownPorts) {
  for (double hostRate : kHostRates) {
    for (const BlockCase& bc : blockCases()) {
      if (bc.name != "128" && bc.name != "varying")
        continue;
      SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", block " + bc.name);
      const std::vector<int> blocks = blockSchedule(kTotalFrames, bc.pattern);
      const Stereo in = makeSignal(kTotalFrames, 31);

      ChainBoundaryMulti boundary;
      ASSERT_TRUE(boundary.reset(hostRate, kChainRate, bc.maxBlock, 1, 3));
      EXPECT_EQ(boundary.numUpPorts(), 1);
      EXPECT_EQ(boundary.numDownPorts(), 3);
      std::vector<ChainFx> fx;
      for (int q = 0; q < 3; ++q)
        fx.emplace_back(driveForPort(q));
      const std::vector<Stereo> got = runMulti(
          boundary, {in}, 3, blocks, false,
          [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
            for (int q = 0; q < 3; ++q)
              applyFx(fx[static_cast<size_t>(q)], chainIn[0], chainOut[q], frames);
          });

      for (int q = 0; q < 3; ++q) {
        const Stereo ref = containerReference(hostRate, bc.maxBlock, blocks, in, driveForPort(q));
        EXPECT_TRUE(bitIdentical(got[static_cast<size_t>(q)], ref))
            << "down-port " << q << ", max difference "
            << maxAbsDifference(got[static_cast<size_t>(q)], ref);
      }
      EXPECT_EQ(boundary.underrunCount(), 0);
    }
  }
}

// Three input strips summed at the chain rate into one output bus. The
// reference is built from containers only: two of them just record what
// their up side hands the chain, and a third sums those recordings into its
// own chain block. That only works if all three containers run the same
// callback schedule, which is the property the multi-port class relies on.
TEST(ChainBoundaryMultiTest, ThreeUpPortsOneDownPort) {
  for (double hostRate : kHostRates) {
    for (const BlockCase& bc : blockCases()) {
      if (bc.name != "128" && bc.name != "varying")
        continue;
      SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", block " + bc.name);
      const std::vector<int> blocks = blockSchedule(kTotalFrames, bc.pattern);
      std::vector<Stereo> ins;
      for (std::uint32_t p = 0; p < 3; ++p)
        ins.push_back(makeSignal(kTotalFrames, 40 + p));

      auto sumInto = [](ChainFx& fx, const float* const* a, const float* const* b,
                        const float* const* c, float* const* out, int frames) {
        for (int ch = 0; ch < 2; ++ch)
          for (int i = 0; i < frames; ++i)
            out[ch][i] = (a[ch][i] + b[ch][i]) + c[ch][i];
        applyFx(fx, out, out, frames);
      };

      // Reference.
      Stereo recorded[2] = {Stereo{}, Stereo{}};
      for (size_t p = 0; p < 2; ++p) {
        ChainBoundaryResampler recorder(kChainRate);
        recorder.Reset(hostRate, referenceResetBlock(hostRate, bc.maxBlock));
        Stereo& tape = recorded[p];
        const ContainerFn record = [&tape](float** inputs, float** outputs, int frames) {
          for (size_t ch = 0; ch < 2; ++ch) {
            tape[ch].insert(tape[ch].end(), inputs[ch], inputs[ch] + frames);
            std::memcpy(outputs[ch], inputs[ch], sizeof(float) * static_cast<size_t>(frames));
          }
        };
        runContainer(recorder, ins[p + 1], blocks, record);
      }
      ChainBoundaryResampler container(kChainRate);
      container.Reset(hostRate, referenceResetBlock(hostRate, bc.maxBlock));
      ChainFx refFx;
      size_t tapePos = 0;
      const ContainerFn refFn = [&](float** inputs, float** outputs, int frames) {
        ASSERT_LE(tapePos + static_cast<size_t>(frames), recorded[0][0].size());
        const float* b[2] = {recorded[0][0].data() + tapePos, recorded[0][1].data() + tapePos};
        const float* c[2] = {recorded[1][0].data() + tapePos, recorded[1][1].data() + tapePos};
        sumInto(refFx, inputs, b, c, outputs, frames);
        tapePos += static_cast<size_t>(frames);
      };
      const Stereo ref = runContainer(container, ins[0], blocks, refFn);

      // Multi-port.
      ChainBoundaryMulti boundary;
      ASSERT_TRUE(boundary.reset(hostRate, kChainRate, bc.maxBlock, 3, 1));
      ChainFx fx;
      const std::vector<Stereo> got = runMulti(
          boundary, ins, 1, blocks, false,
          [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
            sumInto(fx, chainIn[0], chainIn[1], chainIn[2], chainOut[0], frames);
          });

      EXPECT_TRUE(bitIdentical(got[0], ref)) << "max difference " << maxAbsDifference(got[0], ref);
      EXPECT_EQ(boundary.underrunCount(), 0);
    }
  }
}

// ============================================================================
// Oversampler halves
// ============================================================================

namespace {

constexpr int kOsFactors[] = {1, 2, 4, 8};

// The chain-stage contract of the direct path: process in place on `outputs`,
// moving the audio across first when the pointers differ.
void chainStage(ChainFx& fx, float** inputs, float** outputs, int frames) {
  applyFx(fx, inputs, outputs, frames);
}

// Reference: today's ChainOversampler::process, in place on the host block.
Stereo runOversamplerProcess(int factor, int maxBlock, const Stereo& in,
                             const std::vector<int>& blocks, float drive) {
  ChainOversampler os;
  os.prepare(factor, maxBlock);
  ChainFx fx(drive);
  Stereo io = in;
  int offset = 0;
  for (int n : blocks) {
    float* channels[2] = {io[0].data() + offset, io[1].data() + offset};
    os.process(channels, channels, n, [&](float** ins, float** outs, int frames) {
      EXPECT_EQ(frames, n * factor);
      chainStage(fx, ins, outs, frames);
    });
    offset += n;
  }
  return io;
}

} // namespace

TEST(ChainOversamplerHalves, UpChainDownIsBitIdenticalToProcess) {
  const int total = 4000;
  const std::vector<BlockCase> cases = {{"1", 1, {1}},
                                        {"64", 64, {64}},
                                        {"100", 100, {100}},
                                        {"512", 512, {512}},
                                        {"varying", 512, {64, 1, 512, 7, 100, 33, 128, 2}}};
  for (int factor : kOsFactors) {
    for (const BlockCase& bc : cases) {
      for (bool mono : {false, true}) {
        // Mono is how the processor runs a mono chain through the 2-channel
        // oversampler: the signal on channel 0 and a silent second channel.
        Stereo in = makeSignal(total, 50);
        if (mono)
          in[1].assign(in[1].size(), 0.0f);
        const std::vector<int> blocks =
            blockSchedule(bc.name == "1" ? 300 : total, bc.pattern);
        const int frames = bc.name == "1" ? 300 : total;
        in[0].resize(static_cast<size_t>(frames));
        in[1].resize(static_cast<size_t>(frames));
        SCOPED_TRACE("factor " + std::to_string(factor) + ", block " + bc.name +
                     (mono ? ", mono" : ", stereo"));

        const Stereo ref = runOversamplerProcess(factor, bc.maxBlock, in, blocks, 1.3f);

        // Variant A: the instance's own chain buffer, host block in place.
        // Variant B: caller-owned chain buffers and a separate output block.
        float worst = 0.0f;
        for (bool ownBuffers : {false, true}) {
          ChainOversampler os;
          os.prepare(factor, bc.maxBlock);
          ChainFx fx(1.3f);
          Stereo io = in;
          Stereo out = makeSilence(frames);
          Stereo scratch = makeSilence(bc.maxBlock * factor);
          int offset = 0;
          for (int n : blocks) {
            float* hostIn[2] = {io[0].data() + offset, io[1].data() + offset};
            float* hostOut[2] = {out[0].data() + offset, out[1].data() + offset};
            float* scratchPointers[2] = {scratch[0].data(), scratch[1].data()};
            float* const* chain = ownBuffers ? scratchPointers : os.chainBuffer();

            const int chainFrames = os.processUp(hostIn, chain, n);
            ASSERT_EQ(chainFrames, n * factor);
            float* chainChannels[2] = {chain[0], chain[1]};
            chainStage(fx, chainChannels, chainChannels, chainFrames);
            os.processDown(chain, ownBuffers ? hostOut : hostIn, chainFrames);
            offset += n;
          }
          const Stereo& got = ownBuffers ? out : io;
          EXPECT_TRUE(bitIdentical(got, ref))
              << (ownBuffers ? "caller-owned buffers" : "own chain buffer") << ", max difference "
              << maxAbsDifference(got, ref);
          const float d = maxAbsDifference(got, ref);
          if (d > worst)
            worst = d;
        }
        if (bc.name == "varying" || bc.name == "100")
          std::cout << "[ evidence ] oversampler x" << factor << ", block " << bc.name
                    << (mono ? ", mono" : ", stereo") << ": max difference " << worst << "\n";
      }
    }
  }
}

// Mixing the halves and process() on one instance is the same stream: they
// share one filter state.
TEST(ChainOversamplerHalves, HalvesAndProcessShareOneState) {
  for (int factor : kOsFactors) {
    SCOPED_TRACE("factor " + std::to_string(factor));
    const int total = 2000;
    const Stereo in = makeSignal(total, 51);
    const std::vector<int> blocks = blockSchedule(total, {100, 37, 256});
    const Stereo ref = runOversamplerProcess(factor, 256, in, blocks, 1.3f);

    ChainOversampler os;
    os.prepare(factor, 256);
    ChainFx fx(1.3f);
    Stereo io = in;
    int offset = 0;
    bool useHalves = false;
    for (int n : blocks) {
      float* channels[2] = {io[0].data() + offset, io[1].data() + offset};
      if (useHalves) {
        const int chainFrames = os.processUp(channels, os.chainBuffer(), n);
        float* chain[2] = {os.chainBuffer()[0], os.chainBuffer()[1]};
        chainStage(fx, chain, chain, chainFrames);
        os.processDown(os.chainBuffer(), channels, chainFrames);
      } else {
        os.process(channels, channels, n, [&](float** ins, float** outs, int frames) {
          chainStage(fx, ins, outs, frames);
        });
      }
      useHalves = !useHalves;
      offset += n;
    }
    EXPECT_TRUE(bitIdentical(io, ref));
  }
}

// Two instances raised together, one shared chain-rate callback, lowered
// together: each is what it would be alone, and the same audio through both
// stays identical sample for sample.
TEST(ChainOversamplerHalves, TwoInstancesSideBySideStayAligned) {
  const int total = 3000;
  const std::vector<int> blocks = blockSchedule(total, {64, 1, 512, 7, 100});
  for (int factor : kOsFactors) {
    for (bool sameSignal : {false, true}) {
      SCOPED_TRACE("factor " + std::to_string(factor) + (sameSignal ? ", same" : ", different"));
      Stereo in[2] = {makeSignal(total, 60), makeSignal(total, sameSignal ? 60u : 61u)};
      if (sameSignal) {
        // An impulse on top, so alignment is also visible as a peak position.
        for (auto& s : in) {
          s[0][1500] += 4.0f;
          s[1][1500] += 4.0f;
        }
      }
      const float drives[2] = {1.3f, sameSignal ? 1.3f : 0.8f};

      ChainOversampler os[2];
      ChainFx fx[2] = {ChainFx(drives[0]), ChainFx(drives[1])};
      for (auto& o : os)
        o.prepare(factor, 512);
      Stereo io[2] = {in[0], in[1]};
      int offset = 0;
      for (int n : blocks) {
        int chainFrames[2] = {0, 0};
        for (size_t k = 0; k < 2; ++k) {
          float* channels[2] = {io[k][0].data() + offset, io[k][1].data() + offset};
          chainFrames[k] = os[k].processUp(channels, os[k].chainBuffer(), n);
        }
        // The shared callback: one frame count for both.
        ASSERT_EQ(chainFrames[0], chainFrames[1]);
        for (size_t k = 0; k < 2; ++k) {
          float* chain[2] = {os[k].chainBuffer()[0], os[k].chainBuffer()[1]};
          chainStage(fx[k], chain, chain, chainFrames[0]);
        }
        for (size_t k = 0; k < 2; ++k) {
          float* channels[2] = {io[k][0].data() + offset, io[k][1].data() + offset};
          os[k].processDown(os[k].chainBuffer(), channels, chainFrames[0]);
        }
        offset += n;
      }

      for (size_t k = 0; k < 2; ++k)
        EXPECT_TRUE(
            bitIdentical(io[k], runOversamplerProcess(factor, 512, in[k], blocks, drives[k])))
            << "instance " << k;
      if (sameSignal) {
        EXPECT_TRUE(bitIdentical(io[0], io[1]));
        EXPECT_EQ(peakIndex(io[0][0]), peakIndex(io[1][0]));
      }
    }
  }
}

// ============================================================================
// Both layers together, the way stage 3b will nest them
// ============================================================================

// Reference: the processor's nesting today, container -> ChainOversampler::
// process -> chain, one instance of everything per signal. New code: one
// multi-port boundary whose single callback raises every port, runs every
// chain and lowers every port.
TEST(ChainBoundaryMultiTest, WithOversamplerHalvesMatchesTodaysNesting) {
  const int numPorts = 3;
  for (double hostRate : kHostRates) {
    for (int factor : kOsFactors) {
      for (const BlockCase& bc : blockCases()) {
        if (bc.name != "128" && bc.name != "varying")
          continue;
        SCOPED_TRACE("host rate " + std::to_string(hostRate) + ", factor " +
                     std::to_string(factor) + ", block " + bc.name);
        const std::vector<int> blocks = blockSchedule(kTotalFrames, bc.pattern);
        std::vector<Stereo> ins;
        for (int p = 0; p < numPorts; ++p)
          ins.push_back(makeSignal(kTotalFrames, 70 + static_cast<std::uint32_t>(p)));

        ChainBoundaryMulti boundary;
        ASSERT_TRUE(boundary.reset(hostRate, kChainRate, bc.maxBlock, numPorts, numPorts));
        std::vector<ChainOversampler> os(static_cast<size_t>(numPorts));
        std::vector<ChainFx> fx;
        for (int p = 0; p < numPorts; ++p) {
          os[static_cast<size_t>(p)].prepare(factor, boundary.maxChainFrames());
          fx.emplace_back(driveForPort(p));
        }
        const std::vector<Stereo> got = runMulti(
            boundary, ins, numPorts, blocks, true,
            [&](float* const* const* chainIn, float* const* const* chainOut, int frames) {
              int chainFrames = 0;
              for (int p = 0; p < numPorts; ++p) {
                ChainOversampler& o = os[static_cast<size_t>(p)];
                chainFrames = o.processUp(chainIn[p], o.chainBuffer(), frames);
              }
              for (int p = 0; p < numPorts; ++p) {
                ChainOversampler& o = os[static_cast<size_t>(p)];
                float* chain[2] = {o.chainBuffer()[0], o.chainBuffer()[1]};
                chainStage(fx[static_cast<size_t>(p)], chain, chain, chainFrames);
              }
              for (int p = 0; p < numPorts; ++p) {
                ChainOversampler& o = os[static_cast<size_t>(p)];
                o.processDown(o.chainBuffer(), chainOut[p], chainFrames);
              }
            });

        float worst = 0.0f;
        for (int p = 0; p < numPorts; ++p) {
          ChainBoundaryResampler container(kChainRate);
          const int resetBlock = referenceResetBlock(hostRate, bc.maxBlock);
          container.Reset(hostRate, resetBlock);
          ChainOversampler refOs;
          refOs.prepare(factor, static_cast<int>(std::ceil(resetBlock / (hostRate / kChainRate))));
          ChainFx refFx(driveForPort(p));
          const ContainerFn refFn = [&](float** inputs, float** outputs, int frames) {
            refOs.process(inputs, outputs, frames, [&](float** ci, float** co, int chainFrames) {
              chainStage(refFx, ci, co, chainFrames);
            });
          };
          const Stereo ref = runContainer(container, ins[static_cast<size_t>(p)], blocks, refFn);
          EXPECT_TRUE(bitIdentical(got[static_cast<size_t>(p)], ref)) << "port " << p;
          const float d = maxAbsDifference(got[static_cast<size_t>(p)], ref);
          if (d > worst)
            worst = d;
        }
        std::cout << "[ evidence ] boundary + oversampler x" << factor << " at " << hostRate
                  << " Hz, block " << bc.name << ", " << numPorts
                  << " ports: max difference " << worst << "\n";
      }
    }
  }
}
