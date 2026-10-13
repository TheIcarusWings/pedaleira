#include "Looper.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>

// ####################
// MEMORY
// ####################
Looper::Block::~Block() {
  release();
}

void Looper::Block::allocate(std::size_t frames) {
  release();
  // malloc, left uninitialised on purpose: nothing reads a frame before it is
  // written (a layer is read only inside the range it covers), and a large
  // block's pages are only committed when they are first written, so layers
  // the loop never uses cost address space and nothing more. Zeroing them
  // would commit them all.
  data = static_cast<float*>(std::malloc(frames * sizeof(float)));
  if (data == nullptr)
    throw std::bad_alloc();
}

void Looper::Block::release() noexcept {
  std::free(data);
  data = nullptr;
}

Looper::Looper() {
  producerLock.clear();
  levelGain.reset(48000.0, 0.02);
  levelGain.setCurrentAndTargetValue(1.0f);
}

void Looper::prepare(double newSampleRate) {
  if (!(newSampleRate > 0.0))
    return;
  const bool unchanged = ready.load(std::memory_order_acquire) &&
                         std::abs(newSampleRate - sampleRate) < 0.5;
  if (!unchanged) {
    ready.store(false, std::memory_order_release);
    const auto frames = static_cast<std::int64_t>(std::ceil(kMaxSeconds * newSampleRate));
    try {
      for (auto& channels : layer)
        for (auto& block : channels)
          block.allocate(static_cast<std::size_t>(frames));
    } catch (const std::bad_alloc&) {
      for (auto& channels : layer)
        for (auto& block : channels)
          block.release();
      capacity = 0;
      publish();
      return;
    }
    capacity = frames;
    sampleRate = newSampleRate;
    clearAll();
    fifo.reset();
    audioPendingCount = 0;
  }
  const float level = levelOption.load(std::memory_order_relaxed);
  levelGain.reset(sampleRate, 0.02);
  levelGain.setCurrentAndTargetValue(level <= kMinLevelDb ? 0.0f
                                                          : juce::Decibels::decibelsToGain(level));
  ready.store(true, std::memory_order_release);
  publish();
}

// ####################
// CONTROL
// ####################
bool Looper::request(Action action) noexcept {
  // Producers (the message thread, a command server's calls once they have
  // reached it) take turns on a flag; the audio thread never touches it.
  while (producerLock.test_and_set(std::memory_order_acquire)) {
  }
  bool accepted = false;
  {
    const auto scope = fifo.write(1);
    if (scope.blockSize1 > 0) {
      slots[static_cast<std::size_t>(scope.startIndex1)] = action;
      accepted = true;
    }
  }
  producerLock.clear(std::memory_order_release);
  return accepted;
}

void Looper::requestFromAudioThread(Action action) noexcept {
  if (audioPendingCount < kAudioPending)
    audioPending[static_cast<std::size_t>(audioPendingCount++)] = action;
}

void Looper::applyQueued(int bufferChannels, double bpm) noexcept {
  if (const int waiting = fifo.getNumReady(); waiting > 0) {
    const auto scope = fifo.read(waiting);
    for (int i = 0; i < scope.blockSize1; ++i)
      apply(slots[static_cast<std::size_t>(scope.startIndex1 + i)], bufferChannels, bpm);
    for (int i = 0; i < scope.blockSize2; ++i)
      apply(slots[static_cast<std::size_t>(scope.startIndex2 + i)], bufferChannels, bpm);
  }
  for (int i = 0; i < audioPendingCount; ++i)
    apply(audioPending[static_cast<std::size_t>(i)], bufferChannels, bpm);
  audioPendingCount = 0;
}

