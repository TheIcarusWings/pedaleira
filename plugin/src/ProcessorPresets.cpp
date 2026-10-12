#include "Processor.h"

#include "LegacyParamIds.h"

#include <cmath>
#include <limits>

// #############################
// INTERNAL PRESETS
// #############################
//
// A preset is a self-contained tone: the full chain snapshot (same tree the
// undo system uses, plus embedded model bytes so it loads offline) and the
// faceplate parameter values. Loading goes through the same reconciling
// restore as undo/redo (engines are reused where possible, everything else
// loads in the background seeded from the embedded bytes) and records one
// undo step, so a preset load is itself undoable.
//
// Where presets live is the setlist library's business (SetlistLibrary.h,
// ProcessorLibrary.cpp): the preset list everything here works on is the
// current setlist, slot by slot. The tree is the same whether it comes out
// of a JSON file of the library or a factory .t3kpreset.

const std::vector<juce::String>& TONE3000Processor::presetParameterIds() {
  // chainSolo* stays out on purpose: solo is monitoring state, not tone,
  // and a preset saved mid-audition must not load with a chain muted.
  static const std::vector<juce::String> ids = {
      "inputLevel",     "outputLevel",      "outputBalance",
      "toneBass",       "toneMid",          "toneTreble",
      "gateThreshold",  "gateEnabled",      "gateRelease",
      "gateHold",       "gateRange",        "toneEqEnabled",
      "pitchEnabled",   "pitchSemitones",   "pitchStep",
      "pitchTonality",  "pitchWindow",
      "spreadEnabled",  "spreadOffset",     "spreadWobble",
      "spreadWobbleEnabled", "spreadCrossover", "spreadCrossoverEnabled",
      "spreadDiffuseEnabled",
      "alignEnabled",   "alignOffset",      "alignWobble",
      "alignWobbleEnabled", "alignCrossover", "alignCrossoverEnabled",
      "alignDiffuseEnabled",
      "chainPanLeft",   "chainPanRight",    "chainPanLinked",
      "chainInvertLeft", "chainInvertRight",
  };
  return ids;
}

void TONE3000Processor::setActivePreset(const juce::String& id, const juce::String& name) {
  juce::ScopedLock lock(chainMutex);
  activePresetId = id;
  activePresetName = name;
  bumpChainRevision();
}

juce::String TONE3000Processor::currentSetlistId() const {
  const juce::String selected = userLibrary().current().setlistId;
  if (selected == kFactorySetlistId)
    return selected;
  const auto setlists = userLibrary().setlists();
  for (const auto& setlist : setlists)
    if (setlist.id == selected)
      return selected;
  // Nothing selected yet, or the selected setlist is gone (deleted in a
  // file browser, another branch checked out).
  return setlists.empty() ? juce::String(kFactorySetlistId) : setlists.front().id;
}

std::vector<PresetManager::Info> TONE3000Processor::factoryPresets() const {
  std::vector<PresetManager::Info> out;
  for (auto& info : presetManager.list())
    if (info.factory)
      out.push_back(std::move(info));
  return out;
}

std::vector<PresetManager::Info> TONE3000Processor::currentPresetSlots() const {
  const juce::String setlistId = currentSetlistId();
  if (setlistId == kFactorySetlistId)
    return factoryPresets();  // no gaps: slot = place in the name-ordered list
  std::vector<PresetManager::Info> out;
  for (const auto& slot : userLibrary().list(setlistId).slots) {
    out.resize(static_cast<size_t>(slot.slot));
    out.back().id = slot.id;
    out.back().name = slot.name;
  }
  return out;
}

void TONE3000Processor::presetListChanged() {
  // Host program names and the active program's index follow the list.
  hostProgramInfoCache.clear();
  updateHostDisplay(ChangeDetails{}.withProgramChanged(true));
}

