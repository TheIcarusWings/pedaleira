#include "Processor.h"
#include "AppIdentity.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "FiniteValue.h"
#include "LegacyParamIds.h"

// #############################
// STATE PERSISTENCE
// #############################

// Machine-wide user settings.
// Shared PropertiesFile for preferences that belong to the machine, not the
// session/preset (multi-core processing, the default A2 size for new
// blocks, and the machine defaults for the calibration and oversampling
// parameters). Same app-data root as PresetManager: ~/Library/Application
// Support/TONE3000 on macOS, %APPDATA%/TONE3000 on Windows,
// $XDG_CONFIG_HOME/TONE3000 (default ~/.config/TONE3000) on Linux.
namespace {

constexpr auto kMultiCoreKey = "multiCore";
constexpr auto kNamSlimSizeDefaultKey = "namSlimSizeDefault";

// Plugin Settings parameters that double as machine-wide defaults (see
// Processor.h, isMachineDefaultParameter). Each is stored under its
// parameter id.
constexpr const char* kMachineDefaultParameterIds[] = {"calibrateInput", "inputCalibrationLevel",
                                                       "osEnabled", "osFactor"};

// Process-wide test switch for the constructor's seeding (see
// disableMachineDefaultParametersForTesting).
std::atomic<bool> machineDefaultParametersEnabled{true};

// Magic prefix for the binary ValueTree state format (see getStateInformation).
constexpr char kStateMagic[] = {'T', '3', 'K', 'B'};

// Bump when the TONE3000State tree changes shape. Readers ignore state from a
// newer schema rather than guessing at it.
// The newest state this build reads. 1 is the legacy rig, and what a legacy
// rig is still saved as, byte for byte; 2 adds the grid layout and is
// written only for a grid rig.
constexpr int kStateSchemaVersion = 2;
constexpr int kLegacyStateSchemaVersion = 1;

juce::PropertiesFile::Options userSettingsOptions() {
  juce::PropertiesFile::Options options;
  // getDefaultFile() uses applicationName as the filename stem. "preferences"
  // keeps this store distinct from the standalone holder's TONE3000.settings
  // in the same folder: two PropertiesFile instances on one file clobber each
  // other, since each save rewrites the whole file from its in-memory copy.
  options.applicationName = "preferences";
  options.filenameSuffix = ".settings";
  options.osxLibrarySubFolder = "Application Support";
#if JUCE_LINUX || JUCE_BSD
  // PropertiesFile puts a bare folderName directly under ~ on Linux, so pass
  // the XDG config location as an absolute path instead (same root as
  // PresetManager and the logs).
  options.folderName =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile(kAppDataFolderName)
          .getFullPathName();
#else
  options.folderName = kAppDataFolderName;
#endif
  return options;
}

// Save now and say so when it fails: an unwritable app-data folder used to
// make settings silently vanish on every relaunch, and the log's startup
// snapshot ("exists=no" forever) was the only trace (github issue #76). The
// constructor heals the folder (see ensureWritableDir); this keeps the
// write itself honest.
void saveSettingsOrLog(juce::PropertiesFile& settings) {
  if (!settings.saveIfNeeded())
    juce::Logger::writeToLog("[Processor] Couldn't write the settings file: " +
                             settings.getFile().getFullPathName());
}

}  // namespace

juce::File TONE3000Processor::getSettingsFile() {
  return userSettingsOptions().getDefaultFile();
}

juce::PropertiesFile::Options TONE3000Processor::uiPreferencesOptions() {
  // Same folder as the shared settings, own file: the native UI's per-machine
  // preferences get written by the editor on every toggle, and two
  // PropertiesFile instances must never share a file.
  auto options = userSettingsOptions();
  options.applicationName = "ui-preferences";
  return options;
}

bool TONE3000Processor::readPersistedMultiCoreEnabled() {
  return juce::PropertiesFile(userSettingsOptions()).getBoolValue(kMultiCoreKey, true);
}

