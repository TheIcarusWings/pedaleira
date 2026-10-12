#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

// The audio-thread half of a capture run (CaptureSession.h is the state, the
// wizard steps and the commands). While a run is active the rig is muted and
// this class owns the device: it listens to the input channels it was given
// and plays on the output channel it was given.
//
//   levels  a 1 kHz test tone on the send channel (at a level the message
//           thread sets any time); the return (channel A) and, for dry/wet,
//           the second input (channel B) are metered: block peak, peak hold,
//           RMS and a count of samples at or above -0.1 dBFS. A noise floor
//           measurement mutes the tone for a moment and reads the RMS of the
//           return.
//   play    the re-amp: a signal prepared beforehand (the trainer's standard
//           input file, scaled) goes out on the send channel after a pre-roll
//           of silence, and the return is recorded from the first sample of
//           the pre-roll on. Position i of the recording is therefore the
//           device's response to position i - pre-roll - latency of what was
//           played: a recording and a signal that line up sample for sample
//           up to the device's own latency, which the data checks measure.
//   listen  dry/wet: both input channels are recorded together and nothing
//           is played.
//
// Where the channels come from: the processor's host buffer, which today is
// the main stereo pair (grid stage 8, the device channels, is not done). A
// channel index beyond the buffer falls back to the last channel it has, so
// with the stereo pair the usable values are 0 and 1. The routing struct
// carries the numbers the wizard stores in the session, so stage 8 only has
// to hand this class a wider buffer.
//
// Real-time rules, as for InputRecorder: beginBlock() and endBlock() take no
// lock, allocate nothing, do no I/O and throw nothing; while idle they cost
// one atomic read. Everything else is the message thread's. Buffers are only
// replaced while the audio thread is known to be out of them: every start
// first leaves the active mode and waits for `busy` to clear, and the
// callback raises `busy` before it looks at the mode again (both sequentially
// consistent), so the two never overlap.
class CaptureAudio {
public:
  enum class Mode { off = 0, levels, play, listen };
  enum class RunState { idle, running, full };
  enum class NoiseState { none, measuring, done };

  // Which buffer channels do what. `listenB` < 0: no second input.
  struct Routing {
    int send = 0;
    int listenA = 0;
    int listenB = -1;
  };

  // What one metered channel reads (levels in dBFS, kSilenceDb for nothing).
  struct Meter {
    float peakDb = kSilenceDb;       // the loudest sample of the last block
    float holdDb = kSilenceDb;       // the loudest sample since the last reset
    float rmsDb = kSilenceDb;        // over about 300 ms
    int clippedSamples = 0;          // at or above -0.1 dBFS, since the last reset
  };

  static constexpr float kSilenceDb = -120.0f;
  static constexpr double kToneHz = 1000.0;
  static constexpr float kMinSendDb = -80.0f;
  static constexpr float kMaxSendDb = 0.0f;
  // 20 * log10(abs(x)) at or above this counts as clipping.
  static constexpr float kClipLinear = 0.98855f;  // -0.1 dBFS
  // The noise measurement ignores the first stretch while the tone dies away.
  static constexpr double kNoiseSettleSeconds = 0.3;

  static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
  static float linearToDb(float linear) {
    return linear > 1.0e-6f ? 20.0f * std::log10(linear) : kSilenceDb;
  }

  // ---- Message thread ----

  // Levels mode: the meters run, and the tone plays when `toneOn`.
  void startLevels(double rate, Routing channels, float sendDb, bool toneOn) {
    leave();
    prepareRun(rate, channels);
    sendDbTarget.store(clampSend(sendDb));
    tone.store(toneOn);
    toneGain = 0.0f;
    tonePhase = 0.0;
    noiseRequest.store(0);
    noiseLeft = 0;
    noiseState.store(static_cast<int>(NoiseState::none));
    resetMeters();
    runState.store(static_cast<int>(RunState::running));
    mode.store(static_cast<int>(Mode::levels));
  }

  void setSendLevel(float db) { sendDbTarget.store(clampSend(db)); }
  float sendLevel() const { return sendDbTarget.load(); }
  void setToneOn(bool on) { tone.store(on); }
  bool toneOn() const { return tone.load(); }
  // Clears the peak holds and the clip counts, from the next block on.
  void resetHold() { holdReset.store(true); }