juce::var TONE3000Processor::getPresetList() const {
  const juce::String setlistId = currentSetlistId();
  const bool factory = setlistId == kFactorySetlistId;
  juce::String setlistName = "Factory";
  if (!factory)
    if (const auto setlist = userLibrary().findSetlist(setlistId))
      setlistName = setlist->name;

  juce::Array<juce::var> presetArray;
  const auto slots = currentPresetSlots();
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i].id.isEmpty())
      continue;  // an empty slot
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("id", slots[i].id);
    obj->setProperty("name", slots[i].name);
    obj->setProperty("factory", slots[i].factory);
    obj->setProperty("slot", static_cast<int>(i) + 1);
    presetArray.add(juce::var(obj.get()));
  }
  juce::DynamicObject::Ptr setlist = new juce::DynamicObject();
  setlist->setProperty("id", setlistId);
  setlist->setProperty("name", setlistName);
  setlist->setProperty("readOnly", factory);
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("presets", presetArray);
  root->setProperty("setlist", juce::var(setlist.get()));
  return root.get();
}

juce::ValueTree TONE3000Processor::buildPresetTree() {
  juce::ValueTree preset(PresetManager::kPresetTag);
  preset.setProperty("schemaVersion", 1, nullptr);

  // A save: hosted plugins' cached chunks are refreshed first (see
  // getStateInformation).
  serviceHostedPlugins(/*forceStateRefresh=*/true);

  {
    juce::ScopedLock lock(chainMutex);
    juce::ValueTree snapshot = captureChainSnapshot(true);
    // Solo is monitoring state: it stays with the session and is not
    // written into presets (as the legacy chain solos never were).
    juce::ValueTree gridNode = snapshot.getChildWithName("Grid");
    for (int i = 0; i < gridNode.getNumChildren(); ++i)
      if (juce::ValueTree output = gridNode.getChild(i).getChildWithName("Output");
          output.isValid() && output.hasProperty("solo"))
        output.setProperty("solo", false, nullptr);
    // A grid preset says so up front; a legacy one is the file it always was.
    if (gridNode.isValid())
      preset.setProperty("schemaVersion", 2, nullptr);
    preset.appendChild(snapshot, nullptr);
  }

  // Values are stored denormalized (real dB/ratio units), so preset files
  // stay meaningful if a parameter's range is ever retuned.
  juce::ValueTree params("Params");
  for (const auto& paramId : presetParameterIds()) {
    if (auto* p = parameters.getParameter(paramId)) {
      juce::ValueTree paramTree("Param");
      paramTree.setProperty("id", paramId, nullptr);
      paramTree.setProperty("value", static_cast<double>(p->convertFrom0to1(p->getValue())),
                            nullptr);
      params.appendChild(paramTree, nullptr);
    }
  }
  preset.appendChild(params, nullptr);
  return preset;
}

juce::var TONE3000Processor::savePreset(const juce::String& rawName, juce::String* whyNot) {
  const juce::String name = rawName.trim();
  if (name.isEmpty())
    return {};

  // The factory setlist cannot take it: the user's first setlist does, made
  // now if this is the first preset they ever save.
  juce::String setlistId = currentSetlistId();
  if (setlistId == kFactorySetlistId) {
    const auto setlists = userLibrary().setlists();
    if (!setlists.empty()) {
      setlistId = setlists.front().id;
    } else {
      SetlistLibrary::Setlist made;
      if (const juce::Result created = userLibrary().createSetlist("My Presets", &made);
          created.failed()) {
        juce::Logger::writeToLog("[Presets] " + created.getErrorMessage());
        if (whyNot != nullptr)
          *whyNot = created.getErrorMessage();
        return {};
      }
      setlistId = made.id;
    }
  }

  juce::var saved;
  if (const juce::Result result = savePresetToSlot(setlistId, 0, name, &saved); result.failed()) {
    juce::Logger::writeToLog("[Presets] Failed to save preset '" + name +
                             "': " + result.getErrorMessage());
    if (whyNot != nullptr)
      *whyNot = result.getErrorMessage();
    return {};
  }
  return saved;
}

bool TONE3000Processor::loadPresetAtIndex(int index) {
  const auto presets = currentPresetSlots();
  if (index < 0 || index >= static_cast<int>(presets.size()) ||
      presets[static_cast<size_t>(index)].id.isEmpty())
    return false;  // an empty slot, or a program beyond the setlist: ignore
  return loadPreset(presets[static_cast<size_t>(index)].id);
}