void Looper::apply(Action action, int bufferChannels, double bpm) noexcept {
  const bool reverse = reverseOption.load(std::memory_order_relaxed);
  switch (action) {
    case Action::record:
      if (state == State::recording) {
        if (!closing)
          requestClose(State::playing, bpm);
      } else {
        startRecording(bufferChannels);
      }
      break;

    case Action::overdub:
      switch (state) {
        case State::empty: break;
        case State::recording:
          if (closing)
            closeNext = State::overdubbing;
          else
            requestClose(State::overdubbing, bpm);
          break;
        case State::playing: startOverdub(); break;
        case State::overdubbing:
          endOverdub();
          state = State::playing;
          break;
        case State::stopped:
          position = startPosition(reverse);
          startOverdub();
          break;
      }
      break;

    case Action::play:
      switch (state) {
        case State::empty:
        case State::playing: break;
        case State::recording:
          if (closing)
            closeNext = State::playing;
          else
            requestClose(State::playing, bpm);
          break;
        case State::overdubbing:
          endOverdub();
          state = State::playing;
          break;
        case State::stopped:
          position = startPosition(reverse);
          state = State::playing;
          break;
      }
      break;

    case Action::stop:
      switch (state) {
        case State::empty:
        case State::stopped: break;
        case State::recording:
          if (closing)
            closeNext = State::stopped;
          else
            requestClose(State::stopped, bpm);
          break;
        case State::overdubbing:
          endOverdub();
          [[fallthrough]];
        case State::playing:
          state = State::stopped;
          position = startPosition(reverse);
          break;
      }
      break;

    case Action::undo:
      if (state == State::overdubbing) {
        discardOverdub();
        state = State::playing;
      } else if ((state == State::playing || state == State::stopped) && active > 1) {
        --active;
      }
      break;

    case Action::redo:
      if ((state == State::playing || state == State::stopped) && active < used)
        ++active;
      break;

    case Action::clear: clearAll(); break;

    case Action::cycle:
      switch (state) {
        case State::empty: startRecording(bufferChannels); break;
        case State::recording:
          if (closing)
            closeNext = State::overdubbing;
          else
            requestClose(State::overdubbing, bpm);
          break;
        case State::playing: startOverdub(); break;
        case State::overdubbing:
          endOverdub();
          state = State::playing;
          break;
        case State::stopped:
          position = startPosition(reverse);
          state = State::playing;
          break;
      }
      break;

    case Action::playStop:
      switch (state) {
        case State::empty: break;
        case State::recording:
          if (closing)
            closeNext = State::playing;
          else
            requestClose(State::playing, bpm);
          break;
        case State::overdubbing:
          endOverdub();
          [[fallthrough]];
        case State::playing:
          state = State::stopped;
          position = startPosition(reverse);
          break;
        case State::stopped:
          position = startPosition(reverse);
          state = State::playing;
          break;
      }
      break;
  }
}

void Looper::startRecording(int bufferChannels) noexcept {
  clearAll();
  loopChannels = bufferChannels >= 2 ? 2 : 1;
  state = State::recording;
}

// The first recording ends. With quantise on, its length becomes a whole
// number of beats: shorter, it is cut there at once; longer, it runs on until
// that beat (`closing`) and then goes to `next`.
void Looper::requestClose(State next, double bpm) noexcept {
  if (recorded <= 0) {
    clearAll();
    return;
  }
  if (quantiseOption.load(std::memory_order_relaxed) && bpm > 0.0) {
    const double beat = sampleRate * 60.0 / bpm;
    const double beats = std::max(1.0, std::round(static_cast<double>(recorded) / beat));
    const auto target = std::min(static_cast<std::int64_t>(std::llround(beats * beat)), capacity);
    if (target > recorded) {
      closing = true;
      closeAt = target;
      closeNext = next;
      return;
    }
    recorded = std::max<std::int64_t>(1, target);
  }
  finishRecording(next);
}

