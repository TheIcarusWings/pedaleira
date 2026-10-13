#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>

/**
 * The looper: a loop recorded from the rig's output and played back into it,
 * at the end of the main output (after the tone stack, before the master
 * level; see TONE3000Processor::runOutputStrip). One loop, mono or stereo as
 * the chain is, up to 60 seconds, with up to four overdub layers on top of
 * the first recording. The design is written up in docs/looper.md; in short:
 *
 * Memory. prepare() (message thread) allocates kMaxLayers x 2 channels x 60
 * seconds of floats with malloc and leaves them untouched, so the pages the
 * loop does not use cost address space but no memory. Nothing is allocated,
 * freed or zeroed in bulk on the audio thread: a layer carries the range of
 * loop frames it covers, and playback only reads inside that range, so a
 * reused layer needs no clearing.
 *
 * The loop. The first recording sets the length: the number of frames
 * recorded when it ends (or the nearest whole number of beats when quantise
 * is on). Layer 0 is that recording; an overdub records into the next layer
 * and an undo takes the last one out of the sum without freeing it, so redo
 * is an index. Playback is the sum of the layers in use, times the level,
 * times a raised-cosine fade over the first and last 2 ms of the loop, so the
 * seam never clicks. The rig's own output passes through untouched; the loop
 * is added to it.
 *
 * Control. Everything that changes the loop is an Action. A non-audio thread
 * posts one with request() (a wait-free ring, serialised among producers by a
 * spin flag the audio thread never touches); the audio thread, for MIDI,
 * posts with requestFromAudioThread(). Actions are applied at the start of
 * the next process() call, in order. The options (half speed, reverse, one
 * shot, quantise, level) are atomics read once per block. The audio thread
 * publishes the state, length, position and layer counts to atomics for
 * snapshot().
 *
 * Half speed and reverse change playback only: while recording or
 * overdubbing the loop runs at normal speed, forwards, so what is written is
 * what was played.
 */
class Looper {
public:
  enum class State : int { empty = 0, recording, playing, overdubbing, stopped };

  // cycle is the single-pedal action: record, then overdub, then play and
  // overdub in turn. playStop toggles playback.
  enum class Action : int {
    record = 0,
    overdub,
    play,
    stop,
    undo,
    redo,
    clear,
    cycle,
    playStop
  };

  static constexpr double kMaxSeconds = 60.0;
  static constexpr int kMaxLayers = 5;  // the recording and four overdubs
  static constexpr int kMaxChannels = 2;
  static constexpr double kFadeSeconds = 0.002;
  static constexpr float kMinLevelDb = -60.0f;
  static constexpr float kMaxLevelDb = 12.0f;

  struct Snapshot {
    State state{State::empty};
    bool closing{false};  // the first recording is running on to the beat
    bool ready{false};
    std::int64_t lengthFrames{0};
    std::int64_t positionFrames{0};
    std::int64_t recordedFrames{0};
    std::int64_t capacityFrames{0};
    int layers{0};      // in the sum now, the recording included
    int redoLayers{0};  // taken out by undo and still there to redo
    int channels{0};
    int queued{0};
    double sampleRate{48000.0};
    bool halfSpeed{false};
    bool reverse{false};
    bool oneShot{false};
    bool quantise{false};
    float levelDb{0.0f};
  };

  Looper();

  // ---- Message thread ------------------------------------------------------
  /** (Re)allocates for `sampleRate`. Keeps a loop that is already there when
      the rate has not changed. Never call while process() runs. */
  void prepare(double sampleRate);
  /** Memory is allocated and the looper runs. */
  bool isReady() const noexcept { return ready.load(std::memory_order_acquire); }

  // ---- Any thread except the audio thread ----------------------------------
  /** Posts an action for the next block. False when the ring is full. */
  bool request(Action action) noexcept;

  // ---- Audio thread (MIDI) ---------------------------------------------------
  /** Posts an action for the next process() call of this same thread. */
  void requestFromAudioThread(Action action) noexcept;