double TONE3000Processor::readPersistedNamSlimSizeDefault() {
  // 0.0 = lite, the shipped default (see NamBlockProcessor::slimSize).
  return juce::jlimit(
      0.0, 1.0,
      juce::PropertiesFile(userSettingsOptions()).getDoubleValue(kNamSlimSizeDefaultKey, 0.0));
}

void TONE3000Processor::setMultiCoreEnabled(bool enabled, bool persist) {
  if (multiCoreEnabled.load() == enabled)
    return;

  // No fade, no lock: the flag only picks serial vs. parallel scheduling for
  // the next callback, and both schedules produce bit-identical audio.
  multiCoreEnabled.store(enabled);
  if (persist) {
    juce::PropertiesFile settings(userSettingsOptions());
    settings.setValue(kMultiCoreKey, enabled);
    saveSettingsOrLog(settings);
  }

  juce::Logger::writeToLog(juce::String("[Processor] Multi-core processing ") +
                           (enabled ? "enabled" : "disabled"));
  bumpChainRevision();
}
void TONE3000Processor::setNamSlimSizeDefault(double slimSize) {
  slimSize = juce::jlimit(0.0, 1.0, slimSize);
  if (namSlimSizeDefault.load() == slimSize)
    return;

  // No fade, no lock: loaded engines are untouched (each block owns its
  // size); this only decides what loadTone stamps on the next new block.
  namSlimSizeDefault.store(slimSize);
  juce::PropertiesFile settings(userSettingsOptions());
  settings.setValue(kNamSlimSizeDefaultKey, slimSize);
  saveSettingsOrLog(settings);

  juce::Logger::writeToLog("[Processor] Default NAM A2 size set to " + juce::String(slimSize));
  bumpChainRevision();
}

bool TONE3000Processor::isMachineDefaultParameter(const juce::String& paramId) {
  for (const auto* id : kMachineDefaultParameterIds)
    if (paramId == id)
      return true;
  return false;
}

void TONE3000Processor::disableMachineDefaultParametersForTesting() {
  machineDefaultParametersEnabled.store(false);
}

void TONE3000Processor::setDeterministicRandomForOfflineRender(std::int64_t seed) {
  DeckWobble::setSeedOverride(seed);
}

void TONE3000Processor::clearDeterministicRandomForOfflineRender() {
  DeckWobble::clearSeedOverride();
}

void TONE3000Processor::writeMachineDefaultParameter(juce::PropertySet& settings,
                                                     const juce::String& paramId) const {
  auto* p = parameters.getParameter(paramId);
  if (p == nullptr)
    return;
  settings.setValue(paramId, static_cast<double>(p->convertFrom0to1(p->getValue())));
}

void TONE3000Processor::applyMachineDefaultParameters(const juce::PropertySet& settings) {
  juce::String applied;
  for (const auto* id : kMachineDefaultParameterIds) {
    if (!settings.containsKey(id))
      continue;
    auto* p = parameters.getParameter(id);
    if (p == nullptr)
      continue;
    // Out-of-range file values (a hand edit, a future build's wider range)
    // clamp to the parameter's own range through convertTo0to1.
    const auto denormalised = static_cast<float>(settings.getDoubleValue(id));
    p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, p->convertTo0to1(denormalised)));
    applied << (applied.isEmpty() ? "" : ", ") << id << "=" << settings.getValue(id);
  }
  if (applied.isNotEmpty())
    juce::Logger::writeToLog("[Processor] Machine defaults applied: " + applied);
}

void TONE3000Processor::persistParameterAsMachineDefault(const juce::String& paramId) {
  // Only the Settings-page set: a stray id here would turn a tone parameter
  // into a machine-wide default.
  if (!isMachineDefaultParameter(paramId)) {
    jassertfalse;
    return;
  }
  juce::PropertiesFile settings(userSettingsOptions());
  writeMachineDefaultParameter(settings, paramId);
  saveSettingsOrLog(settings);
}

void TONE3000Processor::seedMachineDefaultParameters() {
  if (!machineDefaultParametersEnabled.load())
    return;
  // Nothing on disk yet: a fresh install runs on the parameter defaults.
  if (!getSettingsFile().existsAsFile())
    return;
  applyMachineDefaultParameters(juce::PropertiesFile(userSettingsOptions()));
}