bool TONE3000Processor::stepPreset(int delta) {
  // The presets only: a step never lands on an empty slot.
  std::vector<PresetManager::Info> presets;
  for (auto& info : currentPresetSlots())
    if (info.id.isNotEmpty())
      presets.push_back(std::move(info));
  const int count = static_cast<int>(presets.size());
  if (count == 0 || delta == 0)
    return false;

  juce::String currentId;
  {
    juce::ScopedLock lock(chainMutex);
    currentId = activePresetId;
  }
  int index = -1;
  for (int i = 0; i < count; ++i) {
    if (presets[static_cast<size_t>(i)].id == currentId) {
      index = i;
      break;
    }
  }

  // Mirrors the preset bar's ‹ › buttons: wrap at the ends; with nothing
  // active (or a deleted preset), next starts at the first and previous at
  // the last.
  const int next = index < 0 ? (delta > 0 ? 0 : count - 1)
                             : ((index + delta) % count + count) % count;
  return loadPreset(presets[static_cast<size_t>(next)].id);
}

juce::ValueTree TONE3000Processor::readPresetTree(
    const juce::String& presetId, std::optional<SetlistLibrary::Located>* located) const {
  if (presetId.startsWith("factory:"))
    return presetManager.load(presetId);
  const auto found = userLibrary().find(presetId, currentSetlistId());
  if (!found.has_value())
    return {};
  const auto read = userLibrary().load(*found);
  if (!read.preset.isValid()) {
    juce::Logger::writeToLog("[Presets] Cannot read " + found->slot.file.getFullPathName() +
                             ": " + read.error);
    return {};
  }
  // Not a failure: the blocks concerned come up failed or as placeholders
  // under their names and the rest of the rig plays.
  for (const auto& asset : read.missing)
    juce::Logger::writeToLog("[Presets] '" + found->slot.name + "' is missing the asset '" +
                             asset.name + "' (" + asset.hash + "." + asset.ext + ")");
  if (located != nullptr)
    *located = found;
  return read.preset;
}

bool TONE3000Processor::loadPreset(const juce::String& presetId) {
  serviceHostedPlugins();  // the undo snapshot below reads cached plugin chunks
  std::optional<SetlistLibrary::Located> located;
  const juce::ValueTree preset = readPresetTree(presetId, &located);
  if (!applyPresetTree(preset, presetId))
    return false;

  // The position follows a load inside the current setlist. A preset loaded
  // from elsewhere by its id (a favourite, a recent) leaves it where it is.
  const juce::String setlistId = currentSetlistId();
  std::optional<SetlistLibrary::Position> position;
  if (located.has_value() && located->setlist.id == setlistId) {
    position = SetlistLibrary::Position{setlistId, located->slot.slot};
  } else if (!located.has_value() && setlistId == kFactorySetlistId) {
    const auto factory = factoryPresets();
    for (size_t i = 0; i < factory.size(); ++i)
      if (factory[i].id == presetId)
        position = SetlistLibrary::Position{setlistId, static_cast<int>(i) + 1};
  }
  userLibrary().noteLoaded(presetId, position);
  return true;
}

bool TONE3000Processor::applyPresetTree(const juce::ValueTree& preset,
                                        const juce::String& presetId) {
  return applyPresetTree(preset, presetId, preset.getProperty("name").toString());
}

