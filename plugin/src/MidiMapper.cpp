#include "MidiMapper.h"

MidiMapper::MidiMapper(juce::AudioProcessorValueTreeState& params) : parameters(params) {
  lastLooperCcValue.fill(-1);
}

MidiMapper::~MidiMapper() {
  cancelPendingUpdate();
}

MidiMapper::BlockPowerTarget MidiMapper::blockPowerTargetFor(const juce::String& targetId) {
  const bool right = targetId.startsWith("rightBlock");
  const int prefixLength = right ? 10 : 5;  // "rightBlock" / "block"
  if ((!right && !targetId.startsWith("block")) || !targetId.endsWith("Power"))
    return {};
  const auto middle = targetId.substring(prefixLength, targetId.length() - 5);
  if (middle.isEmpty() || !middle.containsOnly("0123456789"))
    return {};
  const int position = middle.getIntValue();  // 1-based in the id
  if (position < 1 || position > 64)
    return {};
  return {position - 1, right};
}

//==============================================================================
// Audio thread

void MidiMapper::processMidi(const juce::MidiBuffer& midi) {
  if (midi.isEmpty())
    return;

  const int channel = channelFilter.load(std::memory_order_relaxed);

  if (learnArmed.load(std::memory_order_relaxed)) {
    // Learn mode: capture the first eligible event and drive nothing. The
    // control being wiggled may already be mapped elsewhere, and jerking that
    // target mid-learn would be hostile.
    for (const auto metadata : midi) {
      const auto msg = metadata.getMessage();
      if (channel != 0 && msg.getChannel() != channel)
        continue;
      if (!msg.isController() && !msg.isNoteOn())
        continue;
      if (!learnArmed.exchange(false))
        break;  // another callback captured first
      capturedSource.store(static_cast<int>(msg.isController() ? Source::cc : Source::note));
      capturedNumber.store(msg.isController() ? msg.getControllerNumber() : msg.getNoteNumber());
      triggerAsyncUpdate();
      break;
    }
    return;
  }

  const juce::SpinLock::ScopedTryLockType tryLock(mapLock);
  if (!tryLock.isLocked())
    return;  // rare contended edit: drop one buffer of MIDI, never block audio

  // One clock read per buffer that carries MIDI, for the tap tempo control.
  const double bufferTimeMs = juce::Time::getMillisecondCounterHiRes();

  for (const auto metadata : midi) {
    const auto msg = metadata.getMessage();
    if (channel != 0 && msg.getChannel() != channel)
      continue;

    // Program changes switch presets: always live, no mapping needed.
    if (msg.isProgramChange()) {
      pendingProgram.store(msg.getProgramChangeNumber());
      triggerAsyncUpdate();
      continue;
    }

    if (!msg.isController() && !msg.isNoteOn())
      continue;
    const auto source = msg.isController() ? Source::cc : Source::note;
    const int number = msg.isController() ? msg.getControllerNumber() : msg.getNoteNumber();
    bool mapped = false;
    for (auto& mapping : mappings)
      if (mapping.source == source && mapping.number == number) {
        applyEvent(mapping, msg);
        mapped = true;
      }
    // The fixed defaults only speak for a controller the user has not given
    // a job of their own.
    if (!mapped && source == Source::cc)
      applyFixedControl(number, msg.getControllerValue(), bufferTimeMs);
  }
}