void Looper::finishRecording(State next) noexcept {
  closing = false;
  length = recorded;
  layerStart[0] = 0;
  layerCount[0] = length;
  active = 1;
  used = 1;
  fadeFrames = std::min<std::int64_t>(std::llround(kFadeSeconds * sampleRate), length / 4);
  const bool reverse = reverseOption.load(std::memory_order_relaxed);
  position = next == State::overdubbing ? 0.0 : startPosition(reverse);
  state = next;
  if (next == State::overdubbing) {
    state = State::playing;
    startOverdub();
  }
}

void Looper::startOverdub() noexcept {
  if (active >= kMaxLayers)
    return;
  position = std::floor(position);
  used = active;  // a new layer ends the redo of the ones undone
  layerStart[static_cast<std::size_t>(active)] = static_cast<std::int64_t>(position);
  layerCount[static_cast<std::size_t>(active)] = 0;
  state = State::overdubbing;
}

void Looper::endOverdub() noexcept {
  const auto slot = static_cast<std::size_t>(active);
  layerCount[slot] = std::min(layerCount[slot], length);
  if (layerCount[slot] > 0) {
    ++active;
    used = active;
  } else {
    layerCount[slot] = 0;
  }
}

void Looper::discardOverdub() noexcept {
  layerCount[static_cast<std::size_t>(active)] = 0;
}

void Looper::clearAll() noexcept {
  state = State::empty;
  length = 0;
  position = 0.0;
  recorded = 0;
  closing = false;
  closeAt = 0;
  active = 0;
  used = 0;
  fadeFrames = 0;
  layerStart.fill(0);
  layerCount.fill(0);
  publish();
}

double Looper::startPosition(bool reverse) const noexcept {
  return reverse && length > 0 ? static_cast<double>(length - 1) : 0.0;
}

// ####################
// AUDIO
// ####################
bool Looper::covered(int l, std::int64_t index) const noexcept {
  const auto slot = static_cast<std::size_t>(l);
  std::int64_t delta = index - layerStart[slot];
  if (delta < 0)
    delta += length;
  return delta < layerCount[slot];
}

float Looper::fadeGain(std::int64_t index) const noexcept {
  if (fadeFrames <= 0)
    return 1.0f;
  std::int64_t k = index;
  if (k >= fadeFrames) {
    k = length - 1 - index;
    if (k >= fadeFrames)
      return 1.0f;
  }
  constexpr float kPi = 3.14159265358979f;
  return 0.5f - 0.5f * std::cos(kPi * (static_cast<float>(k) + 0.5f) / static_cast<float>(fadeFrames));
}