  // Levels mode: mutes the tone and reads the return's noise for `seconds`
  // (after the settling stretch). noiseState() says when it is done.
  void measureNoise(double seconds) {
    if (getMode() != Mode::levels)
      return;
    noiseState.store(static_cast<int>(NoiseState::measuring));
    noiseRequest.store(static_cast<int>((seconds + kNoiseSettleSeconds) * sampleRate));
  }
  NoiseState getNoiseState() const { return static_cast<NoiseState>(noiseState.load()); }
  float noiseRmsDb() const { return noiseRms.load(); }
  float noisePeakDb() const { return noisePeak.load(); }

  // Re-amp: plays `signal` on the send channel after `preRoll` samples of
  // silence and records the return for preRoll + signal + `tail` samples.
  // `signal` is taken as it is (the caller has scaled it).
  void startPlay(double rate, Routing channels, std::vector<float> signal, int preRoll, int tail) {
    leave();
    prepareRun(rate, channels);
    playSignal = std::move(signal);
    playPreRoll = std::max(0, preRoll);
    const size_t total = static_cast<size_t>(playPreRoll) + playSignal.size() +
                         static_cast<size_t>(std::max(0, tail));
    recordA.assign(total, 0.0f);
    recordB.clear();
    position.store(0);
    totalSamples.store(static_cast<int>(total));
    resetMeters();
    runState.store(static_cast<int>(RunState::running));
    mode.store(static_cast<int>(Mode::play));
  }

  // Dry/wet: records both input channels for `numSamples`, plays nothing.
  void startListen(double rate, Routing channels, int numSamples) {
    leave();
    prepareRun(rate, channels);
    playSignal.clear();
    playPreRoll = 0;
    const size_t total = static_cast<size_t>(std::max(1, numSamples));
    recordA.assign(total, 0.0f);
    recordB.assign(total, 0.0f);
    position.store(0);
    totalSamples.store(static_cast<int>(total));
    resetMeters();
    runState.store(static_cast<int>(RunState::running));
    mode.store(static_cast<int>(Mode::listen));
  }

  // Back to idle; what was recorded is dropped.
  void stop() {
    leave();
    runState.store(static_cast<int>(RunState::idle));
    position.store(0);
    totalSamples.store(0);
    recordA.clear();
    recordB.clear();
    playSignal.clear();
  }

  Mode getMode() const { return static_cast<Mode>(mode.load()); }
  RunState getRunState() const { return static_cast<RunState>(runState.load()); }
  int recordedSamples() const { return position.load(); }
  int plannedSamples() const { return totalSamples.load(); }
  int preRollSamples() const { return playPreRoll; }
  double rate() const { return sampleRate; }

  Meter meter(int which) const {
    const Level& level = which == 0 ? levelA : levelB;
    Meter out;
    out.peakDb = linearToDb(level.blockPeak.load());
    out.holdDb = linearToDb(level.hold.load());
    out.rmsDb = linearToDb(level.rms.load());
    out.clippedSamples = level.clipped.load();
    return out;
  }

  // Copies `count` recorded samples of channel `which` (0: A, 1: B) from
  // `from`, for a look at a recording still going on. False unless that
  // stretch has been recorded already.
  bool copyRecorded(int which, int from, int count, std::vector<float>& out) const {
    const std::vector<float>& source = which == 0 ? recordA : recordB;
    const int done = position.load();
    if (from < 0 || count <= 0 || from + count > done ||
        from + count > static_cast<int>(source.size()))
      return false;
    out.assign(source.begin() + from, source.begin() + from + count);
    return true;
  }

  // Takes the finished recording out (channel A, then B for listen) and
  // returns to idle. False unless the run is `full`.
  bool take(std::vector<float>& a, std::vector<float>& b) {
    if (getRunState() != RunState::full)
      return false;
    leave();
    a = std::move(recordA);
    b = std::move(recordB);
    recordA.clear();
    recordB.clear();
    playSignal.clear();
    runState.store(static_cast<int>(RunState::idle));
    position.store(0);
    totalSamples.store(0);
    return true;
  }

  // ---- Audio thread ----