juce::ValueTree TONE3000Processor::serializeBlockSettings(const ChainBlock& block) {
  // A kind this build does not have is written back exactly as it came:
  // nothing here understands the node well enough to rebuild it.
  if (block.isMissingKind()) {
    juce::ValueTree kept = block.missingKindNode.createCopy();
    kept.setProperty("id", juce::String(block.id), nullptr);  // a clone has its own id
    return kept;
  }

  // A registry-kind block with a tone on its way in (swapTone; the block is
  // still KIND until the model lands) is written as the tone block it is
  // becoming, typed by the tone's format: that is what the user asked for
  // and what the UI shows, and it reloads like any tone block. Writing the
  // kind instead would save a node that has both a tone and no tone.
  const bool toneOnKind = block.hasToneOnKind();
  juce::String typeString = chainBlockTypeToString(block.type);
  if (toneOnKind) {
    juce::String format = block.toneVar["format"].toString().toLowerCase();
    if (format.isEmpty())
      format = block.toneVar["platform"].toString().toLowerCase();
    typeString = chainBlockTypeToString(format == "nam" ? ChainBlockType::NAM : ChainBlockType::IR);
  }

  juce::ValueTree blockState("ChainBlock");

  blockState.setProperty("id", juce::String(block.id), nullptr);
  blockState.setProperty("type", typeString, nullptr);
  blockState.setProperty("enabled", block.enabled, nullptr);
  // The type-specific settings. Every node carries both whatever its type
  // (see BlockProcessorState): the block's copy fills in what its processor
  // does not own, the processor answers for what it does.
  BlockProcessorState processorState = block.processorState;
  if (block.processor != nullptr)
    block.processor->getState(processorState);
  blockState.setProperty("normalize", processorState.normalize, nullptr);
  blockState.setProperty("slimSize", processorState.slimSize, nullptr);
  blockState.setProperty("inputGain", block.inputGainNormalized, nullptr);
  blockState.setProperty("outputGain", block.outputGainNormalized, nullptr);
  blockState.setProperty("mix", block.mixNormalized, nullptr);

  if (block.type == ChainBlockType::KIND && !toneOnKind) {
    // A registry kind: no tone, and everything the kind owns in ONE child
    // (see BlockProcessor::getKindState), so the NAM / IR node shape above
    // never has to grow for a new kind. A block whose processor is still
    // being built writes the node it is being built from.
    blockState.appendChild(block.eq.toValueTree(), nullptr);
    if (block.processor != nullptr)
      blockState.appendChild(block.processor->getKindState(block.kindName), nullptr);
    else if (block.pendingKindState.isValid())
      blockState.appendChild(block.pendingKindState.createCopy(), nullptr);
  } else if (block.type != ChainBlockType::INSERT) {
    blockState.setProperty("toneId", block.toneId, nullptr);
    blockState.setProperty("toneJson", block.toneJson, nullptr);
    blockState.setProperty("activeModelId", block.activeModelId, nullptr);
    blockState.appendChild(block.eq.toValueTree(), nullptr);
  }

  return blockState;
}