void Looper::process(float* const* data, int numChannels, int numFrames, double bpm) noexcept {
  if (!ready.load(std::memory_order_acquire) || numFrames <= 0 || numChannels <= 0)
    return;
  const int bufferChannels = std::min(numChannels, kMaxChannels);
  applyQueued(bufferChannels, bpm);

  const Options options{halfSpeedOption.load(std::memory_order_relaxed),
                        reverseOption.load(std::memory_order_relaxed),
                        oneShotOption.load(std::memory_order_relaxed),
                        quantiseOption.load(std::memory_order_relaxed)};
  const float levelDb = levelOption.load(std::memory_order_relaxed);
  levelGain.setTargetValue(levelDb <= kMinLevelDb ? 0.0f : juce::Decibels::decibelsToGain(levelDb));

  // The loop's channels against the buffer's: a stereo loop on a mono buffer
  // is heard as its two sides averaged, a mono loop on a stereo buffer on
  // both.
  const auto readInput = [&](int frame, float (&in)[kMaxChannels]) {
    const float left = data[0][frame];
    const float right = bufferChannels > 1 ? data[1][frame] : left;
    if (loopChannels >= 2) {
      in[0] = left;
      in[1] = right;
    } else {
      in[0] = bufferChannels > 1 ? 0.5f * (left + right) : left;
      in[1] = 0.0f;
    }
  };
  const auto addOutput = [&](int frame, const float (&p)[kMaxChannels]) {
    if (loopChannels == bufferChannels) {
      for (int c = 0; c < bufferChannels; ++c)
        data[c][frame] += p[c];
    } else if (loopChannels >= 2) {
      data[0][frame] += 0.5f * (p[0] + p[1]);
    } else {
      for (int c = 0; c < bufferChannels; ++c)
        data[c][frame] += p[0];
    }
  };

  for (int i = 0; i < numFrames; ++i) {
    if (state == State::empty || state == State::stopped) {
      levelGain.skip(numFrames - i);
      break;
    }

    if (state == State::recording) {
      float in[kMaxChannels];
      readInput(i, in);
      for (int c = 0; c < loopChannels; ++c)
        layer[0][static_cast<std::size_t>(c)].data[recorded] = in[c];
      ++recorded;
      if (closing) {
        if (recorded >= closeAt)
          finishRecording(closeNext);
      } else if (recorded >= capacity) {
        finishRecording(State::playing);
      }
      continue;
    }

    // Playing or overdubbing. While overdubbing the loop runs at normal
    // speed, forwards.
    const bool overdubbing = state == State::overdubbing;
    const bool backwards = !overdubbing && options.reverse;
    const double step = !overdubbing && options.halfSpeed ? 0.5 : 1.0;

    float in[kMaxChannels] = {0.0f, 0.0f};
    if (overdubbing)
      readInput(i, in);

    const auto index = static_cast<std::int64_t>(position);
    const auto fraction = static_cast<float>(position - static_cast<double>(index));
    const std::int64_t next = index + 1 >= length ? 0 : index + 1;
    float playback[kMaxChannels] = {0.0f, 0.0f};
    const int inSum = layersInSum();
    for (int l = 0; l < inSum; ++l) {
      if (!covered(l, index))
        continue;
      const bool nextCovered = fraction > 0.0f && covered(l, next);
      for (int c = 0; c < loopChannels; ++c) {
        const float* source = layer[static_cast<std::size_t>(l)][static_cast<std::size_t>(c)].data;
        float v = source[index];
        if (fraction > 0.0f)
          v += fraction * ((nextCovered ? source[next] : 0.0f) - v);
        playback[c] += v;
      }
    }
    const float gain = fadeGain(index) * levelGain.getNextValue();
    playback[0] *= gain;
    playback[1] *= gain;
    addOutput(i, playback);

    if (overdubbing) {
      const auto slot = static_cast<std::size_t>(active);
      std::int64_t delta = index - layerStart[slot];
      if (delta < 0)
        delta += length;
      for (int c = 0; c < loopChannels; ++c) {
        float* target = layer[slot][static_cast<std::size_t>(c)].data;
        if (delta < layerCount[slot])
          target[index] += in[c];
        else
          target[index] = in[c];
      }
      if (delta >= layerCount[slot] && layerCount[slot] < length)
        ++layerCount[slot];
    }

    bool wrapped = false;
    if (!backwards) {
      position += step;
      if (position >= static_cast<double>(length)) {
        position -= static_cast<double>(length);
        wrapped = true;
      }
    } else {
      position -= step;
      if (position < 0.0) {
        position += static_cast<double>(length);
        wrapped = true;
      }
    }
    if (wrapped && options.oneShot) {
      if (overdubbing)
        endOverdub();
      state = State::stopped;
      position = startPosition(options.reverse);
    }
  }
  publish();
}

// ####################
// STATE
// ####################
void Looper::publish() noexcept {
  pubState.store(static_cast<int>(state), std::memory_order_relaxed);
  pubClosing.store(closing, std::memory_order_relaxed);
  pubLength.store(length, std::memory_order_relaxed);
  pubPosition.store(state == State::recording ? recorded : static_cast<std::int64_t>(position),
                    std::memory_order_relaxed);
  pubRecorded.store(recorded, std::memory_order_relaxed);
  pubLayers.store(layersInSum(), std::memory_order_relaxed);
  pubRedo.store(std::max(0, used - active), std::memory_order_relaxed);
  pubChannels.store(state == State::empty ? 0 : loopChannels, std::memory_order_relaxed);
}