bool TONE3000Processor::applyPresetTree(const juce::ValueTree& preset, const juce::String& presetId,
                                        const juce::String& presetName) {
  const juce::ValueTree snapshot = preset.getChildWithName("ChainSnapshot");
  if (!snapshot.isValid())
    return false;
  if (const juce::String refusal = gridSnapshotRefusal(snapshot); refusal.isNotEmpty()) {
    juce::Logger::writeToLog("[Presets] Cannot load preset: " + refusal);
    return false;
  }

  // A preset replaces the whole chain (and jumps the faceplate parameters
  // below), so mute-splice the transition like any structural edit. The fade
  // holds until everything is in place, then glides back in on the new rig.
  ChainEditFade editFade(*this);

  Lane retired;  // destroyed after the lock; see restoreChainSnapshot
  {
    juce::ScopedLock lock(chainMutex);
    pushChainHistory();
    retired = restoreChainSnapshot(snapshot);  // bumps the revision
    activePresetId = presetId;
    activePresetName = presetName;
  }
  retired.clear();
  // What the restore queued for the legacy image controls, before the
  // preset's own values go on.
  flushLegacyParamWrites();

  // Faceplate parameters: every preset-managed id is set, entries missing
  // from the file (saved before a parameter existed) land on the parameter
  // default, so a preset always restores the same rig. Ids an older build
  // wrote are renamed first (in this call's copy of the tree; the file is
  // rewritten with the current ids on its next save). Gestured so hosts
  // treat this like a user edit (automation write modes record it instead
  // of fighting it).
  juce::ValueTree params = preset.getChildWithName("Params");
  t3k::legacy_ids::migrateParamIds(params, "id");
  // The preset's values, not moves of a control: a grid preset's rows have
  // just been restored and are what counts (see legacyControlMoved).
  legacyControlSyncDepth.fetch_add(1);
  for (const auto& paramId : presetParameterIds()) {
    auto* p = parameters.getParameter(paramId);
    if (p == nullptr)
      continue;
    const juce::ValueTree paramTree = params.getChildWithProperty("id", paramId);
    // A value that is no number (a preset file can say NaN or an infinity)
    // is a missing one: the default, never a NaN handed to the host.
    const double value = paramTree.isValid()
                             ? static_cast<double>(paramTree.getProperty("value"))
                             : std::numeric_limits<double>::quiet_NaN();
    float norm = std::isfinite(value) ? p->convertTo0to1(static_cast<float>(value))
                                      : p->getDefaultValue();
    if (!std::isfinite(norm))
      norm = p->getDefaultValue();
    p->beginChangeGesture();
    p->setValueNotifyingHost(norm);
    p->endChangeGesture();
  }
  legacyControlSyncDepth.fetch_sub(1);
  if (isGridMode()) {
    juce::ScopedLock lock(chainMutex);
    // A grid preset: the image controls are the rows' or rest at neutral.
    syncLegacyControlsFromGrid();
  }
  flushLegacyParamWrites();

  // A preset may name its tempo (none this build saves does; see
  // docs/library.md). Without one the rig's tempo stays as it is.
  if (const juce::var tempo = preset.getProperty("tempo"); tempo.isDouble() || tempo.isInt())
    setTempo(static_cast<double>(tempo));

  juce::Logger::writeToLog("[Presets] Loaded preset: " + activePresetName);
  markPresetBaseline();

  // Tell hosts the current program moved so their program parameter/menus
  // follow (the VST3 wrapper syncs its program parameter off this; its echo
  // is suppressed by applyHostProgram's already-active guard).
  updateHostDisplay(ChangeDetails{}.withProgramChanged(true));

  // The restore queued every block's engine build on the background loader;
  // hold the mute until they land (bounded), else the chain fades back in on
  // unloaded pass-through blocks and blasts the raw dry input.
  editFade.releaseWhenChainLoadsSettle();
  return true;
}

// ####################
// A RIG FROM ANOTHER PROCESSOR
// ####################
// What SandboxRig::applyTo is made of: the rig as a tree out of one
// processor and into another, the way a preset load would put it there, but
// with nothing read from or written to the library.

juce::ValueTree TONE3000Processor::captureRigTree() { return buildPresetTree(); }

bool TONE3000Processor::applyRigTree(const juce::ValueTree& rig, const juce::String& presetId,
                                     const juce::String& presetName, bool unsaved) {
  serviceHostedPlugins();  // the undo snapshot reads cached plugin chunks (as loadPreset)
  // The name is the caller's to give: the tree itself has none (see
  // buildPresetTree).
  if (!applyPresetTree(rig, presetId, presetId.isNotEmpty() ? presetName : juce::String()))
    return false;
  if (unsaved && presetId.isNotEmpty()) {
    // applyPresetTree took the rig as the preset's own; this one is known
    // not to be.
    juce::ScopedLock lock(chainMutex);
    presetBaseline.knownUnsaved = true;
  }
  return true;
}