void MidiMapper::applyFixedControl(int ccNumber, int value, double timeMs) {
  const auto ccOf = [this](FixedControl control) {
    return fixedControlCc[static_cast<size_t>(control)].load(std::memory_order_relaxed);
  };

  // Everything these do takes the chain lock or more (a scene switch, a
  // settings write), so each is recorded for the message thread.
  if (ccNumber == ccOf(FixedControl::sceneSelect)) {
    if (value < 8) {
      pendingScene.store(value);
      triggerAsyncUpdate();
    }
  } else if (ccNumber == ccOf(FixedControl::tapTempo)) {
    // A press, by the same rule as a mapped toggle: a footswitch sends 127
    // going down and 0 coming up, and only the first is a tap.
    const bool press = value >= 64 || lastTapCcValue < 64;
    lastTapCcValue = value;
    if (press) {
      const auto scope = tapQueue.write(1);
      if (scope.blockSize1 > 0) {
        tapTimes[static_cast<size_t>(scope.startIndex1)] = timeMs;
        triggerAsyncUpdate();
      }
    }
  } else if (ccNumber == ccOf(FixedControl::tuner)) {
    pendingTuner.store(value >= 64 ? 1 : 0);
    triggerAsyncUpdate();
  } else if (ccNumber == ccOf(FixedControl::footswitchMode)) {
    if (value < 4) {
      pendingFootswitchMode.store(value);
      triggerAsyncUpdate();
    }
  } else if (onLooperControl) {
    // The looper's controls take the looper's lock-free door straight from
    // here: no hop to the message thread, so the action lands in this block.
    for (int i = static_cast<int>(FixedControl::looperRecord); i < kNumFixedControls; ++i) {
      const auto control = static_cast<FixedControl>(i);
      if (ccNumber != ccOf(control))
        continue;
      switch (control) {
        case FixedControl::looperHalfSpeed:
        case FixedControl::looperReverse:
        case FixedControl::looperOneShot:
        case FixedControl::looperQuantise: onLooperControl(control, value >= 64 ? 1 : 0); break;
        case FixedControl::looperLevel: onLooperControl(control, value); break;
        default: {
          // A press, by the same rule as the tap and the mapped toggles.
          int& last = lastLooperCcValue[static_cast<size_t>(i)];
          const bool press = value >= 64 || last < 64;
          last = value;
          if (press)
            onLooperControl(control, 1);
          break;
        }
      }
    }
  }
}

void MidiMapper::setFixedControlCc(FixedControl control, int ccNumber) {
  if (control == FixedControl::count)
    return;
  fixedControlCc[static_cast<size_t>(control)].store(ccNumber >= 0 && ccNumber <= 127 ? ccNumber
                                                                                     : -1);
  notifyChanged();
}

int MidiMapper::getFixedControlCc(FixedControl control) const {
  return control == FixedControl::count
             ? -1
             : fixedControlCc[static_cast<size_t>(control)].load();
}

void MidiMapper::applyEvent(Mapping& mapping, const juce::MidiMessage& msg) {
  if (mapping.toggle) {
    // Note-ons always fire. For CCs, detect a *press*: any value ≥ 64, or a
    // value < 64 that doesn't follow a ≥ 64 one from this control. A
    // momentary switch (127 then 0) still fires once per stomp (the 0 is
    // its release), while footswitches programmed to send only low values (a
    // common programmable-pedal setup) fire on every message instead of
    // being silently dropped by a plain ≥ 64 gate.
    if (msg.isController()) {
      const int value = msg.getControllerValue();
      const bool press = value >= 64 || mapping.lastCcValue < 64;
      mapping.lastCcValue = value;
      if (!press)
        return;
    }
    switch (mapping.kind) {
      case Kind::blockPower:
        // Block enable is a locked, undoable chain edit, so hop to the message
        // thread. XOR keeps rapid double-stomps parity-correct if they land
        // before the async update runs.
        (mapping.rightBlock ? pendingRightBlockToggles : pendingBlockToggles)
            .fetch_xor(juce::uint64(1) << mapping.blockIndex);
        triggerAsyncUpdate();
        return;
      case Kind::stereoMode:
        // Same deferral as block powers; a flip counter's parity is the XOR
        // equivalent for a single toggle.
        pendingStereoToggles.fetch_add(1);
        triggerAsyncUpdate();
        return;
      case Kind::presetStep:
        // Preset loads are heavyweight message-thread work; the signed sum
        // nets rapid prev/next stomps to the right landing spot.
        pendingPresetSteps.fetch_add(mapping.presetDelta);
        triggerAsyncUpdate();
        return;
      case Kind::parameter: {
        auto& param = *mapping.param;
        param.setValueNotifyingHost(param.getValue() < 0.5f ? 1.0f : 0.0f);
        return;
      }
    }
    return;
  }

  mapping.param->setValueNotifyingHost(static_cast<float>(msg.getControllerValue()) / 127.0f);
}

//==============================================================================
// Message thread