const char* Looper::stateName(State s) noexcept {
  switch (s) {
    case State::empty: return "empty";
    case State::recording: return "recording";
    case State::playing: return "playing";
    case State::overdubbing: return "overdubbing";
    case State::stopped: return "stopped";
  }
  return "empty";
}

Looper::Snapshot Looper::snapshot() const noexcept {
  Snapshot s;
  s.state = static_cast<State>(pubState.load(std::memory_order_relaxed));
  s.closing = pubClosing.load(std::memory_order_relaxed);
  s.ready = ready.load(std::memory_order_acquire);
  s.lengthFrames = pubLength.load(std::memory_order_relaxed);
  s.positionFrames = pubPosition.load(std::memory_order_relaxed);
  s.recordedFrames = pubRecorded.load(std::memory_order_relaxed);
  s.capacityFrames = capacity;
  s.layers = pubLayers.load(std::memory_order_relaxed);
  s.redoLayers = pubRedo.load(std::memory_order_relaxed);
  s.channels = pubChannels.load(std::memory_order_relaxed);
  s.queued = fifo.getNumReady();
  s.sampleRate = sampleRate;
  s.halfSpeed = halfSpeedOption.load(std::memory_order_relaxed);
  s.reverse = reverseOption.load(std::memory_order_relaxed);
  s.oneShot = oneShotOption.load(std::memory_order_relaxed);
  s.quantise = quantiseOption.load(std::memory_order_relaxed);
  s.levelDb = levelOption.load(std::memory_order_relaxed);
  return s;
}

juce::var Looper::describe() const {
  const Snapshot s = snapshot();
  const double rate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
  const auto seconds = [rate](std::int64_t frames) { return static_cast<double>(frames) / rate; };
  const bool recording = s.state == State::recording;

  auto* options = new juce::DynamicObject();
  options->setProperty("halfSpeed", s.halfSpeed);
  options->setProperty("reverse", s.reverse);
  options->setProperty("oneShot", s.oneShot);
  options->setProperty("quantise", s.quantise);
  options->setProperty("levelDb", s.levelDb);

  auto* object = new juce::DynamicObject();
  object->setProperty("state", stateName(s.state));
  object->setProperty("ready", s.ready);
  object->setProperty("closing", s.closing);
  object->setProperty("queued", s.queued);
  object->setProperty("channels", s.channels);
  object->setProperty("lengthFrames", static_cast<juce::int64>(s.lengthFrames));
  object->setProperty("lengthSeconds", seconds(s.lengthFrames));
  object->setProperty("playbackLengthSeconds",
                      seconds(s.lengthFrames) * (s.halfSpeed ? 2.0 : 1.0));
  object->setProperty("recordedSeconds", recording ? seconds(s.recordedFrames) : 0.0);
  object->setProperty("positionFrames", static_cast<juce::int64>(s.positionFrames));
  object->setProperty("positionSeconds", seconds(s.positionFrames));
  object->setProperty("position",
                      s.lengthFrames > 0 && !recording
                          ? static_cast<double>(s.positionFrames) / static_cast<double>(s.lengthFrames)
                          : 0.0);
  object->setProperty("layers", s.layers);
  object->setProperty("canUndo", s.layers > 1 || s.state == State::overdubbing);
  object->setProperty("redoLayers", s.redoLayers);
  object->setProperty("canRedo", s.redoLayers > 0);
  object->setProperty("layersFull", s.layers >= kMaxLayers);
  object->setProperty("maxLayers", kMaxLayers);
  object->setProperty("maxSeconds", kMaxSeconds);
  object->setProperty("capacitySeconds", seconds(s.capacityFrames));
  object->setProperty("options", juce::var(options));
  return juce::var(object);
}