  // Reads what comes in, then silences the buffer so the rig processes
  // nothing. True when the run is active; endBlock() must follow the rig's
  // processing of the same buffer.
  bool beginBlock(juce::AudioBuffer<float>& buffer) noexcept {
    if (mode.load(std::memory_order_relaxed) == static_cast<int>(Mode::off))
      return false;
    busy.store(true);
    const Mode now = static_cast<Mode>(mode.load());
    if (now == Mode::off || buffer.getNumChannels() <= 0) {
      busy.store(false);
      return false;
    }
    const int count = buffer.getNumSamples();
    const int last = buffer.getNumChannels() - 1;
    const int channelA = std::min(std::max(0, routing.listenA), last);
    const int channelB = routing.listenB >= 0 ? std::min(routing.listenB, last) : -1;

    if (holdReset.exchange(false)) {
      levelA.clearHold();
      levelB.clearHold();
    }

    const float* inA = buffer.getReadPointer(channelA);
    const float* inB = channelB >= 0 ? buffer.getReadPointer(channelB) : nullptr;
    levelA.measure(inA, count, rmsDecay(count));
    if (inB != nullptr)
      levelB.measure(inB, count, rmsDecay(count));

    // The noise floor: read the return while the tone is held off.
    if (now == Mode::levels) {
      if (noiseLeft <= 0) {
        const int requested = noiseRequest.exchange(0);
        if (requested > 0) {
          noiseLeft = requested;
          noiseSettle = static_cast<int>(kNoiseSettleSeconds * sampleRate);
          noiseSumSquares = 0.0;
          noiseCount = 0;
          noiseMaxAbs = 0.0f;
        }
      }
      if (noiseLeft > 0) {
        const int used = std::min(noiseLeft, count);
        for (int i = 0; i < used; ++i) {
          if (noiseSettle > 0) {
            --noiseSettle;
            continue;
          }
          const float v = inA[i];
          noiseSumSquares += static_cast<double>(v) * v;
          noiseMaxAbs = std::max(noiseMaxAbs, std::abs(v));
          ++noiseCount;
        }
        noiseLeft -= used;
        if (noiseLeft <= 0) {
          const double meanSquare =
              noiseCount > 0 ? noiseSumSquares / static_cast<double>(noiseCount) : 0.0;
          noiseRms.store(linearToDb(static_cast<float>(std::sqrt(meanSquare))));
          noisePeak.store(linearToDb(noiseMaxAbs));
          noiseState.store(static_cast<int>(NoiseState::done));
        }
      }
    } else {
      // Recording: the input goes into the buffers at the play position.
      const int at = position.load(std::memory_order_relaxed);
      const int room = totalSamples.load(std::memory_order_relaxed) - at;
      const int copy = std::min(count, std::max(0, room));
      if (copy > 0) {
        std::copy(inA, inA + copy, recordA.begin() + at);
        if (inB != nullptr && !recordB.empty())
          std::copy(inB, inB + copy, recordB.begin() + at);
      }
    }

    buffer.clear();
    return true;
  }

  // Puts what the run plays into the buffer (all other channels silent) and
  // moves the run on. Must follow a beginBlock() that returned true.
  void endBlock(juce::AudioBuffer<float>& buffer) noexcept {
    const Mode now = static_cast<Mode>(mode.load());
    const int count = buffer.getNumSamples();
    buffer.clear();
    if (now == Mode::off || buffer.getNumChannels() <= 0) {
      busy.store(false);
      return;
    }
    const int send = std::min(std::max(0, routing.send), buffer.getNumChannels() - 1);
    float* out = buffer.getWritePointer(send);

    if (now == Mode::levels) {
      const bool noiseActive = noiseLeft > 0 || noiseRequest.load(std::memory_order_relaxed) > 0;
      const float wantedGain = tone.load(std::memory_order_relaxed) && !noiseActive
                                   ? dbToLinear(sendDbTarget.load(std::memory_order_relaxed))
                                   : 0.0f;
      // About 10 ms to the new level, so a change or a switch does not click.
      const float step = static_cast<float>(1.0 / (0.010 * sampleRate));
      const double increment = 2.0 * 3.14159265358979323846 * kToneHz / sampleRate;
      for (int i = 0; i < count; ++i) {
        if (toneGain < wantedGain)
          toneGain = std::min(wantedGain, toneGain + step * std::max(wantedGain, 0.001f));
        else if (toneGain > wantedGain)
          toneGain = std::max(wantedGain, toneGain - step * std::max(toneGain, 0.001f));
        out[i] = toneGain * static_cast<float>(std::sin(tonePhase));
        tonePhase += increment;
        if (tonePhase > 6.283185307179586)
          tonePhase -= 6.283185307179586;
      }
    } else {
      const int at = position.load(std::memory_order_relaxed);
      const int total = totalSamples.load(std::memory_order_relaxed);
      if (now == Mode::play) {
        const int signalSize = static_cast<int>(playSignal.size());
        for (int i = 0; i < count; ++i) {
          const int index = at + i - playPreRoll;
          out[i] = index >= 0 && index < signalSize ? playSignal[static_cast<size_t>(index)] : 0.0f;
        }
      }
      const int next = std::min(total, at + count);
      position.store(next);
      if (next >= total) {
        runState.store(static_cast<int>(RunState::full));
        mode.store(static_cast<int>(Mode::off));
      }
    }
    busy.store(false);
  }