void TONE3000Processor::applyKindState(ChainBlock& block, const juce::ValueTree& blockState) {
  const juce::ValueTree kindState = blockState.getChildWithName("KindState");
  const juce::String kindId = kindState.getProperty("kind").toString();
  const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);

  // A JSON preset whose asset for this block is missing (a plugin's state
  // that never made it into the library folder): nothing sensible can be
  // built from half a state. The block stays where it is as a bypassed
  // placeholder under its name, and its node is kept, so the next save
  // writes the reference back and the preset is whole again the day the
  // file turns up.
  const bool assetMissing = t3k::presetjson::hasMissingAsset(kindState);

  // The node says this block is a registry kind and nothing else. A live
  // block reused for it may be carrying more: a tone being swapped onto it
  // (still downloading, or failed), a kind build from an earlier restore.
  // All of that ends here. Clearing the active model is what retires a
  // model load in flight (the loaders check it before they touch the
  // block); bumping the serial does the same for a kind build.
  setToneOnBlock(block, 0, {}, {});
  block.activeModelId = 0;
  block.modelCache.clear();
  block.loadFailed = false;
  block.applyDefaultMixOnLoad = false;
  block.missingKindNode = {};
  block.pendingKindState = {};
  const int serial = ++block.kindBuildSerial;

  // A hosted plugin is a registry kind in every way but how its processor
  // comes to be: around an instance of one particular plugin, created on
  // the message thread (ProcessorPlugins.cpp).
  if (kindId == HostedPluginBlockProcessor::kKindId && !assetMissing) {
    applyHostedPluginState(block, blockState, kindState, serial);
    return;
  }

  if (info == nullptr || !info->addable || assetMissing) {
    // A kind this build cannot make (a preset from a newer build, a plugin
    // that is not installed). Keep the block where it is as a bypassed
    // placeholder under the name it was saved with, and keep its node so
    // the next save writes it back untouched.
    juce::Logger::writeToLog("[Restore] Block " + juce::String(block.id) +
                             (assetMissing ? " is missing an asset of its '"
                                           : " is of unknown kind '") +
                             kindId + "'; kept as a bypassed placeholder");
    // A plugin the block is still hosting goes, as it does whenever the
    // block gets a new instance or none (see applyHostedPluginState): its
    // state is not the saved one.
    if (assetMissing && HostedPluginBlockProcessor::from(block.processor.get()) != nullptr)
      block.processor.reset();
    block.kindId = kindId;
    block.kindName = kindState.getProperty("name").toString();
    if (block.kindName.isEmpty())
      block.kindName = kindId.isNotEmpty() ? kindId : juce::String("Unknown block");
    block.missingKindNode = blockState.createCopy();
    block.loaded = false;
    block.modelLoading = false;
    return;
  }

  // Already running this kind (an undo over a knob move, a preset that
  // keeps the block): only the values move, on the live processor, and a
  // latency that follows them is adopted here (every caller holds the
  // chain lock and the chain-edit fade, and refreshes the compensation
  // before audio resumes). The block is back in processing whatever took
  // it out: a failed tone swap, or its own processor reporting Failed
  // under settings this restore has just replaced.
  if (block.processor != nullptr && block.processor->kind() == ChainBlockType::KIND &&
      block.kindId == kindId) {
    block.processor->setKindState(kindState);
    block.processor->commitLatency();
    block.modelLoading = false;
    block.loaded = true;
    block.processingFailed = false;
    return;
  }

  // A processor has to be built. Not here: construction and a full prepare
  // can be heavy, and this runs under the chain lock. The block waits for
  // it like a tone block waits for its model, which is also what holds a
  // restore's mute until it has landed (releaseChainEditFadeWhenLoadsSettle
  // watches modelLoading).
  block.kindId = kindId;
  block.kindName = info->name;
  block.pendingKindState = kindState.createCopy();
  block.loaded = false;
  block.modelLoading = true;
  loadingThreadPool.addJob(std::function<void()>(
      [this, blockId = block.id, kindId, state = kindState.createCopy(), serial]() {
        buildKindInBackground(blockId, kindId, state, serial);
      }));
}

void TONE3000Processor::buildKindInBackground(const std::string& blockId,
                                              const juce::String& kindId,
                                              const juce::ValueTree& kindState, int serial) {
  PreparedBlockModel prepared;
  if (const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId))
    prepared.processor = info->factory();
  if (prepared.processor != nullptr) {
    // Values first, so the prepare snaps the kind's smoothers (and latches
    // its latency) to them instead of gliding in from the defaults.
    prepared.processor->setKindState(kindState);
    prepared.processor->prepareForChain(blockPrepareSpecForId(blockId));
    prepared.success = true;
  } else {
    juce::Logger::writeToLog("[Restore] Could not build a '" + kindId + "' processor for block " +
                             juce::String(blockId));
  }

  // A kind with latency moves its lane in time as it lands, so the chain is
  // muted around the install. Unless it already is: a restore holds its
  // mute until this very build settles, and owns the release.
  std::unique_ptr<ChainEditFade> editFade;
  if (prepared.success && prepared.processor->chainLatencySamples() > 0 &&
      !chainEditFadePending.load())
    editFade = std::make_unique<ChainEditFade>(*this);

  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    // Gone, turned into something else, or waiting for a newer build.
    if (block == nullptr || block->type != ChainBlockType::KIND ||
        block->kindBuildSerial != serial)
      return;

    // A failed build leaves the node in place, so the block still saves
    // whole; a successful install clears it.
    const juce::ValueTree keptState = block->pendingKindState;
    applyPreparedModelToChainBlock(*block, ChainBlockType::KIND, prepared);
    if (!prepared.success)
      block->pendingKindState = keptState;
  }
  // `prepared` is empty or holds the block's previous processor; either way
  // it dies here, after the lock.
}