bool TONE3000Processor::renamePreset(const juce::String& presetId, const juce::String& newName) {
  const auto located = userLibrary().find(presetId, currentSetlistId());
  if (!located.has_value() ||
      userLibrary().rename(located->setlist.id, located->slot.slot, newName).failed())
    return false;  // a factory preset, an unknown id, or a blank name
  {
    juce::ScopedLock lock(chainMutex);
    if (activePresetId == presetId) {
      activePresetName = SetlistLibrary::normaliseName(newName.trim());
      bumpChainRevision();
    }
  }
  presetListChanged();  // host program names follow the rename
  return true;
}

bool TONE3000Processor::deletePreset(const juce::String& presetId) {
  const auto located = userLibrary().find(presetId, currentSetlistId());
  if (!located.has_value() ||
      userLibrary().remove(located->setlist.id, located->slot.slot).failed())
    return false;
  {
    juce::ScopedLock lock(chainMutex);
    if (activePresetId == presetId) {
      activePresetId.clear();
      activePresetName.clear();
      presetBaseline = {};
      bumpChainRevision();
    }
  }
  // Its slot is empty now (the other presets keep theirs, which is the
  // point of slots), and the active preset may be gone: host program state
  // moved.
  presetListChanged();
  return true;
}

bool TONE3000Processor::movePreset(const juce::String& presetId, int delta) {
  // Pure list-order change: nothing about the loaded chain moves, so no
  // revision bump; the UI re-pulls the preset list after the call. Program
  // numbers follow the list order though, so host displays must refresh.
  const juce::String setlistId = currentSetlistId();
  const auto located = userLibrary().find(presetId, setlistId);
  if (delta == 0 || !located.has_value() || located->setlist.id != setlistId ||
      userLibrary().moveByDelta(setlistId, located->slot.slot, delta).failed())
    return false;
  presetListChanged();
  return true;
}

bool TONE3000Processor::isChainAtDefault() const {
  if (activePresetId.isNotEmpty() || stereoEnabled.load() || routingMode == RoutingMode::Grid)
    return false;
  // Scene names, the active scene and stomp labels can outlive the blocks.
  if (!scenes.isPristine() || !stomps.isEmpty())
    return false;
  for (const auto& l : lanes)
    for (const auto& b : l)
      if (b->type != ChainBlockType::INSERT)
        return false;
  // Normalized-space tolerance: UI resets and preset loads land exactly on
  // the default, but a knob dragged back or host automation can be a hair
  // off. 1e-4 of a ±24 dB range is 0.005 dB, far below any UI step.
  for (const auto& paramId : presetParameterIds())
    if (auto* p = parameters.getParameter(paramId))
      if (std::abs(p->getValue() - p->getDefaultValue()) > 1.0e-4f)
        return false;
  return true;
}

bool TONE3000Processor::resetToDefault() {
  {
    juce::ScopedLock lock(chainMutex);
    if (isChainAtDefault())
      return false;  // nothing to reset; spare the audio the edit fade
  }

  // The whole chain is replaced, so mute-splice the swap like a preset load.
  ChainEditFade editFade(*this);

  Lane retired;  // destroyed after the lock; see restoreChainSnapshot
  {
    juce::ScopedLock lock(chainMutex);
    pushChainHistory();
    // A bare snapshot *is* the default state: the restore retires every
    // block (lanes pad back to fresh insert slots), turns stereo off and
    // clears the branch, in one undoable step.
    retired = restoreChainSnapshot(juce::ValueTree("ChainSnapshot"));
    activePresetId.clear();
    activePresetName.clear();
    presetBaseline = {};
  }
  retired.clear();
  flushLegacyParamWrites();  // before the defaults below, which win

  // Gestured like loadPreset, so hosts treat the jumps as user edits.
  for (const auto& paramId : presetParameterIds()) {
    if (auto* p = parameters.getParameter(paramId)) {
      p->beginChangeGesture();
      p->setValueNotifyingHost(p->getDefaultValue());
      p->endChangeGesture();
    }
  }

  juce::Logger::writeToLog("[Presets] Reset to default");
  // The active preset is gone, so the host program index fell back (see
  // getCurrentProgram); keep host program displays in step.
  updateHostDisplay(ChangeDetails{}.withProgramChanged(true));
  // No deferred fade release: an empty chain queues no model loads.
  return true;
}