juce::var MidiMapper::getState() const {
  auto* obj = new juce::DynamicObject();
  juce::var state(obj);

  obj->setProperty("channel", channelFilter.load());
  obj->setProperty("learnTargetId", learnArmed.load() ? learnTargetId : juce::String());

  juce::Array<juce::var> list;
  {
    const juce::SpinLock::ScopedLockType lock(mapLock);
    for (const auto& mapping : mappings) {
      auto* m = new juce::DynamicObject();
      m->setProperty("targetId", mapping.targetId);
      m->setProperty("source", sourceToString(mapping.source));
      m->setProperty("number", mapping.number);
      list.add(juce::var(m));
    }
  }
  obj->setProperty("mappings", list);

  // The fixed controls and whether a mapping on the same controller number
  // currently silences each.
  static constexpr const char* kFixedControlNames[kNumFixedControls] = {
      "sceneSelect",    "tapTempo",       "tuner",          "footswitchMode", "looperRecord",
      "looperOverdub",  "looperPlay",     "looperStop",     "looperUndo",     "looperRedo",
      "looperClear",    "looperHalfSpeed", "looperReverse", "looperOneShot",  "looperQuantise",
      "looperLevel",    "looperCycle",    "looperPlayStop"};
  juce::Array<juce::var> defaults;
  for (int i = 0; i < kNumFixedControls; ++i) {
    const int cc = fixedControlCc[static_cast<size_t>(i)].load();
    bool overridden = false;
    for (const auto& mapping : list)
      overridden = overridden || (mapping["source"].toString() == "cc" &&
                                  static_cast<int>(mapping["number"]) == cc);
    auto* entry = new juce::DynamicObject();
    entry->setProperty("action", kFixedControlNames[i]);
    entry->setProperty("cc", cc);
    entry->setProperty("overridden", overridden);
    defaults.add(juce::var(entry));
  }
  obj->setProperty("defaults", defaults);
  return state;
}

void MidiMapper::setChannelFilter(int channel) {
  channelFilter.store(juce::jlimit(0, 16, channel));
  notifyChanged();
}

void MidiMapper::startLearn(const juce::String& targetId) {
  if (!isValidTarget(targetId))
    return;
  learnArmed.store(false);  // quiesce the audio thread while re-targeting
  capturedNumber.store(-1);
  learnTargetId = targetId;
  learnArmed.store(true);
  notifyChanged();
}

void MidiMapper::cancelLearn() {
  learnArmed.store(false);
  capturedNumber.store(-1);
  learnTargetId.clear();
  notifyChanged();
}

bool MidiMapper::removeMapping(const juce::String& targetId) {
  bool removed = false;
  {
    const juce::SpinLock::ScopedLockType lock(mapLock);
    const auto before = mappings.size();
    std::erase_if(mappings, [&](const Mapping& m) { return m.targetId == targetId; });
    removed = mappings.size() != before;
  }
  if (removed)
    notifyChanged();
  return removed;
}

bool MidiMapper::setCcMapping(const juce::String& targetId, int ccNumber) {
  if (!isValidTarget(targetId) || ccNumber < 0 || ccNumber > 127)
    return false;
  auto mapping = makeMapping(targetId, Source::cc, ccNumber);
  {
    const juce::SpinLock::ScopedLockType lock(mapLock);
    std::erase_if(mappings, [&](const Mapping& m) { return m.targetId == targetId; });
    mappings.push_back(std::move(mapping));
  }
  // Typing supersedes a listen on the same target, including a capture
  // already waiting for its async commit; a learn armed for another target
  // keeps listening.
  if (learnTargetId == targetId) {
    learnArmed.store(false);
    capturedNumber.store(-1);
    learnTargetId.clear();
  }
  notifyChanged();
  return true;
}

MidiMapper::Mapping MidiMapper::makeMapping(const juce::String& targetId, Source source,
                                            int number) const {
  const auto block = blockPowerTargetFor(targetId);
  const int presetDelta = targetId == kPresetNextTarget   ? 1
                          : targetId == kPresetPrevTarget ? -1
                                                          : 0;
  const Kind kind = presetDelta != 0          ? Kind::presetStep
                    : targetId == kStereoTarget ? Kind::stereoMode
                    : block.index >= 0          ? Kind::blockPower
                                                : Kind::parameter;
  auto* param = kind == Kind::parameter ? parameters.getParameter(targetId) : nullptr;
  jassert(kind != Kind::parameter || param != nullptr);  // callers validate the id first
  const bool toggle = kind != Kind::parameter || source == Source::note ||
                      (param != nullptr && param->isBoolean());
  return {targetId, kind, param, block.index, block.right, presetDelta, source, number, toggle};
}