void TONE3000Processor::applyBlockSettings(ChainBlock& block, const juce::ValueTree& blockState) {
  block.enabled = static_cast<bool>(blockState.getProperty("enabled", true));
  block.processorState.normalize = static_cast<bool>(blockState.getProperty("normalize", true));
  // Through finiteFloat: a preset file can say {"$double": "nan"} or an
  // infinity for any of these, and one of them in a gain or a mix is silence
  // or worse for the rest of the chain. What is not a number is the default;
  // a number is taken as it is, as it always was.
  block.inputGainNormalized = t3k::finiteFloat(blockState.getProperty("inputGain"), 0.5f);
  block.outputGainNormalized = t3k::finiteFloat(blockState.getProperty("outputGain"), 0.5f);
  block.mixNormalized = t3k::finiteFloat(blockState.getProperty("mix"), 1.0f);

  // States from before per-block sizes restore as lite (0.0). An engine the
  // restore keeps loaded (see reconcileChainFromTree) retiers in place: the
  // NAM container fast-paths an unchanged tier, and every restore path runs
  // under the chain-edit fade, so a real retier splices in silently.
  const double slimSize = static_cast<double>(blockState.getProperty("slimSize", 0.0));
  block.processorState.slimSize = std::isfinite(slimSize) ? juce::jlimit(0.0, 1.0, slimSize) : 0.0;
  block.pushProcessorState();

  if (block.type != ChainBlockType::INSERT) {
    // A missing Eq child restores as flat. The restored bands are then
    // designed for the live chain rate: this is the creation funnel for
    // duplicated, pasted and undo/preset-restored blocks, none of which
    // prepareChain has seen.
    block.eq.restoreFromValueTree(blockState.getChildWithName("Eq"));
    prepareBlockForChainRate(block);
  }

  // Last, so a kind built here is installed with the gains and mix above
  // already in place.
  if (block.type == ChainBlockType::KIND)
    applyKindState(block, blockState);
}

void TONE3000Processor::serializeChainToTree(
    const std::vector<std::unique_ptr<ChainBlock>>& blocks, juce::ValueTree& chainState,
    bool includeModelData) {
  for (const auto& block : blocks) {
    juce::ValueTree blockState = serializeBlockSettings(*block);

    // Model bytes only exist for the model-based types. (A missing-kind
    // placeholder's node also has to go out exactly as it came in.)
    if (includeModelData && block->type != ChainBlockType::INSERT &&
        block->type != ChainBlockType::KIND) {
      juce::ValueTree cacheState("ModelCache");
      for (const auto& [modelId, modelData] : block->modelCache) {
        // Only models the block's tone still references are persisted: the
        // active model (what the project needs to reopen offline) and, for
        // local tones, the rest of their stored model list. Auditioned
        // catalog models accumulate in the in-memory cache (switchModel
        // collapses toneJson to the active model but never evicts the old
        // bytes); persisting them wrote 50-224 MB states nothing could ever
        // read again, which hosts then multiplied across autosaves and
        // backups (issue #127).
        if (!block->referencesModel(modelId))
          continue;

        juce::ValueTree cachedModel("CachedModel");
        cachedModel.setProperty("modelId", modelId, nullptr);

        // Raw bytes in a binary var. The ValueTree binary stream writes these
        // verbatim, which matters because this can run with chainMutex held
        // (~8 MB per heavy rig).
        cachedModel.setProperty(
            "data", juce::var(juce::MemoryBlock(modelData.data(), modelData.size())), nullptr);

        cacheState.appendChild(cachedModel, nullptr);
      }
      blockState.appendChild(cacheState, nullptr);
    }

    chainState.appendChild(blockState, nullptr);
  }
}