  // ---- Any thread --------------------------------------------------------------
  void setHalfSpeed(bool on) noexcept { halfSpeedOption.store(on, std::memory_order_relaxed); }
  void setReverse(bool on) noexcept { reverseOption.store(on, std::memory_order_relaxed); }
  void setOneShot(bool on) noexcept { oneShotOption.store(on, std::memory_order_relaxed); }
  void setQuantise(bool on) noexcept { quantiseOption.store(on, std::memory_order_relaxed); }
  void setLevelDb(float db) noexcept {
    levelOption.store(juce::jlimit(kMinLevelDb, kMaxLevelDb, db), std::memory_order_relaxed);
  }

  Snapshot snapshot() const noexcept;
  /** The snapshot as the looper.get command reports it. */
  juce::var describe() const;
  static const char* stateName(State state) noexcept;

  // ---- Audio thread ------------------------------------------------------------
  /** Records from and adds playback to `data` (1 or 2 channels). `bpm` is the
      tempo of the moment, for quantise. In place, no allocation, no locks. */
  void process(float* const* data, int numChannels, int numFrames, double bpm) noexcept;

private:
  struct Options {
    bool halfSpeed;
    bool reverse;
    bool oneShot;
    bool quantise;
  };

  void applyQueued(int bufferChannels, double bpm) noexcept;
  void apply(Action action, int bufferChannels, double bpm) noexcept;
  void startRecording(int bufferChannels) noexcept;
  void requestClose(State next, double bpm) noexcept;
  void finishRecording(State next) noexcept;
  void startOverdub() noexcept;
  void endOverdub() noexcept;
  void discardOverdub() noexcept;
  void clearAll() noexcept;
  double startPosition(bool reverse) const noexcept;
  float fadeGain(std::int64_t index) const noexcept;
  void publish() noexcept;
  int layersInSum() const noexcept { return active + (state == State::overdubbing ? 1 : 0); }
  bool covered(int layer, std::int64_t index) const noexcept;
  float layerSample(int layer, int channel, std::int64_t index) const noexcept;

  // Allocated by prepare().
  struct Block {
    float* data{nullptr};
    ~Block();
    void allocate(std::size_t frames);
    void release() noexcept;
  };
  std::array<std::array<Block, kMaxChannels>, kMaxLayers> layer;
  std::int64_t capacity{0};
  double sampleRate{0.0};
  std::atomic<bool> ready{false};

  // Audio thread state.
  State state{State::empty};
  int loopChannels{2};
  std::int64_t length{0};
  double position{0.0};
  std::int64_t recorded{0};
  bool closing{false};
  std::int64_t closeAt{0};
  State closeNext{State::playing};
  int active{0};  // layers in the sum
  int used{0};    // layers holding audio, active or waiting for redo
  std::array<std::int64_t, kMaxLayers> layerStart{};
  std::array<std::int64_t, kMaxLayers> layerCount{};
  std::int64_t overdubWritten{0};
  std::int64_t fadeFrames{0};
  juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelGain;

  // The control rings.
  static constexpr int kRing = 32;
  juce::AbstractFifo fifo{kRing};
  std::array<Action, kRing> slots{};
  std::atomic_flag producerLock;
  static constexpr int kAudioPending = 16;
  std::array<Action, kAudioPending> audioPending{};
  int audioPendingCount{0};

  // Options.
  std::atomic<bool> halfSpeedOption{false};
  std::atomic<bool> reverseOption{false};
  std::atomic<bool> oneShotOption{false};
  std::atomic<bool> quantiseOption{false};
  std::atomic<float> levelOption{0.0f};

  // Published by the audio thread.
  std::atomic<int> pubState{0};
  std::atomic<bool> pubClosing{false};
  std::atomic<std::int64_t> pubLength{0};
  std::atomic<std::int64_t> pubPosition{0};
  std::atomic<std::int64_t> pubRecorded{0};
  std::atomic<int> pubLayers{0};
  std::atomic<int> pubRedo{0};
  std::atomic<int> pubChannels{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Looper)
};