void MidiMapper::notifyChanged() {
  mapDirty.store(true);
  triggerAsyncUpdate();
}

void MidiMapper::handleAsyncUpdate() {
  // 1. Commit a pending learn capture.
  const int number = capturedNumber.exchange(-1);
  if (number >= 0 && learnTargetId.isNotEmpty()) {
    const auto source = static_cast<Source>(capturedSource.load());
    if (isValidTarget(learnTargetId)) {
      auto mapping = makeMapping(learnTargetId, source, number);
      const juce::SpinLock::ScopedLockType lock(mapLock);
      std::erase_if(mappings, [&](const Mapping& m) { return m.targetId == learnTargetId; });
      mappings.push_back(std::move(mapping));
    }
    learnTargetId.clear();
    mapDirty.store(true);
  }

  // 2. Deliver deferred performance events (presets, block powers).
  const int program = pendingProgram.exchange(-1);
  if (program >= 0 && onProgramChange)
    onProgramChange(program);

  auto toggles = pendingBlockToggles.exchange(0);
  for (int index = 0; toggles != 0; ++index, toggles >>= 1)
    if ((toggles & 1) != 0 && onBlockPowerToggle)
      onBlockPowerToggle(index, false);

  auto rightToggles = pendingRightBlockToggles.exchange(0);
  for (int index = 0; rightToggles != 0; ++index, rightToggles >>= 1)
    if ((rightToggles & 1) != 0 && onBlockPowerToggle)
      onBlockPowerToggle(index, true);

  if (pendingStereoToggles.exchange(0) % 2 != 0 && onStereoToggle)
    onStereoToggle();

  if (const int steps = pendingPresetSteps.exchange(0); steps != 0 && onPresetStep)
    onPresetStep(steps);

  // The fixed controls. Mode before scene: a controller that sends both in
  // one go means "scene mode, then this scene".
  if (const int mode = pendingFootswitchMode.exchange(-1); mode >= 0 && onFootswitchMode)
    onFootswitchMode(mode);
  if (const int scene = pendingScene.exchange(-1); scene >= 0 && onSceneSelect)
    onSceneSelect(scene);
  if (const int tuner = pendingTuner.exchange(-1); tuner >= 0 && onTunerEnabled)
    onTunerEnabled(tuner != 0);
  for (;;) {
    double tapTime = 0.0;
    {
      const auto scope = tapQueue.read(1);
      if (scope.blockSize1 == 0)
        break;
      tapTime = tapTimes[static_cast<size_t>(scope.startIndex1)];
    }
    if (onTapTempo)
      onTapTempo(tapTime);
  }

  // 3. Tell the UI, but only when the map/learn state actually moved;
  //    performance events alone shouldn't cause settings re-pulls.
  if (mapDirty.exchange(false) && onChanged)
    onChanged();
}

//==============================================================================
// Plugin state

juce::ValueTree MidiMapper::toValueTree() const {
  juce::ValueTree tree("MidiMappings");
  tree.setProperty("channel", channelFilter.load(), nullptr);

  const juce::SpinLock::ScopedLockType lock(mapLock);
  for (const auto& mapping : mappings) {
    juce::ValueTree child("Mapping");
    child.setProperty("targetId", mapping.targetId, nullptr);
    child.setProperty("source", sourceToString(mapping.source), nullptr);
    child.setProperty("number", mapping.number, nullptr);
    tree.appendChild(child, nullptr);
  }
  return tree;
}

void MidiMapper::restoreFromValueTree(const juce::ValueTree& tree) {
  std::vector<Mapping> restored;
  int channel = 0;

  if (tree.hasType("MidiMappings")) {
    channel = juce::jlimit(0, 16, static_cast<int>(tree.getProperty("channel", 0)));
    for (const auto& child : tree) {
      if (!child.hasType("Mapping"))
        continue;
      const auto targetId = child.getProperty("targetId").toString();
      if (!isValidTarget(targetId))
        continue;  // unknown/renamed target in an old project: drop it
      restored.push_back(makeMapping(targetId,
                                     sourceFromString(child.getProperty("source").toString()),
                                     static_cast<int>(child.getProperty("number", 0))));
    }
  }

  channelFilter.store(channel);
  {
    const juce::SpinLock::ScopedLockType lock(mapLock);
    mappings = std::move(restored);
  }
  notifyChanged();
}