void TONE3000Processor::getStateInformation(juce::MemoryBlock& destData) {
  // Hosted plugins are saved from a cached chunk (the snapshot below is
  // taken under the chain lock, where a plugin must not be called). A save
  // is the moment to make that cache current, also for plugins that never
  // report their changes. Message thread only; elsewhere the cache stands.
  serviceHostedPlugins(/*forceStateRefresh=*/true);

  juce::ValueTree state("TONE3000State");
  // Decided together with the snapshot below would be tidier, but the
  // property comes first in the tree and a legacy save has to stay exactly
  // the bytes it was. The mode only changes on the message thread, where
  // hosts save.
  state.setProperty("schemaVersion",
                    isGridMode() ? kStateSchemaVersion : kLegacyStateSchemaVersion, nullptr);

  state.appendChild(parameters.copyState(), nullptr);

  // Session-only settings. Presets deliberately don't carry these: input
  // mode is I/O routing, editor scale is a workstation preference, and the
  // MIDI map describes the user's rig, not the tone. (Per-block NAM A2
  // sizes ride the chain snapshot below, with presets and undo.)
  state.setProperty("inputMode", inputModeToString(getInputMode()), nullptr);
  state.setProperty("editorScale", editorScale.load(), nullptr);
  state.setProperty("editorExtraHeight", editorExtraHeight.load(), nullptr);
  state.appendChild(midiMapper.toValueTree(), nullptr);

  {
    juce::ScopedLock lock(chainMutex);
    state.setProperty("activePresetId", activePresetId, nullptr);
    state.setProperty("activePresetName", activePresetName, nullptr);
    // The same ChainSnapshot tree that undo and presets use, with the
    // referenced models' bytes embedded so the project reopens offline
    // (see serializeChainToTree for what qualifies).
    const juce::ValueTree snapshot = captureChainSnapshot(true);
    state.appendChild(snapshot, nullptr);
    // Should the mode have moved between the property above and this lock
    // after all, the snapshot is what counts.
    state.setProperty("schemaVersion",
                      snapshot.getProperty("layout").toString() == "grid"
                          ? kStateSchemaVersion
                          : kLegacyStateSchemaVersion,
                      nullptr);
  }

  // Magic-prefixed binary ValueTree stream. I picked binary over XML so the
  // embedded model bytes go out verbatim; the old Base64-in-XML path burned
  // 100+ ms per host save on a heavy rig.
  juce::MemoryOutputStream out(destData, false);
  out.write(kStateMagic, sizeof(kStateMagic));
  state.writeToStream(out);
  DBG("Plugin state saved successfully");
}