  // Scope guard for the processor's callback: begin on entry, end on every
  // way out of it.
  class BlockScope {
  public:
    BlockScope(CaptureAudio& engine, juce::AudioBuffer<float>& audio) noexcept
        : owner(engine), block(audio), active(engine.beginBlock(audio)) {}
    ~BlockScope() {
      if (active)
        owner.endBlock(block);
    }
    BlockScope(const BlockScope&) = delete;
    BlockScope& operator=(const BlockScope&) = delete;

  private:
    CaptureAudio& owner;
    juce::AudioBuffer<float>& block;
    bool active;
  };

private:
  struct Level {
    std::atomic<float> blockPeak{0.0f};
    std::atomic<float> hold{0.0f};
    std::atomic<float> rms{0.0f};
    std::atomic<int> clipped{0};
    // Audio thread only: the running mean square behind `rms`.
    float meanSquare = 0.0f;

    void clearHold() {
      hold.store(0.0f, std::memory_order_relaxed);
      clipped.store(0, std::memory_order_relaxed);
    }
    void measure(const float* in, int count, float decay) noexcept {
      float peak = 0.0f, sum = 0.0f;
      int over = 0;
      for (int i = 0; i < count; ++i) {
        const float a = std::abs(in[i]);
        peak = std::max(peak, a);
        sum += in[i] * in[i];
        over += a >= kClipLinear ? 1 : 0;
      }
      blockPeak.store(peak, std::memory_order_relaxed);
      if (peak > hold.load(std::memory_order_relaxed))
        hold.store(peak, std::memory_order_relaxed);
      if (over > 0)
        clipped.fetch_add(over, std::memory_order_relaxed);
      const float blockMean = count > 0 ? sum / static_cast<float>(count) : 0.0f;
      meanSquare = decay * meanSquare + (1.0f - decay) * blockMean;
      rms.store(std::sqrt(meanSquare), std::memory_order_relaxed);
    }
  };

  static float clampSend(float db) { return std::min(kMaxSendDb, std::max(kMinSendDb, db)); }

  float rmsDecay(int count) const noexcept {
    return static_cast<float>(std::exp(-static_cast<double>(count) / (0.3 * sampleRate)));
  }

  void prepareRun(double rate, Routing channels) {
    sampleRate = rate > 0.0 ? rate : 48000.0;
    routing = channels;
  }

  void resetMeters() {
    for (Level* level : {&levelA, &levelB}) {
      level->blockPeak.store(0.0f);
      level->hold.store(0.0f);
      level->rms.store(0.0f);
      level->clipped.store(0);
      level->meanSquare = 0.0f;
    }
    holdReset.store(false);
  }

  void leave() {
    mode.store(static_cast<int>(Mode::off));
    // At most one callback's work; the audio thread never blocks on us.
    while (busy.load())
      std::this_thread::yield();
  }

  std::atomic<int> mode{static_cast<int>(Mode::off)};
  std::atomic<bool> busy{false};
  std::atomic<int> runState{static_cast<int>(RunState::idle)};

  // Written by the message thread only while the audio thread is out (see
  // the class comment); read by the audio thread.
  double sampleRate = 48000.0;
  Routing routing;
  std::vector<float> playSignal, recordA, recordB;
  int playPreRoll = 0;
  std::atomic<int> position{0};
  std::atomic<int> totalSamples{0};

  Level levelA, levelB;
  std::atomic<bool> holdReset{false};

  // The tone: its target is the message thread's, the rest the audio thread's.
  std::atomic<float> sendDbTarget{-24.0f};
  std::atomic<bool> tone{false};
  float toneGain = 0.0f;
  double tonePhase = 0.0;

  // Noise floor measurement.
  std::atomic<int> noiseRequest{0};
  std::atomic<int> noiseState{static_cast<int>(NoiseState::none)};
  std::atomic<float> noiseRms{kSilenceDb};
  std::atomic<float> noisePeak{kSilenceDb};
  int noiseLeft = 0;
  int noiseSettle = 0;
  int noiseCount = 0;
  double noiseSumSquares = 0.0;
  float noiseMaxAbs = 0.0f;
};