// #############################
// HOST PROGRAM API
// #############################
//
// The current setlist, exposed as the JUCE/host program list: program n is
// slot n + 1, so the first 128 slots are reachable and an empty slot is an
// empty program. This is
// what makes MIDI program changes work in VST3 hosts (Cubase, Ableton, VST
// Live; GitHub issue #38): VST3 never delivers PC as MIDI. JUCE's wrapper
// reconstructs CCs/notes/pitch bend/aftertouch from IMidiMapping emulation
// parameters, but a program change can only arrive as a change of the
// wrapper's program parameter, which exists only when getNumPrograms() > 1.
// Hosts map incoming PC n onto that parameter and the wrapper lands it in
// setCurrentProgram(n) on the message thread. Raw-MIDI formats (Standalone,
// AU, LV2, CLAP) keep delivering PC through MidiMapper::processMidi →
// loadPresetAtIndex; both routes load the preset at index n in list order,
// so a preset's PC number (shown in the preset browser) means the same tone
// everywhere.
//
// Known wrapper edge, accepted: on an instance that never had a preset
// active, getCurrentProgram() necessarily reports 0, and JUCE's VST3
// wrapper drops program changes that match the current program, so the
// very first PC targeting program 0 is swallowed until any other program
// (or any preset via UI/MIDI) has been selected. Every JUCE plugin with
// programs shares this; the alternative (reserving slot 0) would shift the
// advertised "PC n = nth preset" numbering.

int TONE3000Processor::getNumPrograms() {
  return kNumHostPrograms;
}

int TONE3000Processor::getCurrentProgram() {
  juce::String id;
  {
    juce::ScopedLock lock(chainMutex);
    id = activePresetId;
  }
  if (id.isEmpty())
    return 0;  // no active preset; the API still needs a valid index
  // Fresh list, not the name cache: guards compare against this (see
  // applyHostProgram and the wrapper's own echo suppression), so it must
  // track the store exactly. One scan per call, event-rate only.
  const auto presets = currentPresetSlots();
  const int count = juce::jmin(static_cast<int>(presets.size()), kNumHostPrograms);
  for (int i = 0; i < count; ++i)
    if (presets[static_cast<size_t>(i)].id == id)
      return i;
  return 0;  // active preset deleted elsewhere or beyond the program range
}

void TONE3000Processor::setCurrentProgram(int index) {
  if (index < 0 || index >= kNumHostPrograms)
    return;
  // Preset loads are heavyweight message-thread work (chain lock, undo
  // history, background model builds). VST3 lands here on the message
  // thread, but the AudioProcessor contract doesn't promise one, so defer
  // exactly like MidiMapper defers its PC deliveries.
  if (juce::MessageManager::existsAndIsCurrentThread()) {
    applyHostProgram(index);
  } else {
    pendingHostProgram.store(index);
    triggerAsyncUpdate();
  }
}

void TONE3000Processor::applyHostProgram(int index) {
  const auto presets = currentPresetSlots();
  if (index < 0 || index >= static_cast<int>(presets.size()) ||
      presets[static_cast<size_t>(index)].id.isEmpty())
    return;  // empty program slot: ignore, like an out-of-range PC
  {
    juce::ScopedLock lock(chainMutex);
    if (activePresetId == presets[static_cast<size_t>(index)].id)
      return;  // already active: host echo / re-select, don't reload
  }
  loadPreset(presets[static_cast<size_t>(index)].id);
}

const juce::String TONE3000Processor::getProgramName(int index) {
  // Mutations clear the snapshot; the 500 ms expiry re-reads names saved by
  // another instance sharing the preset folder.
  const auto now = juce::Time::getMillisecondCounter();
  if (hostProgramInfoCache.empty() || now - hostProgramInfoCacheTime > 500) {
    hostProgramInfoCache = currentPresetSlots();
    hostProgramInfoCacheTime = now;
  }
  if (index >= 0 && index < static_cast<int>(hostProgramInfoCache.size()) &&
      hostProgramInfoCache[static_cast<size_t>(index)].id.isNotEmpty())
    return hostProgramInfoCache[static_cast<size_t>(index)].name;
  return "(empty)";
}

void TONE3000Processor::changeProgramName(int, const juce::String&) {
  // Preset renames go through renamePreset (the preset browser); host
  // program-name edits are not supported.
}