void TONE3000Processor::setStateInformation(const void* data, int sizeInBytes) {
  juce::ValueTree state;
  if (sizeInBytes > static_cast<int>(sizeof(kStateMagic)) &&
      std::memcmp(data, kStateMagic, sizeof(kStateMagic)) == 0) {
    state = juce::ValueTree::readFromData(
        static_cast<const char*>(data) + sizeof(kStateMagic),
        static_cast<size_t>(sizeInBytes) - sizeof(kStateMagic));
  }

  if (!state.isValid()) {
    juce::Logger::writeToLog("[Restore] Failed to parse plugin state (" +
                             juce::String(sizeInBytes) + " bytes)");
    return;
  }

  if (static_cast<int>(state.getProperty("schemaVersion", 1)) > kStateSchemaVersion) {
    juce::Logger::writeToLog("[Restore] State schema is newer than this build; ignoring");
    return;
  }

  const juce::ValueTree snapshot = state.getChildWithName("ChainSnapshot");
  // A grid written by a build whose Grid node this one cannot read: ignored
  // whole, like a newer schema, rather than half loaded.
  if (const juce::String refusal = gridSnapshotRefusal(snapshot); refusal.isNotEmpty()) {
    juce::Logger::writeToLog("[Restore] Ignoring the state: " + refusal);
    return;
  }
  juce::Logger::writeToLog(
      "[Restore] Restoring state (" + juce::String(sizeInBytes) + " bytes, " +
      juce::String(snapshot.getChildWithName("ChainBlocks").getNumChildren()) + " left / " +
      juce::String(snapshot.getChildWithName("RightChainBlocks").getNumChildren()) +
      " right blocks)");

  // Parameter ids an older build wrote are renamed in place before anything
  // reads them (the tree is this call's own copy); the next save writes the
  // current ids.
  juce::ValueTree parameterState = state.getChildWithName("PARAMETERS");
  juce::ValueTree midiState = state.getChildWithName("MidiMappings");
  if (const int renamed = t3k::legacy_ids::migrateParamIds(parameterState, "id") +
                          t3k::legacy_ids::migrateParamIds(midiState, "targetId");
      renamed > 0)
    juce::Logger::writeToLog("[Restore] Renamed " + juce::String(renamed) + " legacy parameter ids");

  if (parameterState.isValid()) {
    // The state's own values, not a move of a control: in a grid they are
    // not to be carried onto the rows (see legacyControlMoved). The restore
    // below brings the rows, and puts the controls where they belong.
    legacyControlSyncDepth.fetch_add(1);
    parameters.replaceState(parameterState);
    legacyControlSyncDepth.fetch_sub(1);
    DBG("Parameters restored from state");
  }

  inputMode.store(static_cast<int>(
      inputModeFromString(state.getProperty("inputMode").toString())));

  // Older projects have no editorScale; keep the 1x default. The editor
  // clamps to its supported range when it reads this.
  editorScale.store(static_cast<double>(state.getProperty("editorScale", 1.0)));
  // Default matches the UI's default-visible hint bar (see Processor.h).
  editorExtraHeight.store(static_cast<int>(state.getProperty("editorExtraHeight", 36)));

  // A missing child clears the map; a project without mappings must not
  // inherit the previous session's.
  midiMapper.restoreFromValueTree(midiState);

  // A project load is a reconciling restore: matching blocks keep their
  // loaded engines, everything else decodes its embedded model bytes and
  // loads in the background. No synchronous model prepare under the chain
  // lock.
  //
  // Hosts can re-set state mid-playback (DAW preset browsers), so mute-splice
  // the restore like any structural edit. The mute is held until the restored
  // chain's models settle (deferred release below); the first audible buffers
  // are the finished rig gliding in, never the raw dry input of still-loading
  // blocks.
  ChainEditFade editFade(*this);

  Lane retired;  // destroyed after the lock; see restoreChainSnapshot
  {
    juce::ScopedLock lock(chainMutex);

    // The session is replaced whole: what the legacy image controls were
    // before the outgoing rig became a grid is not theirs to return to.
    gridHeldLegacyParams.clear();
    retired = restoreChainSnapshot(snapshot);  // updates latency, bumps revision

    pendingAddSide = ChainSide::Left;
    activePresetId = state.getProperty("activePresetId").toString();
    activePresetName = state.getProperty("activePresetName").toString();
    // A project/state load replaces the whole session; undoing across it
    // would resurrect chains the user never saw in this session.
    chainHistory.clear();
  }
  // The session's rig is what "unsaved changes" is measured from now.
  markPresetBaseline();
  // And the session's setlist is the one its preset is in.
  holdRestoredPosition();

  editFade.releaseWhenChainLoadsSettle();
  // What the restore queued for the legacy image controls, off the lock
  // (see undoChain).
  flushLegacyParamWrites();

  // Tell the host the whole parameter set may have moved. CLAP requires an
  // explicit CLAP_PARAM_RESCAN_VALUES after a state load (clap-juce-extensions
  // maps programChanged to exactly that); VST3/AU hosts drive their own state
  // restores and treat this as a harmless values refresh.
  updateHostDisplay(ChangeDetails{}.withProgramChanged(true));

  DBG("Plugin state restored successfully");
}
