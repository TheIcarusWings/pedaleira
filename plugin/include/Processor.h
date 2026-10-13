#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/activations.h"
#include "NAM/convnet.h"
#include "NAM/lstm.h"
#include "NAM/util.h"
#include "NAM/wavenet/model.h"
#include "BlockKinds.h"
#include "ChainBlock.h"
#include "HostedPluginBlockProcessor.h"
#include "ChainBoundaryMulti.h"
#include "ChainDomain.h"
#include "ChainHistory.h"
#include "AutoOffset.h"
#include "ChainOversampler.h"
#include "GridTopology.h"
#include "ProcessingPlan.h"
#include "CaptureAudio.h"
#include "InputRecorder.h"
#include "RtWorkerPool.h"
#include "MidiMapper.h"
#include "NoiseGate.h"
#include "PitchShift.h"
#include "Spread.h"
#include "StereoOffset.h"
#include "PresetManager.h"
#include "SetlistLibrary.h"
#include "Scenes.h"
#include "TunerDetector.h"

class EngineCommands;

class TONE3000Processor;

class TONE3000Processor : public juce::AudioProcessor,
                          private juce::AudioProcessorValueTreeState::Listener,
                          private juce::AsyncUpdater,
                          private juce::Timer {
public:
  TONE3000Processor();
  ~TONE3000Processor() override;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  using AudioProcessor::processBlock;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override;

  const juce::String getName() const override;
  double getTailLengthSeconds() const override;

  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  // Host program API, backed by the internal preset list. Not cosmetic:
  // this is the only route a MIDI program change can take in a VST3 host
  // (see the HOST PROGRAM API section in ProcessorPresets.cpp).
  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int) override;
  const juce::String getProgramName(int) override;
  void changeProgramName(int, const juce::String&) override;

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

  juce::AudioProcessorValueTreeState parameters;

  juce::AudioProcessorValueTreeState& getParameters() { return parameters; }

  // MIDI CC/note → parameter mapping with Learn (see MidiMapper). Fed every
  // processBlock; serializes with plugin state so maps travel with DAW
  // sessions and the standalone's saved session alike.
  MidiMapper midiMapper{parameters};

  // Chain management methods
  // Load a tone into an insert slot. `targetInsertId` is the insert block the
  // user clicked (the UI remembers it across the tone-select flow); the new
  // tone block takes that slot's position. When the id is absent or
  // stale (undone away mid-flow), the active lane's first insert is used.
  std::string loadTone(const juce::String& toneJsonString,
                       const std::string& targetInsertId = {});
  // Load dropped local files (`files` = [{ name, data }], base64 bytes; one
  // entry for a single file, many for a folder) as one tone block, one model
  // per file. `targetInsertId` is either an insert slot (adds, consuming
  // that tile) or an existing tone block (swaps in place). Each file is
  // validated (.nam must be A2, .wav must be real audio) and stashed as a
  // content-addressed copy whose path becomes the model's model_url; then
  // the tone routes through loadTone / swapTone, so downstream behavior
  // (background load, cache, undo, duplication, persistence) is identical
  // to catalog tones. Unlike catalog tones the full model list stays in the
  // stored tone JSON (there is no API catalog to page the others back in
  // from). Returns { blockId } on success or { error } with a user-facing
  // message.
  juce::var loadLocalTone(const juce::String& title, const juce::var& files,
                          const std::string& targetInsertId = {});
  // Path-based sibling of loadLocalTone for files already on disk: the
  // UI's drops and Load File / Load Folder pickers (loadLocalTone's
  // byte-array form is what the DSP tests drive). A
  // directory loads as one multi-model tone by the same rules as a folder
  // drop in the UI: majority extension picks NAM vs IR, capped at 300
  // files / 50 MB each, models in natural name order, title from the
  // folder name. A single file must be .nam or .wav; title is the file
  // name. Same return contract as loadLocalTone.
  juce::var loadLocalTonePath(const juce::File& source, const std::string& targetInsertId = {});

  /** URL sibling of loadLocalTonePath, for the iOS document picker.
      Files chosen from the Files app live outside the app sandbox and are
      readable only through the security scope JUCE's FileChooser bookmarked,
      so the bytes have to come through juce::URL rather than the raw path.
      Takes 1..N URLs because multi-select stands in for the folder route on
      iOS (a security-scoped directory cannot be enumerated through
      juce::URL); see LocalFiles::pick. Same return contract as
      loadLocalTone. Compiled on every platform so the DSP suite can test it;
      only the iOS editor calls it. */
  juce::var loadLocalToneUrls(const juce::Array<juce::URL>& sources,
                              const std::string& targetInsertId = {});
  // Age out local-model stash files unused for a week (runs once per
  // process, off-thread). Called from the constructor.
  static void cleanLocalModelStash();
  // Sweep "*_ir.wav" files leaked into the OS temp dir by older builds (the
  // IR loader wrote one per engine build and never deleted it; it now cleans
  // up after itself). Runs once per process, off-thread. Called from the
  // constructor.
  static void cleanLeakedIrTempFiles();
  // The sweep itself: deletes "*_ir.wav" files in `tempDir` untouched for
  // over an hour (the age guard protects a concurrent instance's in-flight
  // file). Returns how many files it deleted.
  static int sweepLeakedIrTempFiles(const juce::File& tempDir);
  // Make `dir` a directory this process can write to, healing the one
  // app-data wound we can't chown away: the folder (or a file squatting on
  // its path) existing but not being writable. A sudo'd run of an older
  // script/install-plugin.sh (macOS sudo keeps $HOME) or a restored backup
  // leaves ~/Library/Application Support/TONE3000 root-owned; every settings
  // save and drop-stash write then fails while reads keep working (github
  // issue #76, every drop answering "Couldn't store the dropped file"). Two
  // unprivileged fixes cover it: a folder the user still owns gets its write
  // bits chmod'd back in place (contents stay put); anything else (root
  // owned, or a file squatting on the path) is renamed aside to a
  // "<name>.unwritable" sibling (kept for manual recovery, never deleted)
  // and recreated fresh, the parent folder being the user's own.
  // Returns true when `dir` is a writable directory on exit; failures log
  // their reason. No-op (two stats) on a healthy folder. Run once per
  // process on the app-data root (see the constructor) and defensively from
  // the stash and preset write paths.
  static bool ensureWritableDir(const juce::File& dir);
  // Resolve a persisted local-model `file://` URL to the stash file that
  // actually holds those bytes, given the current stash root. The URL stored
  // in a block's tone JSON is absolute, and that JSON is persisted in
  // presets, DAW/app state and undo snapshots; on iOS the app data
  // container's UUID rotates on every reinstall or app update, so every one
  // of those paths goes stale. Stash names are content hashes in a flat
  // folder, so the file name *is* the stable token: when the stored path no
  // longer exists, the same name under the current root is the same bytes.
  // A stored path that does exist is returned untouched, which is every
  // desktop case (the root never moves there). Empty File for a non-file URL.
  static juce::File resolveLocalModelFile(const juce::File& stashRoot,
                                          const juce::String& modelUrl);
  // The file name a URL names, percent-decoded. juce::URL::getFileName returns
  // the raw, still-escaped last path component, so a file picked as
  // "Deluxe Reverb 2.nam" reads back as "Deluxe%20Reverb%202.nam" and would
  // reach the title, the model name and the stash name in that form. Desktop
  // derives names from juce::File and never sees escapes; this keeps the URL
  // path identical.
  static juce::String localFileNameFromUrl(const juce::URL& url);
  // Replace the tone of an existing block in place. Keeps the block's chain
  // position and user params (enabled/gains/mix); the new tone's first model
  // is queued for background loading.
  bool swapTone(const std::string& blockId, const juce::String& toneJsonString);
  // Best-effort tone metadata re-sync: merge a fresh /tones/{id} API payload
  // into every non-local block holding that tone (both lanes). The stored
  // models array is preserved per block (native persists only the active
  // model, and its model_url backs retries/reloads); everything else (title,
  // artwork, counts, url, ...) is taken from the fresh payload. Purely
  // metadata: params, activeModelId and engines are untouched, and no undo
  // step is recorded. The revision only bumps when something actually
  // changed, so an identical payload is a true no-op. Returns whether any
  // block changed.
  bool refreshToneMetadata(const juce::String& toneJsonString);
  // Switch the block's active model. Native only stores the active model, so
  // `modelData` (JSON object with id/name/model_url, paged in from the API by
  // the UI) is required and becomes the tone's new sole stored model.
  bool switchModel(const std::string& blockId, int modelId, const juce::var& modelData);
  // Retry a failed model download (block.loadFailed): clears the flag and
  // re-queues the block's active model through the background loader. The
  // simplest recovery when tone3000.com was unreachable mid-load.
  bool retryModelLoad(const std::string& blockId);
  // Add a block of a registered kind (BlockKinds.h): the path for blocks
  // that have no model: the built-in effects. (A hosted plugin is added
  // with addPluginBlock, which also needs to know which plugin.)
  // `targetId` is addressed like a tone load's target: an insert slot (the
  // block takes its position), an existing tone block (replaced in place,
  // keeping its id, gains, mix and EQ), or empty for the active lane's first
  // insert. The processor is built and prepared on the calling thread before
  // the chain lock is taken, then installed through the same step a model
  // load uses, so a new block fades in from bypass and a replaced one mutes,
  // swaps and fades back. A kind that reports latency is spliced in under
  // the chain-edit fade instead: the lane's timing moves, which no single
  // block's fade can hide. Undoable. Returns { blockId } or { error }.
  // Message thread.
  juce::var addKindBlock(const juce::String& kindId, const std::string& targetId = {});
  // Add a block that hosts one of the user's plugins (kind "plugin", see
  // HostedPluginBlockProcessor.h). `pluginKey` names an effect the scanner
  // knows (PluginScanner::findEffect: an identifier from its list, a file
  // path or an exact name). `targetId` is an empty insert slot, or empty for
  // the active lane's first one; replacing a tone block in place is not
  // offered, since the plugin only exists some time after this returns.
  // The block is in the chain when this returns and shows as loading
  // (`modelLoading`); the instance is created on the message thread a
  // message-loop turn later and installed through the usual fade (a chain
  // mute when it has latency). A plugin that fails to come up leaves a
  // bypassed placeholder under its name, never an error that blocks the
  // rig. Undoable. Returns { blockId } or { error }. Message thread.
  juce::var addPluginBlock(const juce::String& pluginKey, const std::string& targetId = {});
  // Message-thread housekeeping for hosted plugins, cheap when there is
  // nothing to do: applies parameter sets made while no audio ran, re-reads
  // the state chunk of every plugin that reported a change (so the next
  // snapshot, taken under the chain lock, has it without calling the
  // plugin), re-prepares instances whose prepare arrived on another thread,
  // schedules the deferred refresh when a plugin changed its latency or
  // tail, and gives blocks waiting for a missing plugin another try after a
  // scan. Runs on a timer while the rig holds a hosted block, before every
  // engine command, and from flushDeferredBlockRefresh; a save calls it
  // with `forceStateRefresh` so plugins that never report changes are saved
  // as they are. Does nothing off the message thread. Never call it with
  // chainMutex held.
  // `throttled` is for callers that only look (the poll timer, a query, the
  // unsaved-changes check): a plugin that keeps flagging changes then has
  // its state re-read at most every kHostedStateCaptureIntervalMs. Callers
  // that are about to snapshot the rig (an edit's undo entry, a save) leave
  // it off and get a current cache.
  void serviceHostedPlugins(bool forceStateRefresh = false, bool throttled = false);
  // The running plugin instance of block `blockId`, or null when that block
  // is not a hosted plugin (or its plugin is still loading or missing). For
  // the plugin's own editor window, which has no command: the UI holds the
  // instance while the window is open and closes the window once the block
  // no longer runs it.
  std::shared_ptr<HostedPluginInstance> hostedPluginInstance(const std::string& blockId);
  bool removeChainBlock(const std::string& blockId);
  bool reorderChainBlocks(const std::vector<std::string>& newOrder);
  // Move a block into the other lane at the given index (stereo mode drag
  // across chains). Engines move with the block; insert slots can't move.
  bool moveBlockToChain(const std::string& blockId, const juce::String& side, int index);
  // Clone a tone block (model, EQ, gains, mix, every persisted setting)
  // into `side` at `index` (absolute lane index, inserts included). A clone
  // landing on an insert slot consumes it (paste into an empty tile);
  // anywhere else it splices in like a cross-lane drop. The clone gets a
  // fresh id and its own engines, loaded cache-first from the source's
  // in-memory model bytes (no network). Returns the new id, "" on failure.
  std::string duplicateChainBlock(const std::string& sourceBlockId, const juce::String& side,
                                  int index);
  // In-app block clipboard (Copy on a tone tile / Paste on an insert slot).
  // Copy stores a self-contained snapshot (the serializeBlockSettings tree +
  // the block's in-memory model bytes) on this instance, so paste keeps
  // working after the source block is gone: preset switches, undo, deleting
  // the copied block. Not undoable; bumps the revision so `canPasteBlock`
  // (getChainState) reaches the UI. Returns false for stale/insert ids.
  bool copyChainBlock(const std::string& blockId);
  // Rebuild a block from the clipboard snapshot into `side` at `index` (same
  // landing rules as duplicateChainBlock: an insert slot there is consumed,
  // anywhere else splices in). The paste gets a fresh id and loads its model
  // cache-first from the copied bytes. Returns the new id, "" on failure
  // (empty clipboard, or the right lane while mono).
  std::string pasteChainBlock(const juce::String& side, int index);

  // TONE3000 OAuth access token. Updated by the UI after the Select flow and
  // again on every refresh. `fetchModelFromUrl` attaches it as a Bearer header
  // because the new TONE3000 model_url endpoints reject anonymous requests.
  void setAccessToken(const juce::String& token);
  juce::String getAccessToken() const;

  // Background loading (called by thread pool jobs)
  void loadToneInBackground(const std::string& blockId, int firstModelId,
                            const juce::String& modelUrl, const juce::String& modelName,
                            ChainBlockType type);
  void switchModelInBackground(const std::string& blockId, int modelId,
                               const juce::String& modelUrl, const juce::String& modelName);

  // Chain state for the UI. `knownRevision` is the last revision the caller
  // saw (-1 for "give me everything"); when the chain hasn't changed since,
  // a minimal { revision, unchanged: true } object is returned so the UI's
  // poll loop stays cheap.
  juce::var getChainState(int knownRevision) const;
  // Current chain revision, promoting any settled continuous-gesture edit
  // into a real bump first. The UI's ChainStore polls this (an atomic read)
  // and refetches getChainState only when it moved.
  juce::uint32 getCurrentChainRevision() const;
  // Single entry point for all per-block user params. The common ones:
  // "enabled" (0/1), "normalize" (0/1), "inputGain", "outputGain", "mix"
  // (normalized 0..1). Any other id is looked up in the block's kind
  // parameter table and `value` is then in that parameter's real unit,
  // clamped to its range. Returns false for unknown blocks/params.
  // Continuous params defer their revision bump to the end of the gesture
  // (see deferredRevisionBump) so drag-rate calls never force full chain
  // resyncs; a kind parameter flagged affectsTail / affectsLatency also
  // schedules the deferred engine refresh (flushDeferredBlockRefresh).
  bool setBlockParam(const std::string& blockId, const juce::String& param, double value);

  // Takes a block out of (true) or back into (false) the audible rig
  // without touching its saved `enabled` switch or the undo history. (A
  // scene change does the same thing to the audio through `enabled` itself;
  // see selectScene.) A kind with
  // tail spillover then rings out instead of being cut (see
  // ChainBlock::released and BlockProcessor::supportsSpillover); any other
  // kind, or one whose spillover switch is off, fades to bypass exactly as
  // for a power-off. Runtime only: not saved, cleared on nothing but the
  // matching call. False for an unknown block or an insert slot.
  bool setBlockReleased(const std::string& blockId, bool released);
  bool isBlockReleased(const std::string& blockId) const;

  // ---- Scenes (ProcessorScenes.cpp; the data is described in Scenes.h) ----
  // Eight scenes per rig, A to H (index 0 to 7). The blocks always carry the
  // active scene, so every existing edit path is a scene edit already:
  // setBlockParam("enabled") and a set of a scene-assigned parameter write
  // the active scene only, a set of any other parameter is heard in every
  // scene.
  //
  // Selecting a scene copies its block states and assigned values onto the
  // blocks under one short hold of the chain lock, and that is all: the
  // audio thread glides from there with the per-block fades it has always
  // had (a block the scene turns off fades to bypass, or rings out when its
  // kind has spillover and the switch is on; gains and mix ride their
  // smoothers; kind parameters are the lock-free stores a knob move makes).
  // Nothing is rebuilt and the chain-edit fade is never taken, so it is safe
  // at footswitch speed. Not an undo step, and never dirties the history.
  // Message thread, like every chain edit; other threads go through
  // footswitchPress or the MIDI mapper, which hand over to it.
  // False for a scene that is not in use (see getSceneCount).
  bool selectScene(int index);
  int getActiveScene() const;
  // How many scenes the rig uses, 1 to 8 (a new rig has one). Scenes at or
  // past it are not in use: selecting, renaming, copying, resetting or
  // setting a value in one is refused, and no footswitch reaches it.
  int getSceneCount() const;
  // Adds the next scene as a copy of the active scene's block states and
  // assigned values, with its default name. The active scene stays. Fails
  // at eight; `newIndex` is the added scene. Undoable.
  juce::Result addScene(int* newIndex = nullptr);
  // Removes a scene in use (not the only one). Later scenes move down one
  // place with their names, states and values; the last place is reset. The
  // active scene follows its own content, or the scene that takes the place
  // of a removed one. Undoable.
  juce::Result removeScene(int index);
  // { active, activeId, count, scenes: [{ index, id, name, active, inUse, blocks: { <block
  //   id>: on }, values: [{ blockId, param, value }] }], assigned: [{
  //   blockId, param }], expression: [{ blockId, param }] }. `blocks` names
  // every tone block, also those whose state no scene has changed yet.
  juce::var getScenesState() const;
  // Empty (or blank) restores the default name. Undoable.
  bool renameScene(int index, const juce::String& name);
  // Block states and assigned values of `from` onto `to`; `to` keeps its
  // name. Copying onto the active scene applies it like a scene switch.
  // Undoable.
  bool copyScene(int from, int to);
  // Back to what a scene is before anyone edits it: default name, every
  // block on, each assigned parameter at the value the active scene has
  // (resetting the active scene keeps its values). Undoable.
  bool resetScene(int index);
  // Give a parameter a value per scene: inputGain, outputGain, mix or a
  // parameter of the block's kind (a hosted plugin's included). Its current
  // value is copied to all eight scenes. Refused for `enabled` (always per
  // scene), `normalize`, a parameter whose change moves the block's latency
  // (that takes a chain mute, which a scene switch never does) and a
  // parameter flagged for the expression pedal. Undoable.
  juce::Result assignSceneParam(const std::string& blockId, const juce::String& param);
  // Back to one shared value: the one the active scene has. Undoable.
  juce::Result unassignSceneParam(const std::string& blockId, const juce::String& param);
  // Set an assigned parameter's value in one scene without selecting it
  // (the active scene's is set on the block, like setBlockParam). Clamped to
  // the parameter's range. Undoable.
  juce::Result setSceneParamValue(int scene, const std::string& blockId,
                                  const juce::String& param, double value);
  // The hook for an expression pedal: flag a parameter as driven by it.
  // Flagging takes the parameter out of the scenes (the active scene's value
  // stays as the shared one) and keeps it out; the pedal itself does not
  // exist yet. Undoable.
  juce::Result setParamExpressionAssigned(const std::string& blockId, const juce::String& param,
                                          bool assigned);

  // ---- Stomps -------------------------------------------------------------
  // One assignment per footswitch A to H (0 to 7), part of the rig: saved
  // with it, in presets, and covered by undo. A press flips each assigned
  // block (so a switch can swap one block for another); with `momentary`
  // the release flips them back.
  juce::Result assignStomp(int footswitch, const std::vector<std::string>& blockIds);
  // One block off the switch, or with an empty id the whole assignment
  // (label and momentary included).
  juce::Result unassignStomp(int footswitch, const std::string& blockId = {});
  // Null pointers leave that option alone.
  juce::Result setStompOptions(int footswitch, const juce::String* label, const bool* momentary);
  // { stomps: [{ switch, index, blockIds, label, momentary }] }, all eight.
  juce::var getStompsState() const;

  // ---- Footswitches -------------------------------------------------------
  // Eight virtual switches A to H plus Bank Up, Bank Down and Tempo (see
  // the kFootswitch constants in Scenes.h). What A to H do follows the
  // footswitch mode:
  //  - Preset: load preset `bank x 8 + switch` of the preset list; Bank
  //    Up / Down move by one bank of eight, wrapping.
  //  - Scene: select scene A to H.
  //  - Stomp: flip the switch's assigned blocks.
  //  - Hybrid: the top row (A to D) and the bottom row (E to H) each do one
  //    of the three. A stomp row uses its own switches' assignments. Scene
  //    rows: with both rows on scenes the top is A to D and the bottom E to
  //    H; a single scene row shows A to D and Bank Up / Down flip it to E to
  //    H and back. Preset rows: four presets per row, the bank being as
  //    many presets as the preset rows show (four or eight).
  // In Scene and Stomp mode, where there is nothing to page, Bank Up / Down
  // step to the next / previous preset. Tempo always taps.
  //
  // Press and release may come from any thread: on the message thread they
  // act at once, anywhere else (the audio thread included) they are queued
  // without locking or allocating and delivered there.
  void footswitchPress(int footswitch);
  void footswitchRelease(int footswitch);
  FootswitchMode getFootswitchMode() const {
    return static_cast<FootswitchMode>(footswitchMode.load());
  }
  // The mode and the hybrid rows are machine-wide settings, kept in the
  // shared settings file like multi-core. `persist` = false skips the write.
  void setFootswitchMode(FootswitchMode mode, bool persist = true);
  void setFootswitchHybridRows(FootswitchRowRole top, FootswitchRowRole bottom,
                               bool persist = true);
  // Everything a pedalboard view needs to draw the eleven switches: { mode,
  // hybridRows: { top, bottom }, bank, bankCount, scenePage, activeScene,
  // bpm, switches: [{ id, index, role, label, colour, lit, held, ... }] }.
  // `role` is preset, scene, stomp, bankUp, bankDown, presetNext,
  // presetPrevious or tapTempo; `empty` marks a switch with nothing behind
  // it (a stomp without blocks, a preset slot past the end of the list);
  // `colour` is a hint: the category of a stomp's first block, else the
  // role.
  juce::var getFootswitchState() const;
  // One tap of the tempo switch at `timeMs` (milliseconds on any clock that
  // moves forward; negative = now). Sets the rig's tempo once the taps give
  // one (see TapTempo) and returns whether this tap did. Message thread.
  bool tapTempo(double timeMs = -1.0);
  // Tests and the headless tools: keep the footswitch settings in `file`
  // instead of the user's settings file, which is then neither read nor
  // written for them. Process-wide; set it before constructing a processor
  // (the constructor reads the mode). An empty File restores the default.
  // Returns the previous override.
  static juce::File setFootswitchSettingsFileForTesting(const juce::File& file);

  // One list of everything settable on a block: the common parameters, then
  // its kind's, each as its BlockParamSpec::toVar plus { scope: "common" |
  // "kind", value }. A void var for an unknown id or an insert slot.
  juce::var getBlockParams(const std::string& blockId) const;

  // What a parameter move leaves for later: the reported tail and, for a
  // kind whose latency follows a parameter, the latency itself (the kind
  // keeps its latched latency until told, see
  // BlockProcessor::commitLatency). Both happen once the gesture has been
  // quiet for kGestureSettleMs, on a message-thread timer. This does it
  // now. A caller WITHOUT a running message loop never gets the timer and
  // has to call this (or state.waitUntilLoaded, which does) before it
  // renders or reads the latency: the headless render tool does. A latency
  // change mutes the chain, commits every pending kind, resizes the
  // compensation and reports the new latency, in that order. Message
  // thread; no-op when nothing is pending.
  void flushDeferredBlockRefresh();

  // The rig's own tempo, used by tempo-synced kinds whenever the host
  // reports none (standalone, or a host without a tempo). Clamped to
  // kMinTempoBpm..kMaxTempoBpm. Not saved with the state: it is a stand-in
  // for the host's, and a project's tempo belongs to the host.
  static constexpr double kMinTempoBpm = 20.0;
  static constexpr double kMaxTempoBpm = 300.0;
  static constexpr double kDefaultTempoBpm = 120.0;
  void setTempo(double bpm);
  double getTempo() const { return internalTempoBpm.load(); }
  // What the last audio callback ran with: the host's tempo when it gave
  // one, else the rig's own.
  double getEffectiveTempo() const { return lastEffectiveBpm.load(); }
  bool isTempoFromHost() const { return lastTempoFromHost.load(); }

  // Per-block NAM A2 size, in NAM's slimmable-size domain (0..1, clamped;
  // 0.0 = lite, 1.0 = full; see NamBlockProcessor::slimSize). Chain state like
  // mix or the active model, so presets/undo/duplication carry each block's
  // size. Not routed through setBlockParam because it retiers the block's
  // loaded engine in place: weight swaps are discontinuous and the retier
  // can hold chainMutex through a submodel prewarm, so it mute-splices the
  // chain like a structural edit (processBlock's try-lock keeps the audio
  // thread off the lock while the fade holds). Undoable; no-op sets never
  // dip the audio.
  bool setBlockSlimSize(const std::string& blockId, double slimSize);

  // Default NAM A2 size for newly added blocks (machine-wide user setting,
  // like multi-core), in the same slimmable-size domain. Existing blocks
  // keep their own per-block size; this only stamps blocks as loadTone
  // creates them, so it persists in the shared settings file rather than
  // sessions or presets.
  double getNamSlimSizeDefault() const { return namSlimSizeDefault.load(); }
  void setNamSlimSizeDefault(double slimSize);

  // Tests: callbacks whose plan did not match the lanes it was about to
  // run (see topologyRevision). Zero unless a structural edit skipped
  // rebuildTopology.
  int stalePlanCallbacksForTesting() const { return stalePlanCallbacks.load(); }
  // Tests: the active plan as text (describePlan), and whether it holds
  // against the rig it was compiled from (validatePlan; empty = it does).
  std::string describeActivePlanForTesting() const;
  std::string validateActivePlanForTesting() const;

  // ---- Grid mode (ProcessorGrid.cpp) ------------------
  // The rig as a 4 x 8 grid: every row has its own input (a device channel
  // or pair, or none when a split or other rows feed it), its own output (a
  // device pair or another row) and a format, and holds exactly eight
  // slots. Entered by convertRouting(true), by any of the row
  // mutators below (which convert first, as one undo step with the edit) or
  // by loading a grid state or preset. All of it is message-thread API.
  bool isGridMode() const noexcept { return gridModeActive.load(std::memory_order_relaxed); }
  // Legacy to grid or back, one undoable step. Returns { ok, mode, lossy:
  // [what the grid cannot carry over], reasons: [why it cannot convert] }.
  // A rig that is already in the mode asked for answers ok without an undo
  // step. To grid: a branch becomes a split of row 0 into row 1 that never
  // rejoins, with the trunk as row 0 (a trunk on the right swaps the lanes,
  // which take their pans along); a lane with more than eight blocks is
  // refused, as is a branch lane that does not fit behind the tap. To
  // legacy: the grid has to be one of the shapes the legacy rig can express
  // (see legacyProjectionBlockers).
  juce::var convertRouting(bool toGrid);
  // Why the grid cannot be written as a legacy rig, one line per reason;
  // empty when it can (or when the rig is legacy already).
  juce::StringArray legacyProjectionBlockers() const;
  // { mode, legacyProjectable, rows: [{ index, format, runsAs, source, sink,
  //   input, output, slots, active, activeSlots, routed, latencySamples,
  //   padSamples, cpu, inputMissing, outputFallback }], splits: [{ id, row,
  //   lowerRow, splitAt, mixAt, type, active, lowerActive, params,
  //   alignment, meters }], feeds: [{ from, to, active, padSamples }],
  //   groups, legacyControls, latencySamples, cpu: { total, critical }, io:
  //   { inputs, outputs } }. In legacy mode `rows` is empty.
  juce::var getGridState() const;
  // `channel` and `pair` are 0-based here (the commands take them 1-based).
  juce::Result setRowInput(int row, RowInput::Type type, int channel, bool stereo,
                           RowInput::Fold fold);
  // `toRow` is the row to feed for RowOutput::Type::Row: its output is then
  // summed into that row's input (the rows that feed one row are kept in
  // step). The target has no input of its own and is not the lower row of a
  // split, and the feeds form no loop; otherwise the edit is refused with
  // the reason and nothing changes.
  juce::Result setRowOutput(int row, RowOutput::Type type, int pair, int toRow = -1);

  // ---- Splits ----------------------
  // A split forks `row` into the row directly below it at the boundary
  // `splitCol` (0 to 8: before slot splitCol) and brings the two paths back
  // into `row` at `mixCol`, or never when that is -1: the lower row then runs
  // to its own output or feed. One split per row; the lower row must have no
  // input of its own; a split of a lower row has to lie inside the split
  // above it (that is the three-way nest). An edit the rules forbid is
  // refused with the reason and leaves the rig and its plan as they were.
  // All of these are structural edits under the chain-edit fade, except
  // setSplitParam, which is a lock-free store the junction ramps to.
  // Adding a split gives the lower row the upper row's format; change it
  // afterwards with setRowFormat (a dual-mono upper row always makes the
  // lower one dual mono).
  juce::Result addSplit(int row, int splitCol, int mixCol, SplitterType type);
  // Absent fields stay; mixCol -1 removes the rejoin.
  juce::Result moveSplit(int row, std::optional<int> splitCol, std::optional<int> mixCol);
  juce::Result removeSplit(int row);
  juce::Result setSplitType(int row, SplitterType type);
  // `param` is one of SplitJunction::parameterSpecs' ids ("split.balance",
  // "mix.levelB" ...), `value` in its real unit, clamped to its range.
  juce::Result setSplitParam(int row, const juce::String& param, double value);
  // { id, row, type, params: [spec + value] }; void when the row has no split.
  juce::var getSplitParams(int row) const;
  // Mono, stereo or dual mono. A dual-mono row carries two voices in every
  // NAM engine, so its amps rebuild (the chain stays muted until they have).
  juce::Result setRowFormat(int row, RowFormat format);
  // Volume (dB, kRowVolumeMinDb and below is off), pan (0 left to 1 right:
  // constant power on a mono row, a balance on a stereo or dual-mono one),
  // mute, solo and polarity. Smoothed on the audio thread: no recompile, no
  // fade. Absent fields are left alone.
  struct RowMixEdit {
    std::optional<float> volumeDb, pan;
    std::optional<bool> mute, solo, invert;
  };
  juce::Result setRowMix(int row, const RowMixEdit& edit);
  // The row's share of the input stage: its trim on top of the rig's input
  // level, whether its strip runs the noise gate, and whether it asks for
  // the pitch shifter (one strip runs it; the others are padded).
  struct RowInputStageEdit {
    std::optional<float> trimDb;
    std::optional<bool> gate, pitch;
  };
  juce::Result setRowInputStage(int row, const RowInputStageEdit& edit);
  // Move a block to (row, slot). Swap exchanges it with whatever sits there
  // (a block or an empty slot); insert makes room by pushing the blocks from
  // `slot` on toward the nearest empty slot of that row, and fails when the
  // row is full. A NAM block that lands in a row of another voice count is
  // rebuilt for it.
  juce::Result moveGridBlock(const std::string& blockId, int row, int slot, bool insertMode);
  // The id of whatever sits at (row, slot), empty slots included; "" in
  // legacy mode or out of range.
  std::string gridSlotBlockId(int row, int slot) const;
  // duplicateChainBlock by row number (any of the four in grid mode).
  std::string duplicateChainBlockToRow(const std::string& sourceBlockId, int row, int index);
  // Tests: how often the chain stage met a plan buffer it could not resolve
  // (a row or bus that does not exist, a block larger than the buffers).
  int unresolvedPlanRefsForTesting() const { return unresolvedPlanRefs.load(); }
  // Tests: whether the resampling boundary is open for exactly the strips
  // and buses of the active plan (true as well when the host rate needs no
  // boundary).
  bool boundaryMatchesPlanForTesting() const;
  // Tests: force the cross-bus latency padding on (1) or off (0) whatever
  // the wrapper type; -1 restores the default (on in every plugin format,
  // off in the standalone app and outside a wrapper). Recompiles the plan.
  void setGridCrossBusPaddingForTesting(int mode);
  // The plan CPU value, from the blocks'
  // current costs: { total, critical, rows: [..] } as fractions of one core.
  juce::var getPlanCpu() const;
  // Tests: one block's cost estimate; -1 for an unknown id.
  double blockCpuCostForTesting(const std::string& blockId) const;

  // Which device channel each channel of the processBlock buffer carries.
  // The standalone app opens a subset of
  // the device's channels and hands them over packed, so buffer channel 2
  // can be the interface's input 5. A plan's strips and buses name physical
  // channels and find their buffer channel through this map. Recorded by
  // prepareToPlay: from the audio device where there is one (the standalone
  // app), else buffer channel n is channel n of the main bus (a plugin host,
  // the tests, the render tool).
  struct DeviceChannelMap {
    // Physical channel (0-based) per buffer channel; -1 past the count.
    std::array<int, kMaxIoChannels> inputPhysical{{-1, -1, -1, -1, -1, -1, -1, -1}};
    std::array<int, kMaxIoChannels> outputPhysical{{-1, -1, -1, -1, -1, -1, -1, -1}};
    int numInputs = 0;
    int numOutputs = 0;
    // The device's own channel names, by buffer channel. Not for the audio
    // thread.
    juce::StringArray inputNames, outputNames;
    // True when the map was read from an audio device.
    bool fromDevice = false;

    // The buffer channel that carries a physical channel, or -1 when the
    // device does not have it or it is not open.
    int inputBufferChannel(int physical) const noexcept {
      for (int i = 0; i < numInputs; ++i)
        if (inputPhysical[static_cast<size_t>(i)] == physical)
          return i;
      return -1;
    }
    int outputBufferChannel(int physical) const noexcept {
      for (int i = 0; i < numOutputs; ++i)
        if (outputPhysical[static_cast<size_t>(i)] == physical)
          return i;
      return -1;
    }
  };
  // The map of the last prepareToPlay, copied under the chain lock. Any
  // thread but the audio thread.
  DeviceChannelMap getDeviceChannelMap() const;

  // The main-bus rule behind isBusesLayoutSupported, per wrapper type: the
  // standalone app, and a processor made outside any plugin wrapper (the
  // tests, the render tool), take 1 to 8 inputs and 1 to 8 outputs,
  // independently; every plugin format takes mono or stereo with as many
  // inputs as outputs.
  static bool isMainBusLayoutSupported(WrapperType wrapper, const BusesLayout& layouts);

  /** True while a chain-edit fade session holds the chain muted (including
      the deferred hold that waits out a restore's background loads). Lets
      tests (which pump audio far faster than the wall clock the release
      waiter runs on) block until the rig is actually audible again. */
  bool isChainEditFadeHeld() const { return chainEditFadePending.load(); }

  // Multi-core processing (machine-wide user setting).
  // When on, the chain stage spreads its independent work across the
  // RtWorkerPool realtime threads: in stereo mode the two lanes run
  // concurrently (the Right/branch lane on a worker, the other on the audio
  // thread), and with oversampling active each NAM block's phase instances
  // run concurrently too (see NamEngine). Off = everything runs sequentially
  // on the audio thread as before. Output is bit-identical either way; the
  // toggle trades single-core headroom for several busy cores, so it
  // persists per machine, never with presets/sessions.
  bool getMultiCoreEnabled() const { return multiCoreEnabled.load(); }
  // `persist` = false skips the settings-file write (tests toggling the
  // scheduling path must not touch the user's machine-wide preference).
  void setMultiCoreEnabled(bool enabled, bool persist = true);

  // Machine-wide defaults for the Plugin Settings parameters that describe
  // the workstation rather than the tone: input calibration (on/off, dBu
  // level) and oversampling (on/off, factor). They stay host parameters, so
  // a project reopens exactly as it was saved and automation keeps working;
  // the shared settings file only seeds a fresh instance (in the
  // constructor, before any host restore, which then wins) and is written
  // when the user changes one of them in Plugin Settings, never from a
  // restore or automation (github issue #66: a new DAW instance came up at
  // the parameter defaults, while the standalone remembered them).
  static bool isMachineDefaultParameter(const juce::String& paramId);
  // Store `paramId`'s current value as the machine default. Ignores ids
  // outside the set above.
  void persistParameterAsMachineDefault(const juce::String& paramId);
  // The two halves over an explicit PropertySet (the settings file in the
  // plugin; a scratch set in tests). Values are stored denormalised (dBu,
  // choice index, 0/1), like preset files, so a range retune can't move them.
  void writeMachineDefaultParameter(juce::PropertySet& settings, const juce::String& paramId) const;
  void applyMachineDefaultParameters(const juce::PropertySet& settings);
  // Tests: construction seeds from the user's real settings file, which
  // would leak a developer's oversampling choice into every DSP test, so the
  // test binary turns the seeding off process-wide and drives the two halves
  // above directly.
  static void disableMachineDefaultParametersForTesting();
  // Tests / offline renders: the Spread and Align wobble noise is clock-seeded,
  // so a render of a rig with wobble on differs every run. This seeds it with
  // `seed` at every later prepare (process-wide), making the render repeatable;
  // the clear call restores the random default. Call before prepareToPlay.
  static void setDeterministicRandomForOfflineRender(std::int64_t seed);
  static void clearDeterministicRandomForOfflineRender();
  // Tests / offline renders: point the local-model stash (drop-loaded files are
  // copied into <user app data>/<app>/LocalModels) at `dir`, so a run never
  // writes into the user's real folder. An empty File restores the default.
  // Process-wide; set it before constructing the first processor, whose
  // constructor starts the stash GC. Returns the previous override so a test
  // can scope it.
  static juce::File setLocalModelsDirForTesting(const juce::File& dir);
  // The stash folder itself (the override when one is set).
  static juce::File getLocalModelsDir();

  // Records the raw input for a guitar profile take (see InputRecorder.h).
  // processBlock feeds it; idle, it costs one atomic read per callback.
  InputRecorder inputRecorder;

  // The audio half of a capture run: while one is active the rig is muted and
  // this plays the test signal and records the return (see CaptureAudio.h).
  // processBlock holds a scope over it; idle, that costs one atomic read.
  CaptureAudio captureAudio;

  // The command layer over this processor (docs/commands.md) with the agent
  // layer on top (AgentHub.h), made on first use and destroyed with the
  // processor. The standalone application gets it at start, which is where
  // the command server comes up when the user has switched it on (see
  // createPluginFilter); tests and tools make their own EngineCommands.
  // Message thread.
  EngineCommands& commandLayer();

  // All meter levels in one call: { input, output, blocks: { id: { in, out } },
  // cpu (0..1 audio-callback load), correlation (-1..1 stereo-image output
  // correlation, from whichever engine the mode runs) }. Levels in dB with a
  // -60 floor. Designed to be polled once per UI frame.
  juce::var getMeterLevels() const;

  // Per-block EQ (wet-only post-model by default, pre-model when its pre flag is on).
  // setBlockEqBand takes { type, freqHz, gainDb, q } for one band, the undo
  // stack's mutation granularity. Band drags defer their revision bump like
  // continuous block params (see deferredRevisionBump).
  bool setBlockEqBand(const std::string& blockId, int bandIndex, const juce::var& bandVar);
  bool setBlockEqEnabled(const std::string& blockId, bool enabled);
  bool setBlockEqPre(const std::string& blockId, bool pre);
  bool resetBlockEq(const std::string& blockId);

  // Per-block spectrum for the EQ editor backdrop. The UI enables a block's
  // analyzer while its EQ view is open and polls getBlockSpectrum (~30 Hz);
  // when disabled the audio thread does no analyzer work for that block.
  bool setBlockSpectrumEnabled(const std::string& blockId, bool enabled);
  juce::var getBlockSpectrum(const std::string& blockId);
  // Editor teardown: one call instead of one per open EQ view.
  void disableAllBlockSpectrums();

  // Stereo mode: two independent Left/Right chains.
  void setStereoMode(bool enabled);
  bool isStereoMode() const { return stereoEnabled.load(); }

  // Chain branching (stereo mode).
  // A single optional tap point: the *branch* lane takes its input from the
  // *trunk* lane's signal after one of the trunk's tone blocks, instead of
  // from its own channel input. Only the branch lane's input source changes
  // (its blocks are untouched), so reverting to independent chains is trivial.
  // `side` names the trunk lane; `afterBlockId` the tone block whose output
  // is tapped. Calling it while already branched re-points the tap (either
  // lane, one move, no clearing first). Undoable; cleared automatically
  // when the tapped block leaves the trunk lane. Turning stereo mode off
  // only makes the branch dormant; it re-engages when stereo comes back
  // (like the right lane itself).
  bool setChainBranch(const juce::String& side, const std::string& afterBlockId);
  bool clearChainBranch();

  // How a stereo source feeds the chain(s). Interfaces usually expose stereo
  // pairs (line 1+2) even when only one jack is plugged in, and a DAW's
  // stereo track may carry one guitar or two, so the user picks:
  //  - Stereo (default): the natural routing for the chain mode. Mono chain:
  //    the source is summed to mono, ½(L+R), the same fold a host applies
  //    (a mono source on a stereo track, L == R, passes bit-identically).
  //    Stereo chains: channel 0 feeds the Left chain, channel 1 the Right.
  //  - DualMono: a mono chain runs each channel separately through its own
  //    copy of the chain, as if the same blocks sat in both stereo lanes
  //    (double-tracked guitars through one rig). Doubles the NAM work; with
  //    multi-core the two voices run on separate cores (see NamEngine.h).
  //    Only in effect with a mono chain on a rig that both supplies and
  //    reproduces two channels (dualMonoEngaged); otherwise it behaves as
  //    Stereo, so stereo chains see no difference between the two.
  //  - Left / Right: one channel folded onto both.
  // Set from the faceplate input-mode button (visible only when the source
  // is actually stereo; see stereoInputDetected). Saved with the
  // plugin/session state but not with presets: it's I/O routing, not tone.
  enum class InputMode { Stereo = 0, Left = 1, Right = 2, DualMono = 3 };
  void setInputMode(InputMode mode);
  InputMode getInputMode() const { return static_cast<InputMode>(inputMode.load()); }
  // Unknown strings (including "dual" read by an older build, which
  // doesn't reach here) fall back to Stereo.
  static InputMode inputModeFromString(const juce::String& s) {
    if (s == "left") return InputMode::Left;
    if (s == "right") return InputMode::Right;
    if (s == "dual") return InputMode::DualMono;
    return InputMode::Stereo;
  }
  static juce::String inputModeToString(InputMode mode) {
    switch (mode) {
      case InputMode::Left: return "left";
      case InputMode::Right: return "right";
      case InputMode::DualMono: return "dual";
      case InputMode::Stereo: break;
    }
    return "stereo";
  }
  // Modes that keep both source channels distinct (no fold onto one
  // channel). A branched chain has a single mono source, so these are the
  // modes it rejects (see setChainBranch).
  static bool isStereoFeed(InputMode mode) {
    return mode == InputMode::Stereo || mode == InputMode::DualMono;
  }

  // True while dual mono is actually in effect: the mode is selected, the
  // chain is mono, and the rig both supplies and reproduces two channels.
  // Atomics only, so the audio thread and getChainState can both ask.
  bool dualMonoEngaged() const noexcept {
    // A legacy notion: in grid mode a row's format says it.
    return !gridModeActive.load(std::memory_order_relaxed) &&
           getInputMode() == InputMode::DualMono &&
           !stereoEnabled.load(std::memory_order_relaxed) &&
           stereoInputDetected.load(std::memory_order_relaxed) &&
           stereoOutputDetected.load(std::memory_order_relaxed);
  }
  // Diagnostics (the DSP tests pin the voice-count transitions): the voice
  // count of a *loaded* NAM block's engine, 1 or 2 (see NamEngine.h); 0 for
  // an unknown id, a non-NAM block, or a block still loading.
  int namEngineVoiceCount(const std::string& blockId) const;
  // What one block's engine reports through the BlockProcessor interface:
  // the delay it adds to its wet path in chain-rate samples (zero for the
  // model-based kinds; part of the plugin's reported latency, see
  // refreshLatencyCompensation) and how long it rings after its input stops. -1 for an unknown id or an insert
  // placeholder, which has no engine.
  int blockLatencySamples(const std::string& blockId) const;
  double blockTailSeconds(const std::string& blockId) const;

  // Editor window scale, 1.0 = the 1024x578 design size. Written by the
  // editor whenever it is resized and read back when a new editor opens, so
  // the chosen size survives editor teardown. Saved with the plugin/session
  // state but deliberately not with presets: window size is a workstation
  // preference, not tone. Atomic: hosts may call get/setStateInformation off
  // the message thread.
  std::atomic<double> editorScale{1.0};

  // Extra window height (design px) for persistent chrome strips, currently
  // just the hint bar. Persisted like editorScale so the next editor opens at
  // the height the UI's first paint will actually need, instead of growing a
  // beat after launch. Dynamic strips (the standalone banner) are excluded.
  // Default 36 = the hint bar, which the UI shows by default: a fresh insert
  // then opens at its final height and never asks the host to resize. Some
  // hosts (LUNA) grant a post-open grow without re-laying-out their own
  // plugin-window chrome, which visibly misplaces it.
  std::atomic<int> editorExtraHeight{36};

  // Which lane loadTone falls back to ("left"/"right") when no valid target
  // insert id is supplied. The UI sets it when an Add starts, so a tone
  // picked later lands in the lane the user was working in. Not a view mode
  // and not part of undo history.
  void setActiveEditChain(const juce::String& side);
  // Swap the Left and Right chains wholesale (stereo mode only). Undoable.
  bool swapChains();

  // Undo/redo over chain edits (structure, tones, params, EQ, stereo mode).
  // Snapshot-based: every mutator captures the pre-mutation chain settings
  // (see ChainHistory); undo/redo restore by reconciling against the live
  // chains so loaded engines are reused whenever the tone/model still match.
  // Both return false when there is nothing to undo/redo.
  bool undoChain();
  bool redoChain();

  // Internal presets (ProcessorPresets.cpp).
  // A preset = chain snapshot (with its model bytes, so it loads offline) +
  // the faceplate parameter values (kPresetParameterIds). The active preset
  // { id, name } rides getChainState; it only ever changes together with a
  // revision bump.
  //
  // The preset list is the current setlist of the library (below), in slot
  // order: these calls mean "in the current setlist". A preset's place in
  // the list is its slot, so program change n, footswitch n and
  // loadPresetAtIndex(n) all reach slot n + 1, and an empty slot does
  // nothing. The factory presets are a read-only setlist of their own.
  // { presets: [{ id, name, factory, slot }], setlist: { id, name, readOnly } }
  juce::var getPresetList() const;
  // Saves into the current setlist: over the preset of that name, else into
  // the first empty slot. With the factory setlist current it goes to the
  // first setlist of the user's own (made on the spot when there is none),
  // which becomes the current one. { id, name, setlist, slot } or a void var
  // (then `whyNot`, when given, says why).
  juce::var savePreset(const juce::String& name, juce::String* whyNot = nullptr);
  bool loadPreset(const juce::String& presetId);   // undoable (chain part)
  // Whether the id is a preset of any setlist, the factory one included.
  bool presetExists(const juce::String& presetId) const;
  // The rig as a preset tree (the chain snapshot with its models' bytes and
  // the preset-managed parameters) without touching the library, and the
  // way back: applyRigTree replaces the rig exactly as loadPreset does
  // (mute-spliced, one undoable step for the chain, the tree's tempo when it
  // names one) and names the active preset as given (empty: none). Nothing
  // of the library is read or written: no recents, no position. `unsaved`
  // says the tree is not what that preset's file holds (the rig it came
  // from had been edited since), so hasUnsavedChanges answers true until
  // the next save or load. Together they carry a rig from one processor to
  // another (see SandboxRig). Message thread.
  juce::ValueTree captureRigTree();
  bool applyRigTree(const juce::ValueTree& rig, const juce::String& presetId = {},
                    const juce::String& presetName = {}, bool unsaved = false);
  // Makes this processor a guest of `other`'s library (a sandbox rig uses
  // the library of the rig it was made from, test stores included): the
  // same factory presets, setlists and asset store, through a library
  // object of its own, so it has its own position and never moves the
  // other's program numbers. A guest only ever adds to the library:
  //   - its position and its loads stay in memory (library.json keeps the
  //     player's position and recents),
  //   - a save goes into an empty slot and never replaces a preset,
  //   - an export never replaces a file.
  // Everything else that writes the library is not in a sandbox's command
  // set at all (EngineCommands::Scope). Message thread, before any preset
  // call.
  void sharePresetStoreWith(const TONE3000Processor& other);
  bool isLibraryGuest() const { return libraryGuest; }
  bool renamePreset(const juce::String& presetId, const juce::String& newName);
  // Moves the preset's file to the trash.
  bool deletePreset(const juce::String& presetId);
  // Move a preset by `delta` places among the presets of the current
  // setlist (negative = earlier); the occupied slots keep their numbers and
  // the files are renumbered. prev/next stepping and MIDI program-change
  // numbers follow (see loadPresetAtIndex).
  bool movePreset(const juce::String& presetId, int delta);
  // Back to the factory-default state: empty mono chain, every preset-managed
  // faceplate parameter at its default, no active preset. One undoable step,
  // mute-spliced like a preset load. Returns false (leaving the audio
  // untouched) when the state is already at default.
  bool resetToDefault();

  // The folder of the old .t3kpreset store (see importLegacyPresets).
  juce::File getUserPresetsDir() const { return presetManager.userPresetsDir(); }

  // Re-root the preset stores at an explicit directory (tests use a temp
  // dir so preset/program behavior can be driven without touching the
  // user's folders): the old .t3kpreset store and its Factory folder at
  // `baseDir` (see PresetManager's baseDir constructor), and this
  // instance's library at `libraryRoot`, or `baseDir`/Library without one.
  // Message thread only, before any preset call.
  void setPresetStoreForTesting(const juce::File& baseDir, const juce::File& libraryRoot = {}) {
    presetManager = PresetManager(baseDir);
    library = std::make_unique<SetlistLibrary>(
        libraryRoot != juce::File() ? libraryRoot : baseDir.getChildFile("Library"));
    hostProgramInfoCache.clear();
    // A store named outright is one whose presets may be brought over (see
    // ensureLegacyMigration), and this library has not been looked at yet.
    legacyMigrationAllowed = true;
    legacyMigrationStarted = false;
    legacyMigrationDone.store(false);
  }
  // True when nothing in this instance holds the chain lock: what a test
  // asks from another thread to see that file work is done outside it.
  bool chainLockFreeForTesting() const {
    if (!chainMutex.tryEnter())
      return false;
    chainMutex.exit();
    return true;
  }

  // The setlist library (ProcessorLibrary.cpp, docs/library.md): setlists
  // as folders of JSON presets under one root, by default Pedaleira in the
  // user's Documents. Slots are numbered from 1, as the file names show
  // them. All of it is message-thread API and none of it touches audio.
  //
  // Process-wide: every processor made from here on keeps its library under
  // `dir` instead (an invalid File restores the real one). Returns the
  // previous override. Tests and tools call it before any processor exists.
  static juce::File setLibraryRootForTesting(const juce::File& dir) {
    return SetlistLibrary::setRootForTesting(dir);
  }
  static constexpr const char* kFactorySetlistId = "factory";
  juce::File getLibraryRoot() const { return library->root(); }
  // The setlist the preset list shows: the selected one while it exists,
  // else the first of the user's own, else the factory setlist.
  juce::String currentSetlistId() const;
  // { setlists: [{ id, name, readOnly, presetCount, current }], current }
  juce::var getSetlists() const;
  // { id, name, readOnly, capacity, current, currentSlot, slots: [{ slot,
  // id, name, sceneCount, tempo, tags, favourite, file }], ignored: [{ file,
  // reason }] }; a void var when there is no such setlist.
  juce::var getSetlistState(const juce::String& setlistId) const;
  // { root, setlists, current: { setlist, slot, presetId }, favourites,
  // recents, unsaved, legacyPresets }
  juce::var getLibraryState();
  juce::Result createSetlist(const juce::String& name, bool select, juce::String* id = nullptr);
  juce::Result renameSetlist(const juce::String& setlistId, const juce::String& name);
  juce::Result deleteSetlist(const juce::String& setlistId);
  juce::Result reorderSetlist(const juce::String& setlistId, int index);
  juce::Result selectSetlist(const juce::String& setlistId);
  // Slot operations; an empty setlist id means the current setlist.
  // `name` may be empty for a slot that holds a preset (its name stays).
  juce::Result savePresetToSlot(const juce::String& setlistId, int slot, const juce::String& name,
                                juce::var* saved = nullptr);
  // Loading from another setlist selects that setlist.
  juce::Result loadPresetSlot(const juce::String& setlistId, int slot);
  juce::Result movePresetSlot(const juce::String& setlistId, int from, int to, bool insert);
  juce::Result duplicatePresetSlot(const juce::String& setlistId, int slot, int toSlot,
                                   juce::var* made = nullptr);
  juce::Result deletePresetSlot(const juce::String& setlistId, int slot);
  juce::Result copyPresetToSetlist(const juce::String& setlistId, int slot,
                                   const juce::String& toSetlistId, int toSlot,
                                   juce::var* made = nullptr);
  // Writes a preset (an empty id: the rig as it is now, under `name`) as a
  // JSON file anywhere. Its assets go to an Assets folder beside the file,
  // or into the file itself with `embedAssets`.
  juce::Result exportPresetJson(const juce::String& presetId, const juce::File& file,
                                bool embedAssets, const juce::String& name = {});
  // Reads a JSON preset (or a .t3kpreset) from anywhere into an empty slot
  // (0 = the first one). Assets are looked for in the file itself, in an
  // Assets folder beside it and in the library, and copied into the library.
  juce::Result importPresetJson(const juce::File& file, const juce::String& setlistId, int slot,
                                juce::var* made = nullptr);
  // The same for a preset tree already in hand (what the importer of foreign
  // presets builds): into an empty slot (0 = the first one), the assets it
  // names that nobody has listed in `made`'s missingAssets.
  juce::Result importPresetTree(const juce::ValueTree& preset,
                                const std::vector<t3k::presetjson::AssetRef>& missingAssets,
                                const juce::String& setlistId, int slot,
                                juce::var* made = nullptr);
  // Copies the user presets of the old .t3kpreset store into a setlist
  // (made when missing, a second one when the first is full), keeping their
  // ids. The old files are not touched, and a preset imported before is
  // skipped. { imported, skipped, failed, setlists: [ids] }
  juce::var importLegacyPresets(const juce::String& setlistName);
  // Reads every file of the library again; `tidy` also renames the preset
  // files to what their slots and names make them. { renamed } plus
  // getLibraryState.
  juce::var rescanLibrary(bool tidy);
  juce::Result setPresetFavourite(const juce::String& presetId, bool favourite);
  // [{ id, name, setlist, slot, factory }], most recent first; presets that
  // are gone are left out.
  juce::var getRecentPresets() const;
  // The rig differs from the preset it was loaded from or saved as: the
  // chain as an undo snapshot sees it, or a preset-managed parameter. With
  // no preset active it is "not at the default" (isChainAtDefault). After a
  // session restore the session's rig is what it is compared with.
  bool hasUnsavedChanges();
  // Loads a preset straight from a file (.t3kpreset or .json), outside any
  // store: what the render tool and tests do with a file they were handed.
  // Assets of a JSON preset are looked for beside the file, in the library
  // the file sits in, and in this instance's library.
  bool loadPresetFile(const juce::File& file);

  // Tuner: enabled by the UI while the tuner screen is visible. Reads the raw
  // (pre-gain, pre-gate) input so gating never starves the pitch detector.
  void setTunerEnabled(bool enabled) { tuner.setEnabled(enabled); }
  bool isTunerEnabled() const { return tuner.isEnabled(); }
  juce::var getTunerReading() { return tuner.getReading(); }

  // Auto balance: one-shot chain energy match.
  // startAutoBalance() arms a "listening" measurement: the audio thread
  // accumulates the raw chain outputs' energy (pre-balance, pre-pan, so the
  // result is the chains' true mismatch at any pan position; silence-gated)
  // until ~2 s of real signal has been heard, then pollAutoBalance()
  // (called from the message thread by the UI's poll loop) maps the measured
  // dB difference onto the outputBalance parameter (host-automatable, so
  // undo/presets come free).
  // One-shot measurement of real playing is the industry norm here:
  // continuous AGC fights the player's dynamics, and an injected noise burst
  // is both audible and unrepresentative for nonlinear amp chains.
  void startAutoBalance();
  void cancelAutoBalance();
  juce::var pollAutoBalance();  // { state: "idle"|"listening"|"done"|"timeout", matchedDb? }

  // Auto align: probe-based chain time alignment (stereo chain mode). One
  // press drives both chains with an identical internal sweep, output muted
  // for under half a second, and measures the inter-chain lag and relative
  // polarity to sub-sample precision; see AutoOffset.h for the rationale
  // and threading model. pollAutoOffset() runs the analysis and applies the
  // result (align offset + power, polarity flip) when it's trustworthy.
  void startAutoOffset();
  void cancelAutoOffset();
  juce::var pollAutoOffset();  // { state: "idle"|"listening"|"done"|"timeout", matchedMs?, polarityFlipped?, progress? }

  // Location of the on-disk diagnostic log. Single source of truth shared by the
  // FileLogger setup and the UI's "copy/reveal logs" actions so they never drift.
  // macOS: ~/Library/Logs/Pedaleira/Pedaleira.log, Windows: %APPDATA%/Pedaleira/Pedaleira.log;
  // <app data>/Pedaleira.log while LibraryPaths::setAppDataDirForTesting redirects the folder.
  static juce::File getLogFile();

  // Location of the shared machine-wide settings file (the multi-core
  // preference). Logged once at startup (see the constructor) so
  // a "settings don't persist" report can be diagnosed straight from the
  // log instead of guessed at.
  static juce::File getSettingsFile();

  // PropertiesFile options for the native UI's per-machine preferences
  // (hint bar, PC numbers, cached session…); see plugin/ui/services/UiPrefs.h.
  static juce::PropertiesFile::Options uiPreferencesOptions();

private:
  // See commandLayer(). Reset first thing in the destructor: its server
  // threads and sandbox rigs must be gone before anything else is.
  std::unique_ptr<EngineCommands> appCommands;

  // One chain of blocks. Two of these make up `lanes` (declared below).
  using Lane = std::vector<std::unique_ptr<ChainBlock>>;

  // Helper methods
  // Attenuation-only unit-energy gain for an IR file, matched to what the
  // convolver actually runs (rate-corrected, same length cap). `maxIrFileSamples`
  // is in the file's own sample rate, like the cap passed to loadImpulseResponse.
  float computeIrNormalizationGain(const juce::File& irFile, size_t maxIrFileSamples);
  juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

  // Tone loading helpers
  std::vector<uint8_t> fetchModelFromUrl(const juce::String& modelUrl);
  // Cache-hit loads bypass fetchModelFromUrl, so this keeps the stash
  // honest for local models: recreates a missing stash copy from the cached
  // bytes and re-stamps an existing one's mtime (the GC's liveness signal).
  void refreshLocalStashCopy(const juce::String& modelUrl, const std::vector<uint8_t>& bytes);
  // Shared tail of loadLocalTone / loadLocalTonePath: dedupe the stashed
  // models by content id, synthesize the local tone JSON, and route it
  // through swapTone (existing tone tile) or loadTone (insert slot).
  juce::var finishLocalToneLoad(const juce::String& title,
                                const juce::Array<juce::var>& stashedModels,
                                const juce::String& firstError, int fileCount,
                                const std::string& targetInsertId);

  /** Largest frame count the chain stage can see per boundary callback at the
      base rate: the host max block size converted to 48 kHz frames (and never
      below the host size, so the direct 48k path is covered too). Floors at 1
      to avoid prepare(0). */
  int chainBaseBlockSize() const noexcept;

  /** Largest frame count the chain domain can see per callback: the base
      block size times the current oversampling factor. */
  int chainDomainBlockSize() const noexcept;

  /** The block size IR convolvers are prepared with right now: the base
      block capped at kIrConvolverMaxBlockSize (see ChainBlock.h). */
  int irConvolverBlockSize() const noexcept {
    return irConvolverBlockSizeFor(chainBaseBlockSize());
  }

  /** The effective chain rate: kChainBaseSampleRate × oversampling factor.
      Safe from any thread (the factor is atomic; it only changes inside the
      full re-prepare paths, never under the audio thread's feet). */
  double chainSampleRate() const noexcept {
    return kChainBaseSampleRate * chainOversampleFactor.load(std::memory_order_relaxed);
  }

  /** Everything a block engine in `row` needs from the processor, read at
      one moment (see BlockProcessor.h). Per row because the voice count is:
      a dual-mono row's NAM engines carry two voices. Safe from any thread
      like the values it gathers; the install and re-prepare paths call it
      under chainMutex. */
  BlockPrepareSpec blockPrepareSpecFor(int row) const noexcept {
    return {chainSampleRate(), chainOversampleFactor.load(), chainBaseBlockSize(),
            chainDomainBlockSize(), 2, wantedNamVoices(row)};
  }
  /** The row a block sits in, or -1 when it is in none. Caller holds
      chainMutex. */
  int rowOfBlock(const ChainBlock& block) const noexcept;
  /** The spec for a build that runs off the chain lock and has only an id
      at hand: that of the row holding the block with that id (a block, or
      the insert slot a new block is headed for), else of the row a new
      block would land in. Takes chainMutex briefly; the install step
      re-checks against the live row (applyPreparedModelToChainBlock). */
  BlockPrepareSpec blockPrepareSpecForId(const std::string& blockId);

  struct PreparedBlockModel {
    bool success = false;

    // The complete replacement engine, built and prepared off-thread against
    // the chain domain as it was then. Prepares can race prepareToPlay at
    // startup (restore-time loads run first) and an oversampling or
    // input-mode change in flight, so the apply step checks it against the
    // live domain before it goes live: a NAM engine's phase and voice counts
    // are baked in and get the block re-queued, block-size drift is repaired
    // in place (BlockProcessor::prepareForInstall). After the apply this
    // holds the block's *previous* processor instead.
    std::unique_ptr<BlockProcessor> processor;
  };

  /** CPU/file heavy; call without holding `chainMutex`. NAM engines come out
      at the given slimmable size (the callers read it off the block, so the
      tier selection and prewarm run out here instead of under the apply
      lock). */
  PreparedBlockModel prepareBlockModelOffThread(ChainBlockType type, const std::vector<uint8_t>& modelData,
                                                const juce::String& filename, double namSlimSize,
                                                int namVoices);
  /** Bring a registry-kind block in line with its saved node (the KindState
      child of `blockState`): restore the parameters onto the processor it
      holds when that is already the right kind, else build the kind and
      install it; a kind this build does not have turns the block into a
      missing-kind placeholder that keeps the node. Called from
      applyBlockSettings, the funnel for restored, duplicated and pasted
      blocks, so the caller holds chainMutex. That means the kind's factory
      holds chainMutex. Nothing heavy runs under it: a processor that has
      to be built is queued on the loader pool (buildKindInBackground) and
      the block waits for it like a tone block waits for its model
      (`modelLoading`, with the node kept in pendingKindState). */
  void applyKindState(ChainBlock& block, const juce::ValueTree& blockState);
  /** Loader thread: build and prepare a `kindId` processor from `kindState`
      off the chain lock, then install it on the block by swap, unless the
      block is gone or has since asked for another build (`serial`). */
  void buildKindInBackground(const std::string& blockId, const juce::String& kindId,
                             const juce::ValueTree& kindState, int serial);

  // ---- Hosted plugins (kind "plugin") -------------------------------------
  /** applyKindState for a node that names a hosted plugin. A block already
      running that plugin takes the node's chunk on its live instance (on
      the message thread; elsewhere it is rebuilt). Anything else waits for
      a new instance (queueHostedPluginBuild), unless the scanner does not
      know the plugin as an addable effect: then the block becomes the
      missing-kind placeholder under the plugin's name, keeping `blockState`
      whole. Caller holds chainMutex. */
  void applyHostedPluginState(ChainBlock& block, const juce::ValueTree& blockState,
                              const juce::ValueTree& kindState, int serial);
  /** Mark `block` as loading and queue the creation of its plugin instance
      for the message thread (see HostedPluginLink). `savedNode` is the
      block node the request came from when there is one (a restore): a
      failed creation keeps exactly that node. Caller holds chainMutex; any
      thread. */
  void queueHostedPluginBuild(ChainBlock& block, const juce::PluginDescription& description,
                              const juce::ValueTree& kindState,
                              const juce::ValueTree& savedNode, int serial);
  /** Message thread, off the chain lock: create the instance, synchronously
      where the format allows it, else asynchronously (an Audio Unit v3). */
  void createHostedPlugin(const std::string& blockId, const juce::PluginDescription& description,
                          const juce::ValueTree& kindState, const juce::ValueTree& savedNode,
                          int serial);
  /** Message thread: wrap, restore and prepare the new instance off the
      chain lock, then install it on the block through the fade handshake.
      A null instance, or one with no usable layout, turns the block into a
      placeholder instead. */
  void finishHostedPlugin(std::unique_ptr<juce::AudioPluginInstance> instance,
                          const juce::String& creationError, const std::string& blockId,
                          const juce::PluginDescription& description,
                          const juce::ValueTree& kindState, const juce::ValueTree& savedNode,
                          int serial);
  /** Blocks that are placeholders for a hosted plugin the scanner now
      knows are applied again from their saved node. Message thread. */
  void reviveMissingPluginBlocks();

  // What the message-thread closures above hold instead of `this`: cleared
  // by the destructor, so a creation that completes after the processor has
  // gone finds nobody and lets its instance go.
  // It also carries the queue of builds waiting for the message thread. A
  // posted message only wakes the queue up; the work itself is run by
  // whoever gets to runQueuedHostedPluginWork first. That matters for a
  // caller that is ON the message thread and waiting for the rig to load
  // (state.waitUntilLoaded in a build that cannot pump its own message
  // loop): the message it is waiting for could never be delivered, but it
  // can run the queue itself (serviceHostedPlugins does).
  struct HostedPluginLink {
    std::atomic<TONE3000Processor*> processor{nullptr};
    std::mutex mutex;
    std::vector<std::function<void(TONE3000Processor&)>> queue;
  };
  std::shared_ptr<HostedPluginLink> hostedPluginLink;
  /** Message thread: run every queued hosted-plugin build. */
  void runQueuedHostedPluginWork();
  // Polls serviceHostedPlugins while the rig holds a hosted block: the
  // plugin's own notifications may arrive on the audio thread, where
  // nothing can be posted.
  struct HostedPluginTimer : juce::Timer {
    explicit HostedPluginTimer(TONE3000Processor& p) : processor(p) {}
    void timerCallback() override;
    TONE3000Processor& processor;
  };
  HostedPluginTimer hostedPluginTimer{*this};
  // Message thread only.
  juce::uint32 hostedAttentionSeen{0};
  juce::uint32 scannerGenerationSeen{0};
  bool servicingHostedPlugins{false};

  /** Short path under `chainMutex` only: swaps the new processor onto
      `block` and stamps `newType` (a tone swap may change the block's type;
      the old engine kept processing under the old type until this moment).
      The block's *old* processor ends up back in `prepared`; the caller must
      let `prepared` die *after* releasing the lock, because engine
      destructors (NAM graphs, convolution state) are far too heavy to run
      while the audio thread may be blocked on chainMutex. */
  void applyPreparedModelToChainBlock(ChainBlock& block, ChainBlockType newType,
                                      PreparedBlockModel& prepared);

  /** Ask the audio thread to glide `blockId`'s wet path down to silence,
      then wait (bounded) for the fade, so the caller's change (engine swap,
      failure drop, block removal) never splices the waveform (audible
      click). `muteWetOnly` picks the fade shape (see ChainBlock.h): false
      crossfades the output toward the block's dry input, right when the
      change ENDS at bypass (removal, failure drop); true mutes just the wet
      term while the dry share of the user's mix holds, right for engine
      swaps, whose dry input was never audible (at 100% mix the bypass fade
      would blast the un-processed signal, e.g. a raw amp head with no cab).
      Returns immediately when the block isn't audibly processing or no
      audio callbacks are running (the change is inaudible then anyway).
      Must be called WITHOUT holding chainMutex. */
  void requestSwapFadeAndWait(const std::string& blockId, bool muteWetOnly = false);

  /** The fade a loader job needs before it installs model `modelId` on
      `blockId`, or nothing at all when the block no longer wants that model
      (checked under the lock: a superseded job must not dip a block that
      has moved on). A block whose current processor has latency is leaving
      its lane's timing behind, so the whole chain is muted (the returned
      fade, which the caller keeps alive across its install); otherwise the
      block's own swap fade, shaped by `muteWetOnly`. */
  struct ChainEditFade;
  std::unique_ptr<ChainEditFade> fadeForModelInstall(const std::string& blockId, int modelId,
                                                     bool muteWetOnly);

  /** Same idea for structural edits that can't be expressed as one block's
      wet fade (reorder, cross-lane move): glide the whole chain output to
      silence and wait (bounded) so the edit splices in silently. The caller
      clears `chainEditFadePending` afterwards to glide back; use the
      ChainEditFade RAII below. Must be called WITHOUT holding chainMutex. */
  void requestChainEditFadeAndWait();

  /** Deferred release of the chain-edit fade for whole-chain restores
      (preset load, undo/redo): a restore queues its blocks' engines on the
      background loader, so releasing the mute at scope exit plays the raw
      dry input at full level for the whole reload window, the "preset
      switch pop". This instead keeps the chain muted until no block is
      `modelLoading` anymore (or a bounded cap expires; a network fetch
      shouldn't hold the rig silent), then glides back in on the ready rig.
      Called via ChainEditFade::releaseWhenChainLoadsSettle. */
  void releaseChainEditFadeWhenLoadsSettle();

  /** RAII wrapper for the chain-edit fade: fades the chain output to silence
      on construction, lets it glide back on scope exit (after the mutation,
      including early-error returns). Restores that queue background model
      loads call releaseWhenChainLoadsSettle() instead, handing the release
      to a background waiter that holds the mute until the loads land. */
  struct ChainEditFade {
    explicit ChainEditFade(TONE3000Processor& proc) : p(proc) {
      // Newer fade session: any deferred waiter still in flight stands down
      // (its generation is stale); this fade owns the release now.
      p.chainEditFadeHoldGeneration.fetch_add(1);
      p.requestChainEditFadeAndWait();
    }
    ~ChainEditFade() {
      if (!deferred)
        p.chainEditFadePending.store(false);
      // Every edit that can move the chain's block latency runs inside one
      // of these, and this destructor runs after the edit's chain lock has
      // been released (the lock is declared after the fade), which is where
      // a latency report belongs: the caller reads the new value on return.
      p.syncReportedLatency();
    }
    void releaseWhenChainLoadsSettle() {
      deferred = true;
      p.releaseChainEditFadeWhenLoadsSettle();
    }
    TONE3000Processor& p;
    bool deferred{false};
  };

  /** True while the audio thread is actively receiving callbacks (updated
      every processBlock). The fade handshakes skip their bounded waits when
      audio is stopped: nothing is audible, and blocking a click gesture on
      the message thread for the full timeout would feel sluggish. */
  bool isAudioActive() const {
    return juce::Time::currentTimeMillis() - lastAudioCallbackMs.load() < 150;
  }

  // Run one chain (the per-block loop) over the supplied working buffer. The buffer may have
  // 1 channel (a single side in stereo mode) or 1-2 channels (mono mode). All per-channel work
  // is keyed on buffer.getNumChannels(). Runs inside the chain domain (48 kHz; see
  // ChainDomain.h). Must be called while holding `chainMutex`.
  // `dryScratch` is the dry-copy scratch the per-block mix blends against,
  // per lane (see laneDryScratch) so the two lanes never share mutable state
  // and can run concurrently.
  // `beginIdx`/`endIdx` bound the block range [beginIdx, endIdx): a plan
  // segment is a stretch of one row (the branched routing splits a lane
  // around the tap point). What the blocks need to know about the rest of
  // the signal path comes in `segment`, worked out by the caller, because
  // the path does not end where the range does (see SegmentCtx).
  struct SegmentCtx {
    // Highest index of an enabled and loaded NAM block on the path this
    // segment belongs to: its own row, or INT_MAX when an amp follows in a
    // row the path continues into (see lastActiveNamIndex and
    // planSegmentLastNamIndex).
    int lastNamIndex = -1;
    // The buffer's two channels are two independent mono signals.
    bool dualMono = false;
  };
  void processChainOnBuffer(std::vector<std::unique_ptr<ChainBlock>>& blocks,
                            juce::AudioBuffer<float>& buffer,
                            juce::AudioBuffer<float>& dryScratch, int beginIdx, int endIdx,
                            const SegmentCtx& segment);
  // Highest index in [scanBegin, scanEnd) of a NAM block that is loaded and
  // switched on, or -1. Caller holds chainMutex.
  static int lastActiveNamIndex(const Lane& l, int scanBegin, int scanEnd) noexcept;

  // The whole chain stage at the chain rate: the active plan over the given
  // ports. Called either directly (48k host, oversampling off), from the
  // oversampler halves, or as the boundary's callback. `inputs` carries one
  // channel pair per input strip and `outputs` one per output bus, indexed
  // by port (on a mono host buffer the main pair's second pointer is the
  // scratch channel). A legacy plan works in place on output port 0 after
  // an input-to-output copy (skipped when the two alias). Caller holds
  // `chainMutex`.
  void processChainStage(float* const* const* inputs, float* const* const* outputs,
                         int numFrames);

  // ---- Plan executor (ProcessorPlanExec.cpp) ------------------------------
  // What one invocation of the chain stage hands the plan: the channel
  // pointers of each port and the frame count. A legacy plan has the one
  // port, the main pair.
  struct PlanStageIo {
    std::array<float* const*, kMaxIoPairs> ports{};
    // Grid plans: where each bus is written (by down-port). Legacy plans
    // work in place on `ports`.
    std::array<float* const*, kMaxIoPairs> busPorts{};
    int numFrames = 0;
  };
  // The grid's two own operations (ProcessorPlanExec.cpp): fill a row's
  // buffer from its strip with the row trim, and send it to its bus through
  // the alignment pad and the row mix.
  void runRowIn(const PlanOp& op, const PlanStageIo& io) noexcept;
  void runRowOut(const PlanOp& op, const PlanStageIo& io) noexcept;
  // Run one sequence of `plan` in order. A Fork runs its children on the
  // worker pool when this callback may fork and at least two of them carry
  // work, else one after the other on this thread; the children write
  // disjoint buffers, so both schedules give the same samples. No
  // allocation, locks or logging. Caller holds chainMutex; pool workers
  // inherit that protection because they run entirely inside this call.
  void runPlanSeq(const ProcessingPlan& plan, int seq, const PlanStageIo& io);
  void runPlanFork(const ProcessingPlan& plan, const PlanOp& fork, const PlanStageIo& io);
  // Does any block in the sequence (its nested forks included) do audible
  // processing? Gates a fork: dispatching an idle sequence costs more than
  // running its empty loop inline.
  bool planSeqHasWork(const ProcessingPlan& plan, int seq) const noexcept;
  // The grid's joins (ProcessorPlanExec.cpp): a split's fork and its mix,
  // and one row summed into another.
  void runSplit(const PlanOp& op, const PlanStageIo& io, bool mix) noexcept;
  void runFeedRow(const PlanOp& op, const PlanStageIo& io) noexcept;
  // The lastNamIndex of a RunBlocks segment (see SegmentCtx).
  int planSegmentLastNamIndex(const PlanOp& op) const noexcept;
  // The channel pointers a BufRef names for this invocation, into `out`
  // (two entries). Returns the channel count. A reference that names no
  // buffer of this invocation (or a buffer too short for it) returns 0 and
  // is counted in unresolvedPlanRefs: an operation is never skipped
  // without a trace.
  int resolvePlanBuffer(const BufRef& ref, const PlanStageIo& io, float** out) const noexcept;
  mutable std::atomic<int> unresolvedPlanRefs{0};

  // ---- Input strips and output buses (host rate) --------------------------
  // processBlock is: the input strips, the chain stage (boundary,
  // oversampler, plan), the output strips. A strip or bus is a channel pair
  // of the plan (ProcessingPlan::Strip / Bus); up to kMaxIoPairs of each
  // can run, and a legacy plan has the one main pair of both.

  // What a callback binds the plan's strips and buses to, on its stack. A
  // pair is either the host buffer's first two channels in place (the
  // legacy main pair; `mainPair` is then the host buffer itself, or a
  // two-channel view of a wider one) or a view of the pair's own working
  // buffer. Null means the pair sits the callback out (see bindHostIo).
  struct HostIo {
    juce::AudioBuffer<float>* mainPair = nullptr;
    juce::AudioBuffer<float> mainPairView;
    std::array<juce::AudioBuffer<float>, kMaxIoPairs> stripViews, busViews;
    std::array<juce::AudioBuffer<float>*, kMaxIoPairs> strips{}, buses{};
  };
  // Bind every strip and bus of rtIo for this callback. A strip that is not
  // in place gathers its device channels (through the device channel map)
  // into its working buffer; a channel the device does not have is silence.
  // Then every host channel that no in-place pair occupies is cleared: it is
  // an output from here on, and nothing in a legacy rig writes it.
  void bindHostIo(juce::AudioBuffer<float>& buffer, HostIo& io) noexcept;
  // Put the buses that are not in place onto their device channels. Every
  // bus has a pair of its own (a missing pair was folded onto Out 1/2 when
  // the plan was compiled), so this is a copy; a pair with only its first
  // channel on the device is folded to mono, half of (L + R).
  void scatterOutputBuses(juce::AudioBuffer<float>& buffer, const HostIo& io) noexcept;
  // Everything of processBlock that works on audio: strips, chain stage,
  // buses. processBlock calls it once with the host buffer, or, for a grid
  // rig handed more frames than prepareToPlay promised, once per slice of
  // the promised size (the working pairs are that long).
  void processHostBlock(juce::AudioBuffer<float>& buffer);

  // One input strip, in place on `strip` (one or two channels): the fold,
  // input gain, input meters, the tuner tap, the gate, the pitch shifter
  // and, on the legacy main pair, the auto-align probe. The order of the
  // arithmetic is the order processBlock always had.
  void runInputStrip(int index, juce::AudioBuffer<float>& strip);
  // What is folded onto what on the legacy main pair this callback.
  enum class InputFold { None, MirrorFirst, MirrorSecond, SumToMono };
  InputFold legacyInputFold() const noexcept;

  // The chain-edit fade for one callback: the gain is advanced once, into
  // chainEditFadeRamp, and every bus is multiplied by the same values.
  struct EditFade {
    enum class State { Idle, Ramp, Silent };
    State state = State::Idle;
    int rampSamples = 0;    // values in chainEditFadeRamp
    float tailGain = 1.0f;  // the gain of every sample past them
  };
  EditFade advanceChainEditFade(int numSamples) noexcept;
  // One output bus, in place on `bus`, after the chain stage (and, for the
  // legacy main pair, the image stage run per slice inside it): DC blocker,
  // the chain-edit fade, and on the main output strip the tone stack, the
  // auto-align mute, output gain and the output meters.
  void runOutputStrip(int index, juce::AudioBuffer<float>& bus, const EditFade& fade);

  // The post-chain stereo image at the host rate, over the two chain output
  // channels the stage above produced (`chR` is the scratch channel on a
  // mono host buffer): the mode's image engine (Spread / Align), the
  // auto-balance and auto-align taps, then the balance/pan matrix, which
  // becomes a mono fold when stereo chains run on a rig that can't
  // reproduce stereo (`stereoRig` false: mono bus, or a one-channel
  // standalone output device). Called once per chain-stage slice so the
  // fold stays correct even when a host exceeds its promised block size;
  // every processor it touches is a streaming engine, so slicing is
  // transparent. Caller holds `chainMutex` (it runs inside the chain-stage
  // slice loop).
  void processImageStage(float* chL, float* chR, int numFrames, bool stereoRig);

  // Prepare every engine in a row for the current chain rate (see
  // chainSampleRate), chain-domain block size and the row's voice count.
  // Holds no lock.
  void prepareChain(int row);

  // (Re)design one block's rate-dependent linear stages (EQ biquads, spectrum
  // analyzer) for the current chain rate. prepareChain covers the blocks that
  // exist when the chain domain moves (prepareToPlay, oversampling changes);
  // every path that brings a tone block into the chain mid-session must call
  // this as well, or the block's EQ runs coefficients designed for the
  // default 48 kHz inside an oversampled chain and every band lands at
  // freq × factor (issue #212). Resets the filters' state and the analyzer's
  // history. Caller holds chainMutex.
  void prepareBlockForChainRate(ChainBlock& block);

  // Recompute the longest block tail across both lanes into longestBlockTail
  // (BlockProcessor::tailSeconds). Called wherever the set of live engines
  // can change (model apply, block removal, snapshot restore) and, deferred,
  // after a parameter flagged affectsTail moved. Caller must hold chainMutex;
  // getTailLengthSeconds reads the atomic lock-free.
  void refreshBlockTailLength();
  std::atomic<double> longestBlockTail{0.0};  // seconds

  // The chain the UI edits/adds to right now (Left in mono mode, or the active side in stereo).
  std::vector<std::unique_ptr<ChainBlock>>& activeChain();

  // Enforce the lane's insert-slot invariant after any structural change:
  // insertCount == max(kMinLaneSlots - toneCount, 1). Shortfalls append fresh
  // placeholders (UUID ids) at the end; overshoot trims inserts from the end
  // so slots the user positioned earlier in the lane stay put. Inserts own no
  // engines, so add/remove is trivially cheap. Caller holds chainMutex.
  void normalizeLaneInserts(Lane& l);

  // The topology funnel: every structural mutator ends here. Re-baselines
  // the legacy lanes (normalizeLaneInserts on rows 0 and 1; rows 2 and 3
  // hold nothing in Legacy mode and are left alone), re-resolves the branch
  // tap, keeps the lanes' visible ends even while a branch is active by
  // trimming the branch lane's surplus *trailing* insert placeholders
  // (trim-only; see the definition for the geometry), then compiles the
  // processing plan for the rig as it now stands, bumps topologyRevision
  // and runs the latency pass. Because the caller holds chainMutex (and,
  // for anything audible, a ChainEditFade), the audio thread only ever sees
  // a plan that matches the lanes. Caller holds chainMutex.
  void rebuildTopology();
  // The compile step of the funnel alone. Caller holds chainMutex.
  void compileActivePlan();
  // The rig as the plan compiler sees it. The snapshot is a member that is
  // refilled, so after the first call an install allocates nothing for it.
  // Caller holds chainMutex; the reference is good until the next call.
  const PlanInput& makePlanInput() const;
  mutable PlanInput planInputScratch;
  // Reopen the boundary and clear the oversamplers when the plan's port
  // counts moved. Off the audio thread, under chainMutex and inside the
  // caller's chain-edit fade (the ports lose their history).
  void syncPlanPorts();
  int lastPlanStrips{1};
  int lastPlanBuses{1};

  // Find a block by id across both chains (ids are globally unique). Returns nullptr if absent.
  ChainBlock* findBlockById(const std::string& blockId);

  // Shared tail of duplicateChainBlock/pasteChainBlock: land a freshly built
  // tone block in `side` at `index` (an insert slot there is consumed,
  // anywhere else splices in), restore the lane invariants, bump the revision
  // and queue the block's active model (cache-first). Returns the block's id.
  // Caller holds chainMutex and has already recorded history.
  std::string landToneBlock(std::unique_ptr<ChainBlock> block, int row,
                            int index);

  // State (de)serialization helpers.
  // serializeBlockSettings/applyBlockSettings cover everything user-editable
  // on a block (identity, tone refs, gains, mix, EQ): the single source of
  // truth shared by plugin state persistence and undo/redo snapshots. Model
  // bytes are only included when `includeModelData` is set (project files).
  static juce::ValueTree serializeBlockSettings(const ChainBlock& block);
  void applyBlockSettings(ChainBlock& block, const juce::ValueTree& blockState);
  // Set a block's tone identity in one place: raw JSON, parsed var, and the
  // slim UI summary (see makeToneSummary) always stay in sync.
  static void setToneOnBlock(ChainBlock& block, int toneId, const juce::String& toneJson,
                             const juce::var& parsedTone);
  // Slim tone projection for getChainState: the UI renders only
  // id/title/format/gear/first image/user/model names/share url; shipping
  // the full API payload (model URLs, tags, counts…) per block per sync is
  // waste.
  static juce::var makeToneSummary(const juce::var& toneVar);
  static void serializeChainToTree(const std::vector<std::unique_ptr<ChainBlock>>& blocks,
                                   juce::ValueTree& chainState, bool includeModelData);

  // Undo/redo internals (ProcessorHistory.cpp).
  // Snapshot both chains + stereo mode as a ValueTree. History snapshots stay
  // settings-only; presets embed the model bytes so they load offline.
  // Caller must hold chainMutex.
  juce::ValueTree captureChainSnapshot(bool includeModelData = false) const;
  // Record the pre-mutation state before a chain edit. `coalesceKey` groups a
  // continuous gesture (knob/EQ drags) into a single undo step; pass an empty
  // string for discrete edits. Caller must hold chainMutex.
  void pushChainHistory(const juce::String& coalesceKey = {});
  // Restore a snapshot by reconciling against the live chains: blocks whose
  // tone/model still match keep their loaded engines (undoing a knob tweak
  // never reloads a model); everything else is rebuilt and queued for a
  // background load. Caller must hold chainMutex, and must destroy the
  // returned retired blocks *after* releasing it (engine teardown is heavy).
  [[nodiscard]] Lane restoreChainSnapshot(const juce::ValueTree& snapshot);
  // One block of a reconcile: the live block with that id when it is still
  // the same thing (taken out of `existing` with its engine), else a fresh
  // one, with the node's settings applied and whatever it has to load
  // queued.
  using ParkedBlocks = std::map<std::string, std::unique_ptr<ChainBlock>>;
  void reconcileChainFromTree(const juce::ValueTree& chainState, Lane& target, Lane& retired,
                              ParkedBlocks* shared = nullptr);
  std::unique_ptr<ChainBlock> reconcileBlockFromTree(const juce::ValueTree& blockState,
                                                     ParkedBlocks& existing);

  // ---- Grid mode internals (ProcessorGrid.cpp) ----------------------------
  // All of these want chainMutex held.
  // The mirrors that let other threads ask about the routing without the
  // lock (isGridMode, wantedNamVoices).
  std::atomic<bool> gridModeActive{false};
  std::array<std::atomic<int>, kGridRows> gridRowVoices{};  // set by syncGridAtomics
  void syncGridAtomics() noexcept;
  // Pad every row to exactly kGridCols slots.
  void padGridRows();
  // Why the legacy rig cannot become a grid; empty when it can.
  juce::String promoteRefusal() const;
  // The promotion mapping. Fills `lossy`.
  void promoteToGridLocked(juce::StringArray& lossy);
  juce::StringArray demoteBlockersLocked() const;
  InputMode projectedInputModeLocked() const;
  juce::var gridStateLocked() const;
  // Section 3.3's projection, written back onto the legacy fields and the
  // pan, balance, solo and polarity parameters. Only when there are no
  // blockers.
  void demoteToLegacyLocked();
  // Shared front of the row mutators: arm the chain-edit fade when the edit
  // (or the conversion it implies) needs one, take the lock, record the
  // undo step, convert a legacy rig, run `edit` and rebuild.
  juce::Result editGrid(bool structural, const juce::String& coalesceKey,
                        const std::function<juce::Result()>& edit);
  // Smoothers of the row mix and trim: (re)rated for the chain, and snapped
  // to their targets (inside a fade or with no audio running).
  void prepareGridRows();
  void snapGridRowGains() noexcept;
  // The target gains of a row's send for the current mix values.
  std::pair<float, float> rowSendGains(int row, bool silencedBySolo) const noexcept;
  // The one gain of a row that feeds another row: its volume, mute and
  // polarity. Pan and solo act where a row meets an output.
  float rowFeedGain(int row) const noexcept;

  // ---- Splits (ProcessorGrid.cpp) ------------------------------------------
  // The junction of a row, or null. Caller holds chainMutex.
  SplitJunction* splitOfRow(int upperRow) const noexcept;
  // The audio thread's way to a junction: by upper row, rebuilt whenever
  // grid.splits changes (rebuildTopology). Null where a row has no split.
  std::array<SplitJunction*, kGridRows> rtSplits{};
  void refreshSplitTable() noexcept;
  // (Re)rate every junction for the chain: crossover design, ramps, cleared
  // filters and alignment lines. With the chain re-prepared or re-rated.
  void prepareGridSplits();
  // Why the grid's routing does not hold as it stands, or empty.
  juce::String gridRoutingFault() const;
  // A restored grid whose splits or feeds break the rules is not refused
  // whole: the split or feed at fault is set aside as an inert node (kept
  // and written back) until the rest holds.
  void sanitizeRestoredRouting();
  void setFeedAside(int row);
  void setAllJoinsAside();

  // ---- The legacy image controls in grid mode ------------------------------
  // The pan, balance, solo, polarity, Spread and Align parameters belong to
  // the legacy image stage, which a grid does not run. So that the current
  // UI's controls never show something the rig is not doing:
  //  - where rows 0 and 1 are two mono rows on device outputs, the pans,
  //    solos and polarities ARE those rows' (a move of the parameter moves
  //    the row, and the reverse), and the balance is their two volumes as
  //    an opposed pair when they are one;
  //  - every other one of these is held at its neutral value for as long as
  //    the rig is a grid (a move snaps back), and what it was is kept and
  //    put back when the rig becomes a legacy rig again.
  // Bits of gridLegacyControls say which are bound to the rows.
  enum LegacyControl : int {
    kLegacyPans = 1,      // chainPanLeft / chainPanRight
    kLegacySolos = 2,     // chainSoloLeft / chainSoloRight
    kLegacyInverts = 4,   // chainInvertLeft / chainInvertRight
    kLegacyBalance = 8    // outputBalance
  };
  std::atomic<int> gridLegacyControls{0};
  // Which of them the rows can carry right now. Caller holds chainMutex.
  int legacyControlsBoundLocked() const noexcept;
  // Rows to parameters, and the neutral value for what is not bound.
  // Message thread, caller holds chainMutex (as demoteToLegacyLocked does
  // when it writes the same parameters).
  void syncLegacyControlsFromGrid();
  // A parameter of the set moved (any thread): onto the rows when bound;
  // otherwise the message thread puts it back.
  void legacyControlMoved(const juce::String& parameterId, float newValue);
  // Raised while this processor writes those parameters itself.
  std::atomic<int> legacyControlSyncDepth{0};
  std::atomic<bool> legacyControlResyncPending{false};
  // What the held parameters were when the rig became a grid, by parameter
  // id (real units). Saved in the Grid node; put back by leaveGridMode.
  std::map<juce::String, float> gridHeldLegacyParams;
  void releaseHeldLegacyParams();
  // Writes to those parameters are host notifications and are never made
  // under chainMutex: they are queued under it and made by
  // flushLegacyParamWrites once the lock is released (every path that
  // queues ends in a flush; handleAsyncUpdate is the safety net).
  std::vector<std::pair<juce::String, float>> pendingLegacyParamWrites;
  std::atomic<bool> legacyParamWritesQueued{false};
  void queueLegacyParamWrite(const juce::String& id, float value);
  float legacyParamValue(const juce::String& id) const;
  // Not under chainMutex. Any thread but the audio thread.
  void flushLegacyParamWrites();
  struct LegacyParamFlush {
    TONE3000Processor& p;
    ~LegacyParamFlush() { p.flushLegacyParamWrites(); }
  };
  // The rig as a legacy client would see it, when the grid is one of the
  // legacy shapes: which blocks each lane lists (normalised like a legacy
  // lane), the branch and the input mode. Caller holds chainMutex.
  struct LegacyProjection {
    bool projectable = false;
    bool stereo = false;
    bool dualMono = false;
    std::vector<ChainBlock*> left, right;
    juce::String branchSide, branchAfterBlockId;
    InputMode inputMode = InputMode::Stereo;
  };
  LegacyProjection projectGridToLegacy() const;
  // The legacy lane-shaped edits (positions in a lane, a lane's order) on a
  // grid: one that is a legacy shape goes back to the legacy rig first.
  // Returns false when the rig is still a grid afterwards.
  bool leaveGridForLaneEdit();
  // See the definition (ProcessorChain.cpp). Takes chainMutex.
  int laneIndexToRowSlot(int row, int index) const;
  // Saved state (section 10).
  juce::ValueTree captureGridNode(bool includeModelData) const;
  void restoreGridSnapshot(const juce::ValueTree& snapshot, Lane& retired);
  void reconcileGridFromTree(const juce::ValueTree& gridNode, Lane& retired);
  // Empty when this build can read the snapshot's Grid node, else why not.
  static juce::String gridSnapshotRefusal(const juce::ValueTree& snapshot);
  // Leave grid mode for a legacy snapshot: rows 2 and 3 are emptied (into
  // `retired`) and the row settings go back to their defaults.
  void leaveGridMode(Lane& retired);
  // What a newer build wrote and this one does not understand, kept so the
  // next save writes it back: per row the Input and Output nodes of an
  // unknown type and an unknown format name, and whole children of the Grid
  // node (splits).
  std::array<juce::ValueTree, kGridRows> gridRawInput, gridRawOutput;
  std::array<juce::String, kGridRows> gridRawFormat;
  std::vector<juce::ValueTree> gridUnknownNodes;
  // Chain-rate buffers of the grid plan, sized with laneDryScratch: a row's
  // own buffer, and a bus's sum when the oversampler stands between the
  // plan and the down-ports.
  std::array<juce::AudioBuffer<float>, kGridRows> rowWork;
  std::array<juce::AudioBuffer<float>, kMaxIoPairs> busWork;
  void sizeGridBuffers();
  // The soloed rows, for the chain stage at hand (bit n: row n). Solo acts
  // where a row meets an output: with any row soloed, a send stays open
  // only when its row, or a row that feeds it, is one of them.
  unsigned rtGridSoloMask = 0;
  unsigned gridSoloMask() const noexcept;
  bool rowSilencedBySolo(int row, unsigned soloMask) const noexcept {
    return soloMask != 0 &&
           (soloMask & activePlan.rowInfo[static_cast<size_t>(row)].upstream) == 0;
  }
  int gridCrossBusPaddingOverride{-1};
  bool gridPadAcrossBuses() const noexcept;
  // Where a new block lands when no slot is named: the lane and position of
  // the first empty slot (of the lane being added to in legacy mode; of the
  // first row that has one in grid mode). False when the grid is full.
  bool findLandingSlot(const std::string& targetInsertId, Lane*& targetLane, size_t& index);
  // Whether a block may be put in `row`: the left lane, the right one in
  // stereo mode, any row in grid mode.
  bool rowTakesBlocks(int row) const noexcept;
  // Queue a background download+prepare of `block`'s active model, resolving
  // url/name from its tone JSON. Used by undo/redo when a restored block's
  // model isn't cached in memory anymore. When the model can't even be
  // resolved from the stored tone JSON (corrupt/legacy state), the block is
  // flagged loadFailed so the UI shows the retry affordance instead of an
  // eternal loader.
  void queueActiveModelLoad(ChainBlock& block);

  ChainHistory chainHistory;

  // Scenes and stomps (ProcessorScenes.cpp). Guarded by chainMutex.
  SceneStore scenes;
  StompStore stomps;
  // The three common block settings a scene can hold besides the kind's.
  static bool isCommonSceneParam(const juce::String& param);
  // With more than one scene in use, a parameter that is about to be set and
  // that scene.assignParam would accept is assigned first (its current value
  // in every scene), so the set changes the active scene only. Anything else
  // is left shared. The caller holds chainMutex and has pushed history.
  void autoAssignSceneParam(const ChainBlock& block, const juce::String& param);
  // A block's current value of a scene parameter / setting one, the same
  // stores setBlockParam makes. False for an id the block does not have (or
  // cannot answer for yet: a kind still being built). `affectsTail`, when
  // given, is raised for a kind parameter that moves the reported tail.
  static bool readSceneParam(const ChainBlock& block, const juce::String& param, float& value);
  static bool writeSceneParam(ChainBlock& block, const juce::String& param, float value,
                              bool* affectsTail = nullptr);
  // Give `block` a state of its own in every scene (its current one), so a
  // toggle that follows writes one scene only. No-op when it has one.
  void registerSceneBlock(const ChainBlock& block);
  // Copy what the blocks carry into the active scene's entries. Caller
  // holds chainMutex.
  void captureActiveScene();
  // The store with the active scene's entries current and nothing left of
  // blocks that are gone: what a save and a scene listing read. Caller
  // holds chainMutex.
  SceneStore scenesWithLiveValues() const;
  // Copy scene `index` onto the blocks. Returns whether a reported tail may
  // have moved. Caller holds chainMutex.
  bool applySceneToBlocks(int index);
  // Why `param` of `block` cannot be a scene (or expression) parameter, or
  // an empty string when it can.
  static juce::String sceneParamRefusal(const ChainBlock& block, const juce::String& param);
  // Flip (or set) several blocks' switches as one undo step.
  bool setBlocksEnabled(const std::vector<std::pair<std::string, bool>>& changes);
  // A block's category as getChainState reports it. Caller holds chainMutex.
  static juce::String blockCategoryName(const ChainBlock& block);

  // Footswitches. Mode and rows are atomics (read by any thread's
  // snapshot); the rest is message-thread state.
  static FootswitchMode readPersistedFootswitchMode();
  static FootswitchRowRole readPersistedHybridRow(bool top);
  std::atomic<int> footswitchMode{static_cast<int>(readPersistedFootswitchMode())};
  std::atomic<int> hybridTopRow{static_cast<int>(readPersistedHybridRow(true))};
  std::atomic<int> hybridBottomRow{static_cast<int>(readPersistedHybridRow(false))};
  void persistFootswitchSettings() const;
  std::atomic<int> footswitchBank{0};
  std::atomic<int> footswitchScenePage{0};
  std::array<std::atomic<bool>, kNumFootswitchInputs> footswitchHeld{};
  // The blocks a momentary press flipped, to flip back on release even if
  // the assignment or the mode changed while the switch was down.
  std::array<std::vector<std::string>, kNumFootswitches> momentaryHeldBlocks;
  TapTempo tapTempoCounter;
  // What one of the eight switches does right now, and its target.
  struct FootswitchRole {
    enum class Kind { None, Preset, Scene, Stomp } kind{Kind::None};
    // Preset: index in the preset list. Scene: scene index. Stomp: the
    // switch whose assignment it uses.
    int target{-1};
  };
  FootswitchRole footswitchRole(int footswitch) const;
  // Presets one bank holds in the current mode (8, or 4 with one hybrid
  // preset row), 0 when no switch loads presets.
  int presetsPerBank() const;
  // True when exactly one hybrid row is on scenes (the paged case).
  bool hybridScenePaged() const;
  void handleFootswitch(int footswitch, bool press, double timeMs);
  // Events from other threads, waiting for the message thread: a wait-free
  // ring any number of threads may write (each claims a slot with one
  // fetch_add) and the message thread drains in handleAsyncUpdate.
  static constexpr juce::uint32 kFootswitchQueueSize = 64;
  std::array<std::atomic<juce::uint32>, kFootswitchQueueSize> footswitchQueueEvents{};
  std::array<std::atomic<double>, kFootswitchQueueSize> footswitchQueueTimes{};
  std::atomic<juce::uint32> footswitchQueueWrite{0};
  juce::uint32 footswitchQueueRead{0};
  void queueFootswitchEvent(int footswitch, bool press);
  void drainFootswitchQueue();

  // Block clipboard (see copyChainBlock/pasteChainBlock). `settings` is a
  // serializeBlockSettings tree (invalid = nothing copied); the model bytes
  // ride separately so paste comes up without a network round trip, exactly
  // like duplicate. Guarded by chainMutex; in-memory only (deliberately not
  // part of the DAW session state).
  juce::ValueTree blockClipboardSettings;
  std::map<int, std::vector<uint8_t>> blockClipboardModelCache;

  // MIDI performance handlers (wired to midiMapper in the constructor,
  // both invoked on the message thread).
  // Program change n loads slot n + 1 of the current setlist. Empty slots
  // and out-of-range programs are ignored.
  bool loadPresetAtIndex(int index);
  // Step the active preset through the current setlist's presets, wrapping
  // at the ends and passing over empty slots: the MIDI twin of the preset
  // bar's ‹ › buttons (mapped "presetPrevious" / "presetNext" controls land
  // here). With no active preset, a forward step starts at the first preset
  // and a backward step at the last.
  bool stepPreset(int delta);
  // Toggle the enabled flag of a lane's Nth tone block (0-based, insert
  // slots skipped). Positional so mappings survive tone swaps and preset
  // loads. No-op when the lane is shorter than N, and for the Right lane
  // outside stereo mode, so an inert lane is never edited invisibly.
  bool toggleBlockPower(int position, bool rightLane);

  // Preset internals (ProcessorPresets.cpp).
  // The faceplate parameters a preset carries. Explicitly scoped: rig
  // calibration (calibrateInput, inputCalibrationLevel) and the global
  // loudness target (targetLoudness) describe the user's setup, not the
  // tone, so they stay out of presets. (Per-block normalization rides the
  // chain snapshot itself.)
  static const std::vector<juce::String>& presetParameterIds();
  void setActivePreset(const juce::String& id, const juce::String& name);
  // True when nothing distinguishes the state from a fresh instance: no
  // active preset, mono mode, no tone blocks, every preset-managed parameter
  // at its default. Ships as getChainState's `atDefault`, which greys the
  // top bar's New button. Caller must hold chainMutex.
  bool isChainAtDefault() const;

  // The old .t3kpreset store: today the home of the factory presets, and
  // of the user presets importLegacyPresets offers.
  PresetManager presetManager;
  // Never null. A pointer so setPresetStoreForTesting can re-root it.
  std::unique_ptr<SetlistLibrary> library =
      std::make_unique<SetlistLibrary>(SetlistLibrary::defaultRoot());
  // Shown in the preset pill; guarded by chainMutex (written on the message
  // thread, read by getChainState).
  juce::String activePresetId;
  juce::String activePresetName;

  // The library, for everything but the migration itself: the first call
  // brings the old store over when that is due (below).
  SetlistLibrary& userLibrary() const;
  // The upgrade path (ProcessorLibrary.cpp, docs/library.md "Presets from
  // before the library"). Before the library, host program n was entry n of
  // the old store's list. The first time this instance uses a library that
  // has no setlist and no record of it having been done, that list becomes
  // the default setlist, entry n in slot n + 1, so program n is still the
  // same preset. Runs once per library (the record is in library.json) and
  // only when the old store holds presets of the user's own.
  void ensureLegacyMigration() const;
  void migrateLegacyStore() const;
  // Not for a processor that only had its library moved for a test or a
  // tool (setLibraryRootForTesting) and still looks at the machine's real
  // old store: a scratch library never pulls in the developer's presets.
  bool legacyMigrationAllowed{!SetlistLibrary::rootIsOverridden()};
  mutable juce::CriticalSection legacyMigrationLock;
  mutable bool legacyMigrationStarted{false};  // guarded by legacyMigrationLock
  mutable std::atomic<bool> legacyMigrationDone{false};
  // See sharePresetStoreWith.
  bool libraryGuest{false};
  // After setStateInformation: this instance's setlist is the one the
  // session's active preset is in, held in memory only (library.json is the
  // player's last choice, not a restored session's).
  void holdRestoredPosition();

  // Library internals (ProcessorPresets.cpp, ProcessorLibrary.cpp).
  // The current setlist by slot: entry i is slot i + 1, with an empty id
  // for an empty slot, up to the last slot in use.
  std::vector<PresetManager::Info> currentPresetSlots() const;
  std::vector<PresetManager::Info> factoryPresets() const;
  // The setlist an argument names (empty = the current one).
  juce::String resolveSetlistId(const juce::String& setlistId) const;
  // The rig as a preset tree, without name or id.
  juce::ValueTree buildPresetTree();
  // Makes `preset` the rig (the undoable restore, the parameters, the fade)
  // and the active preset, named by the tree's "name" unless a name is
  // given. False when the tree has no chain this build can load.
  bool applyPresetTree(const juce::ValueTree& preset, const juce::String& presetId);
  bool applyPresetTree(const juce::ValueTree& preset, const juce::String& presetId,
                       const juce::String& presetName);
  // What a guest of the library answers a write that would replace
  // something with (see sharePresetStoreWith).
  static juce::Result guestCannotReplace(const juce::String& what);
  // The tree of any preset id (library or factory), or an invalid tree.
  // `located` receives where a library preset is.
  juce::ValueTree readPresetTree(const juce::String& presetId,
                                 std::optional<SetlistLibrary::Located>* located = nullptr) const;
  void presetListChanged();
  static juce::var slotVar(const SetlistLibrary::Slot& slot, const juce::StringArray& favourites);

  // What hasUnsavedChanges compares with: the settings-only snapshot and
  // the preset-managed parameters as they were when the active preset was
  // loaded or saved. Guarded by chainMutex.
  struct PresetBaseline {
    bool valid{false};
    juce::MemoryBlock chain;
    std::vector<float> params;  // normalised, in presetParameterIds order
    // A restore finishes in the background (kinds are built, models land),
    // and a block may then write itself a hair differently from the node it
    // was built from. The chain is captured again once the loads settle,
    // unless something was edited in between.
    bool awaitingSettle{false};
    juce::uint32 editSerial{0};
    // The rig was put in place already differing from the active preset
    // (applyRigTree with `unsaved`): unsaved whatever the comparison says.
    bool knownUnsaved{false};
  };
  PresetBaseline presetBaseline;
  // Counts chain edits (every pushChainHistory call).
  juce::uint32 presetEditSerial{0};
  juce::MemoryBlock presetChainSignatureLocked() const;
  void markPresetBaseline();

  // Host program API internals (ProcessorPresets.cpp). The count is a hard
  // constant: JUCE's VST3 wrapper sizes its program parameter once at
  // construction, and a count that tracked the user-editable preset list
  // would leave hosts stale (or kill the mechanism outright below two
  // presets). 128 covers the MIDI program range; slots past the list end
  // are inert.
  static constexpr int kNumHostPrograms = 128;
  // Message-thread work for a setCurrentProgram call from another thread
  // (-1 = none, last one wins); drained in handleAsyncUpdate.
  std::atomic<int> pendingHostProgram{-1};
  // Load the program's preset (message thread). Empty slots are ignored;
  // re-selecting the already-active preset is a no-op (hosts echo the
  // program parameter back after every change and on session restore, and a
  // reload would wipe knob tweaks made since the preset loaded).
  void applyHostProgram(int index);
  // Short-lived preset-list snapshot for getProgramName: hosts ask for all
  // 128 names in a row and list() reads every preset file (factory presets
  // embed model bytes), so names must not cost a folder scan each. Message
  // thread only; preset mutations clear it so fresh names show immediately.
  std::vector<PresetManager::Info> hostProgramInfoCache;
  juce::uint32 hostProgramInfoCacheTime{0};

  // See getDeviceChannelMap. Written by prepareToPlay under chainMutex (a
  // host never runs it beside processBlock, but the message thread reads the
  // map under that lock while it compiles a plan); the audio thread reads
  // the channel arrays.
  DeviceChannelMap deviceChannels;
  // What the device (or, without one, the main bus) offers right now.
  DeviceChannelMap readDeviceChannelMap() const;
  // The legacy rig's two measurements (auto-align, auto-balance) need its
  // stereo image stage. One in flight when the rig stops being that is
  // ended: on the message thread by whatever makes the rig a grid, and by
  // the audio thread should it still find one running in a grid. Any thread.
  void cancelLegacyMeasurements() noexcept;
  // The main bus has one input channel but two or more outputs (a mono
  // source on a wider rig): the buffer's second channel carries no input,
  // so the main strip mirrors the first onto it, as for a mono input
  // device. Resolved in prepareToPlay.
  bool rtMonoMainInput = false;

  // True when running as the standalone app with a mono input device selected.
  // Detected in prepareToPlay (device changes re-trigger it); processBlock then
  // duplicates channel 0 into channel 1 so the dry path is heard on both
  // speakers. Without this, a mono interface/mic only ever feeds the left
  // channel and an empty (or IR-only) chain plays back one-sided.
  std::atomic<bool> standaloneMonoInput{false};

  // Output-side twin: the standalone app with a one-channel output device.
  // The buffer can still be stereo then (the device only plays channel 0),
  // so this feeds stereoOutputDetected below; without it Spread would run
  // and the listener would hear half the double, and a stereo-chain rig
  // would play its Left chain alone instead of the mono sum.
  std::atomic<bool> standaloneMonoOutput{false};

  // The rows of the grid. In Legacy mode lanes[0] = Left/primary (the only
  // lane in mono mode), lanes[1] = Right (stereo mode), and lanes[2] and
  // lanes[3] stay empty (no blocks, no insert placeholders, never saved).
  // Kept as one array so per-lane logic (find, meters, serialization,
  // reconciliation) is written once.
  std::array<Lane, kNumLanes> lanes;
  Lane& lane(ChainSide side) { return lanes[static_cast<size_t>(laneIndex(side))]; }
  const Lane& lane(ChainSide side) const { return lanes[static_cast<size_t>(laneIndex(side))]; }

  std::atomic<bool> stereoEnabled{false};
  // Which lane loadTone inserts into when the target insert id is stale.
  // Set by the UI when an Add starts; not a view mode.
  ChainSide pendingAddSide{ChainSide::Left};
  juce::CriticalSection chainMutex;

  // Routing (see GridTopology.h), guarded by chainMutex. The mode is fixed
  // at Legacy until grid mode exists: the fields above and below stay
  // authoritative and `grid` only lends its per-row audio state (the
  // alignment delays).
  RoutingMode routingMode{RoutingMode::Legacy};
  GridTopology grid;
  // What the chain stage runs, compiled by rebuildTopology from the lanes
  // and the routing; the latency pass (refreshLatencyCompensation) keeps
  // its pads and total current. Guarded by chainMutex: a swap is an
  // assignment made while the audio thread is locked out.
  ProcessingPlan activePlan;
  // Bumped by every compile. The audio thread checks the plan it is about
  // to run against this and against the lanes' lengths; a mismatch means a
  // structural mutator skipped the funnel.
  juce::uint32 topologyRevision{0};
  std::atomic<int> stalePlanCallbacks{0};
  bool planMatchesLanes() const noexcept;
  // A row's format. Legacy mode derives it from the processor's own fields.
  RowFormat rowFormat(int row) const noexcept;

  // Chain branch state (see the public setChainBranch).
  // All guarded by chainMutex (the audio thread reads them with the lock
  // held). `branchAfterBlockId` empty = no branch (today's independent
  // chains). `rtBranchTapIndex` is the id resolved to a trunk-lane index for
  // the RT path (-1 when no branch or while the branch lies dormant in mono
  // mode); refreshBranchTapIndex re-resolves it after every structural change
  // and clears the branch when the tapped block no longer lives in the trunk
  // lane (removed, moved across, stale restore).
  ChainSide branchSourceSide{ChainSide::Left};
  std::string branchAfterBlockId;
  int rtBranchTapIndex{-1};
  void refreshBranchTapIndex();

  // Global chain-edit fade (see ChainEditFade / requestChainEditFadeAndWait).
  // The gain rides the host-rate buffer right after the chain stage; the
  // smoother is audio-thread-only, the flags are the cross-thread handshake.
  std::atomic<bool> chainEditFadePending{false};
  std::atomic<bool> chainEditFadeDone{false};
  juce::LinearSmoothedValue<float> chainEditFadeGain;
  // One callback's worth of that gain (see advanceChainEditFade). It holds
  // one value more than the fade has steps, so a callback of any length is
  // past the glide where the array ends. Sized in prepareToPlay.
  std::vector<float> chainEditFadeRamp;
  // Deferred-release bookkeeping (releaseChainEditFadeWhenLoadsSettle): the
  // generation stamps which fade session a background waiter belongs to
  // (a newer ChainEditFade invalidates in-flight waiters), and the deadline
  // caps how long a restore may hold the chain muted while models load.
  std::atomic<int> chainEditFadeHoldGeneration{0};
  std::atomic<juce::uint32> chainEditFadeHoldDeadlineMs{0};

  // Milliseconds timestamp of the last processBlock, for isAudioActive().
  std::atomic<juce::int64> lastAudioCallbackMs{0};

  // Output-stage gain (main level only; the balance trim lives in the
  // post-chain image matrix, pre-pan), smoothed so knob moves glide instead
  // of stepping once per block. Audio thread only.
  juce::SmoothedValue<float> outputGainSmoother;

  // Monotonic revision of everything getChainState() reports. Bumped on every
  // chain mutation (structure, params, load completion, stereo/side changes)
  // so the UI can cheaply skip resyncs when nothing changed. Mutable + const:
  // getChainState() promotes settled gesture edits into a bump (see below).
  mutable std::atomic<juce::uint32> chainRevision{1};
  void bumpChainRevision() const { chainRevision.fetch_add(1); }

  // Deferred revision bump for continuous gestures (knob/EQ drags). Bumping
  // per drag tick would make every 500 ms poll re-serialize and re-ship the
  // whole chain mid-gesture; instead the mutator records "params changed at
  // <now>" and getChainState() converts it into a real bump once the gesture
  // has been quiet for kGestureSettleMs. The dragging view holds its own
  // optimistic value, so nothing is stale meanwhile.
  static constexpr juce::int64 kGestureSettleMs = 400;
  mutable std::atomic<juce::int64> pendingParamBumpAt{0};  // 0 = nothing pending
  void deferredRevisionBump() { pendingParamBumpAt.store(juce::Time::currentTimeMillis()); }

  // Queue a background download+prepare of a tone's model for `blockId`.
  // Shared by loadTone (new block) and swapTone (existing block).
  void queueToneLoad(const std::string& blockId, int modelId, const juce::String& modelUrl,
                     const juce::String& modelName, ChainBlockType type);

  // Flip a block's loadFailed flag (with a revision bump) after a background
  // download/prepare failure, so the UI can offer a retry instead of showing
  // loading dots forever. No-op if the block is gone.
  void markBlockLoadFailed(const std::string& blockId);

  // Chain-domain resampling boundary (see ChainDomain.h and
  // ChainBoundaryMulti.h): one up-port per input strip and one down-port
  // per output bus on one frame schedule, so one of each for a legacy rig.
  // Engaged (non-null) only when the host rate differs from the chain base
  // rate; created/reset in prepareToPlay, so the audio thread never sees it
  // change. Opening it for another number of ports allocates, so that too
  // belongs off the audio thread, under chainMutex.
  std::unique_ptr<ChainBoundaryMulti> chainBoundary;
  // Chain oversamplers (see ChainOversampler.h), one per port: instance p
  // raises up-port p and lowers down-port p (the two directions keep
  // separate filter state). They raise the chain rate to
  // kChainBaseSampleRate × factor inside the boundary; at factor 1 they are
  // not run at all. All kMaxIoPairs are prepared whatever the plan uses, so
  // a plan with more ports finds its buffers already there. The factor
  // atomic is the single source of truth for the live chain rate; it's only
  // written inside the re-prepare paths (prepareToPlay /
  // applyOversamplingSettings) while the chain is quiesced.
  std::array<ChainOversampler, kMaxIoPairs> chainOversamplers;
  void prepareChainOversamplers(int factor);
  std::atomic<int> chainOversampleFactor{1};
  // The chain stage behind the oversamplers: the callable both invocation
  // paths (direct and boundary) share. Ports as for processChainStage, at
  // the chain base rate.
  void processOversampledChainStage(float* const* const* inputs, float* const* const* outputs,
                                    int numFrames);
  // Boundary latency in host samples (0 at a 48k host). Constant per host
  // rate; chain edits never change reported latency.
  int chainBoundaryLatency = 0;
  // Reports boundary + pitch shift + chain block latency to the host.
  // Message thread: prepareToPlay, the pitch power / window parameter
  // changes, and whenever the chain's block latency moved
  // (refreshLatencyCompensation).
  void updateLatency();

  // Block latency (BlockProcessor::latencySamples).
  // A block that delays its wet path needs three things from the chain, and
  // this sets all of them up from the live lanes: the dry share of its mix
  // delayed by the same amount (ChainBlock::dryDelay, or a mix below 100%
  // comb-filters); the two lanes, or the branch and the trunk's remainder,
  // padded to the longer one so parallel paths stay in phase (the plan's
  // Delay operations, on GridRow::outAlign); and the total, in chain-rate
  // samples, for the latency report (chainBlockLatency). The pads and the
  // total are the latency pass of the active plan (computePlanLatency), so
  // this never changes the plan's structure. Latency follows the chain's
  // structure, not
  // the power switches: a bypassed latent block keeps delaying, so toggling
  // it never moves what the host compensates. Every delay line is sized
  // here, off the audio thread; an unchanged one keeps its contents. Called
  // wherever the set of processors or the routing can change. Caller holds
  // chainMutex. When the total moved it asks for a latency report
  // (triggerAsyncUpdate), which never runs under the lock.
  void refreshLatencyCompensation();
  // Raised (under chainMutex) while a restore is rebuilding the lanes: the
  // installs inside it would each publish a total for a half-built chain.
  // The restore refreshes once, at its end.
  bool latencyRefreshSuspended = false;
  std::atomic<int> chainBlockLatency{0};  // chain-rate samples
  // The same report without the message-loop turn, when the caller is on
  // the message thread: called by ChainEditFade as it ends, so a structural
  // edit's caller reads the new latency on return. Never under chainMutex
  // (a host may answer a latency change by calling back in).
  void syncReportedLatency();

  // Deferred engine refresh (see flushDeferredBlockRefresh).
  std::atomic<bool> blockRefreshPending{false};
  void timerCallback() override;

  // Tempo and transport (see BlockTransport). rtTransport is filled at the
  // top of processBlock and copied into every block's context; the two
  // `last*` atomics publish what it resolved to for tempo.get.
  std::atomic<double> internalTempoBpm{kDefaultTempoBpm};
  std::atomic<double> lastEffectiveBpm{kDefaultTempoBpm};
  std::atomic<bool> lastTempoFromHost{false};
  BlockTransport rtTransport;
  juce::int64 rtProcessedSamples = 0;
  // The plan's strips and buses as the audio thread runs them. The input
  // strips run before the chain lock is taken (and still run when a splice
  // holds it), so they cannot read activePlan; this copy is taken under the
  // lock, by the chain stage of a callback for the ones that follow and by
  // prepareToPlay. A callback takes a changed layout over at its very start
  // when the lock is free (so it runs the new strips whole); one that only
  // finds out in its chain stage, after its input strips ran with the old
  // ones, adopts them there and outputs silence once. Either way such a
  // change only happens inside a chain-edit fade. What a row can change
  // without a fade (its trim, its mix) is not in here: those are atomics the
  // row reads itself.
  struct RtIoLayout {
    std::array<ProcessingPlan::Strip, kMaxIoPairs> strips{};
    std::array<ProcessingPlan::Bus, kMaxIoPairs> buses{};
    int numStrips = 0;
    int numBuses = 0;
    bool legacy = true;
    // The one strip that runs the pitch shifter (there is a single
    // shifter): the first that asks for it, or -1.
    int pitchStrip = -1;
  };
  RtIoLayout rtIo;
  // Caller holds chainMutex.
  bool planIoMatchesRt() const noexcept;
  void adoptPlanIo() noexcept;
  // adoptPlanIo on the audio thread: the strips and buses also start from
  // clean state (gate, pad, DC blocker), since they may be other signals
  // than the ones those indices carried before.
  void adoptPlanIoOnAudioThread() noexcept;

  // Second channel handed to the boundary when the host buffer is mono (a
  // port is always a channel pair). Silent in mono chain mode;
  // with stereo chains it becomes the Right lane's working channel: fed a
  // copy of the mono input per slice, and folded into channel 0 by the
  // image stage afterwards.
  juce::AudioBuffer<float> chainScratchChannel;
  // Per-callback routing state for processChainStage, set by processBlock
  // under chainMutex just before invoking the stage (audio thread only).
  // rtStereoChains follows the *mode* alone: stereo chains run on any rig
  // (a mono rig hears them summed; see processImageStage). rtChainChannels
  // is the host buffer's channel count (at most two): what a plan buffer
  // marked followHostChannels narrows to.
  int rtChainChannels = 2;
  bool rtStereoChains = false;
  // Dual mono in effect for this callback: dualMonoEngaged() on a stereo
  // buffer, resolved once at the top of processBlock (before the input
  // fold, which keys on it). The lane runs its 2-channel buffer as two
  // independent mono signals: NAM engines use their second voice instead
  // of fanning out, stereo IRs convolve their left channel on both sides
  // (as inside a stereo-mode lane), Spread stays idle (the output already
  // is two real channels) and Balance trims the two voices against each
  // other.
  bool rtDualMono = false;
  // Per-callback pool handle: &rtWorkerPool when multi-core is enabled and
  // workers are up, nullptr otherwise (everything runs serially). It gates
  // both parallel sections: the plan's forks (see runPlanFork, which also
  // wants work on at least two sides) and the NAM phase forks (see
  // NamEngine::process), which need no fork around them: a mono chain's
  // oversampled NAM blocks fork too. Resolved once per processBlock under
  // chainMutex.
  RtWorkerPool* rtPhasePool = nullptr;

  // Multi-core processing (see the public getMultiCoreEnabled).
  // One pool serves both parallel sections: the stereo lane fork and the
  // NAM phase forks nested inside a lane. The threads live from
  // prepareToPlay to releaseResources regardless of the setting (parked
  // threads are ~free); the setting only gates dispatch, so toggling it is
  // glitch-free and instant.
  RtWorkerPool rtWorkerPool;
  static bool readPersistedMultiCoreEnabled();
  std::atomic<bool> multiCoreEnabled{readPersistedMultiCoreEnabled()};
  // Hosts hand the device's os_workgroup here (AU/Standalone on macOS);
  // forward it so the workers get scheduled with the audio deadline.
  void audioWorkgroupContextChanged(const juce::AudioWorkgroup& workgroup) override {
    rtWorkerPool.setAudioWorkgroup(workgroup);
  }
  // Does the lane do any audible processing in [beginIdx, endIdx)? Gates a
  // fork: dispatching an idle lane costs more than running its (empty) loop
  // inline. A block still gliding through its wet fade counts as work.
  static bool laneHasWork(const Lane& l, int beginIdx, int endIdx) noexcept {
    const size_t last = static_cast<size_t>(juce::jlimit(0, static_cast<int>(l.size()), endIdx));
    for (size_t i = static_cast<size_t>(juce::jmax(0, beginIdx)); i < last; ++i)
      if (l[i]->type != ChainBlockType::INSERT && l[i]->loaded)
        return true;
    return false;
  }

  // TONE3000 OAuth access token (Bearer). Read by `fetchModelFromUrl` from any
  // thread; written by the UI thread via `setAccessToken`.
  juce::String accessToken;
  mutable juce::CriticalSection accessTokenMutex;

  // Thread pool for background model loading
  juce::ThreadPool loadingThreadPool;

  int maxBlockSize = 0;
  bool eqParamsDirty = true;
  // Tracks the tone stack's power switch across blocks so re-enabling can
  // reset the filters (stale biquad state would otherwise ring).
  bool toneEqWasEnabled = true;

  // Audio-thread state of one input strip. The noise gate sits post input
  // gain at the host rate; its power switch is tracked across blocks so
  // re-enabling resets the detector (a stale envelope must never decide the
  // first block after power-on).
  struct InputStripState {
    NoiseGate gate;
    bool gateWasEnabled = true;
    // Host-rate channel pair of a strip that is not in place on the host
    // buffer: its device channels are gathered here. Sized in prepareToPlay.
    juce::AudioBuffer<float> work;
    // Input meters of a strip other than the first (whose meters are the
    // faceplate's, inputMeterLevelL/R).
    std::atomic<float> meterL{-60.0f}, meterR{-60.0f};
    // What a strip that does not run the pitch shifter waits while another
    // one does: a host-rate delay with
    // room for the shifter's longest window, allocated in prepareToPlay.
    // The length in use is stripPadSamples; a change of length starts from
    // silence.
    juce::AudioBuffer<float> padRing;
    int padWritePos = 0;
    int padLength = 0;
    void preparePad(int maxDelaySamples) {
      padRing.setSize(2, juce::jmax(1, maxDelaySamples), false, false, true);
      clearPad();
    }
    void clearPad() noexcept {
      padRing.clear();
      padWritePos = 0;
    }
    void processPad(juce::AudioBuffer<float>& buffer, int delaySamples) noexcept {
      delaySamples = juce::jlimit(0, padRing.getNumSamples(), delaySamples);
      if (delaySamples != padLength) {
        padLength = delaySamples;
        clearPad();
      }
      if (padLength <= 0)
        return;
      const int numChannels = juce::jmin(buffer.getNumChannels(), padRing.getNumChannels());
      const int numSamples = buffer.getNumSamples();
      for (int ch = 0; ch < numChannels; ++ch) {
        float* line = padRing.getWritePointer(ch);
        float* x = buffer.getWritePointer(ch);
        int pos = padWritePos;
        for (int i = 0; i < numSamples; ++i) {
          const float delayed = line[pos];
          line[pos] = x[i];
          x[i] = delayed;
          if (++pos == padLength)
            pos = 0;
        }
      }
      padWritePos = (padWritePos + numSamples) % padLength;
    }
  };
  std::array<InputStripState, kMaxIoPairs> inputStrips;
  // Host samples the unshifted strips wait: the pitch shifter's latency
  // while it is powered, else 0. Written on the message thread (inside a
  // chain-edit fade when more than one strip runs), read by the strips.
  std::atomic<int> stripPadSamples{0};
  void refreshStripPad();

  // Input-stage pitch shifter (post gate, host rate; see PitchShift.h). Runs
  // while powered and through its power-off blend, then not at all, so a
  // powered-off plugin stays bit-exact and zero-latency. The latency it
  // adds is reported from the message thread (see updateLatency), never
  // from processBlock.
  PitchShift pitchShift;

  // Raw APVTS parameter atomics, resolved once in the constructor. The audio
  // thread reads these every block; getRawParameterValue is a string-keyed
  // map lookup and has no business on the RT path.
  struct ParamRefs {
    std::atomic<float>* inputLevel = nullptr;
    std::atomic<float>* outputLevel = nullptr;
    std::atomic<float>* outputBalance = nullptr;
    std::atomic<float>* spreadEnabled = nullptr;
    std::atomic<float>* spreadOffset = nullptr;
    std::atomic<float>* spreadWobble = nullptr;
    std::atomic<float>* spreadWobbleEnabled = nullptr;
    std::atomic<float>* spreadCrossover = nullptr;
    std::atomic<float>* spreadCrossoverEnabled = nullptr;
    std::atomic<float>* spreadDiffuseEnabled = nullptr;
    std::atomic<float>* alignEnabled = nullptr;
    std::atomic<float>* alignOffset = nullptr;
    std::atomic<float>* alignWobble = nullptr;
    std::atomic<float>* alignWobbleEnabled = nullptr;
    std::atomic<float>* alignCrossover = nullptr;
    std::atomic<float>* alignCrossoverEnabled = nullptr;
    std::atomic<float>* alignDiffuseEnabled = nullptr;
    std::atomic<float>* chainPanLeft = nullptr;
    std::atomic<float>* chainPanRight = nullptr;
    std::atomic<float>* chainSoloLeft = nullptr;
    std::atomic<float>* chainSoloRight = nullptr;
    std::atomic<float>* chainInvertLeft = nullptr;
    std::atomic<float>* chainInvertRight = nullptr;
    std::atomic<float>* toneBass = nullptr;
    std::atomic<float>* toneMid = nullptr;
    std::atomic<float>* toneTreble = nullptr;
    std::atomic<float>* gateThreshold = nullptr;
    std::atomic<float>* gateEnabled = nullptr;
    std::atomic<float>* gateRelease = nullptr;
    std::atomic<float>* gateHold = nullptr;
    std::atomic<float>* gateRange = nullptr;
    std::atomic<float>* toneEqEnabled = nullptr;
    std::atomic<float>* targetLoudness = nullptr;
    std::atomic<float>* calibrateInput = nullptr;
    std::atomic<float>* inputCalibrationLevel = nullptr;
    std::atomic<float>* osEnabled = nullptr;
    std::atomic<float>* osFactor = nullptr;
    std::atomic<float>* pitchEnabled = nullptr;
    std::atomic<float>* pitchSemitones = nullptr;
    std::atomic<float>* pitchStep = nullptr;
    std::atomic<float>* pitchTonality = nullptr;
    std::atomic<float>* pitchWindow = nullptr;
  } paramRefs;
  void resolveParamRefs();

  /** The oversampling factor the osEnabled/osFactor parameters currently
      request: 1 when disabled, else 2/4/8. */
  int resolvedOversampleFactor() const;

  // Oversampling live switching.
  // The osEnabled/osFactor listeners bounce to the message thread (relays
  // fire from UI/host threads), where applyOversamplingSettings re-rates the
  // whole chain domain under the chain-edit fade: linear engines re-prepare
  // in place, NAM engines rebuild off-thread from the model cache.
  void parameterChanged(const juce::String& parameterID, float newValue) override;
  void handleAsyncUpdate() override;
  void applyOversamplingSettings();

  // NAM engine voice count a row requires (see NamEngine.h). In Legacy mode
  // every row answers alike, with what the mode asks for: two for a
  // dual-mono mono chain, one otherwise (the dormant right lane included,
  // as it always was). Deliberately keyed on the mode alone, not the rig: a
  // rig change always comes through prepareToPlay, which re-prepares
  // (prewarms) every engine, so a voice that idled on a mono rig never
  // resumes with stale history. In Grid mode it is the row's format. The
  // loader builds this many voices; the apply path and the helper below
  // enforce it.
  int wantedNamVoices(int row) const noexcept {
    // The atomic mirrors, not the topology: loader threads ask this without
    // the chain lock.
    if (gridModeActive.load(std::memory_order_relaxed))
      return gridRowVoices[static_cast<size_t>(juce::jlimit(0, kGridRows - 1, row))].load(
          std::memory_order_relaxed);
    return getInputMode() == InputMode::DualMono && !stereoEnabled.load(std::memory_order_relaxed)
               ? 2
               : 1;
  }
  // Rebuild (from the in-memory model cache, off-thread) every loaded NAM
  // engine whose voice count differs from its row's wantedNamVoices. Voices are
  // fixed at build like the phase count, and an idle voice would resume
  // with stale model history, so a mode change that moves the requirement
  // rebuilds rather than toggling a dormant voice. Marks the blocks
  // loading (`loaded` = false) exactly like applyOversamplingSettings, so
  // callers hold a ChainEditFade and release it when the loads settle.
  // Returns whether anything was queued. Caller holds chainMutex.
  bool requeueNamEnginesForVoiceCount();
  // Shared tail of the two above: queue a cache-first rebuild of `block`'s
  // engine, marking it loading. Caller holds chainMutex.
  void requeueLoadedNamEngine(ChainBlock& block);

  // Per-block cached values (refreshed once per processBlock from paramRefs).
  float cacheInputLevel = 0.5f;
  float cacheOutputLevel = 0.5f;
  float cacheOutputBalance = 0.5f;
  bool cacheSpreadEnabled = false;
  float cacheSpreadOffset = 0.8125f;  // bipolar, 0.5 = center = 0 ms; default +15 ms R
  float cacheSpreadWobble = 0.25f;    // 0..1 of the ±1.2 ms wobble range
  bool cacheSpreadWobbleEnabled = true;
  float cacheSpreadCrossover = 0.5f;  // log map 32.5..520 Hz; 0.5 = 130 Hz
  bool cacheSpreadCrossoverEnabled = true;
  bool cacheSpreadDiffuseEnabled = true;
  bool cacheAlignEnabled = false;
  float cacheAlignOffset = 0.5f;   // bipolar, 0.5 = center = 0 ms
  float cacheAlignWobble = 0.25f;  // same span as spread's wobble
  bool cacheAlignWobbleEnabled = false;
  float cacheAlignCrossover = 0.5f;  // log map 32.5..520 Hz; 0.5 = 130 Hz
  bool cacheAlignCrossoverEnabled = false;
  bool cacheAlignDiffuseEnabled = false;
  float cacheChainPanLeft = 0.0f;
  float cacheChainPanRight = 1.0f;
  bool cacheChainSoloLeft = false;
  bool cacheChainSoloRight = false;
  bool cacheChainInvertLeft = false;
  bool cacheChainInvertRight = false;
  float cacheBassTone = 5.0f;
  float cacheMidTone = 5.0f;
  float cacheTrebleTone = 5.0f;
  float cacheGateThreshold = -80.0f;
  bool cacheGateEnabled = true;
  float cacheGateRelease = 50.0f;   // ms
  float cacheGateHold = 20.0f;      // ms
  float cacheGateRange = 80.0f;     // dB of attenuation when closed
  bool cacheToneEqEnabled = true;
  float cacheTargetLoudness = -18.0f;
  bool cacheCalibrateInput = false;
  float cacheInputCalibrationLevel = 12.0f;
  bool cachePitchEnabled = false;
  PitchShift::Params cachePitch;

  void updateEqCoefficients();
  void updateCachedParameters();
  void processToneStack(juce::AudioBuffer<float>& buffer);

  // Audio-thread state of one output bus. The DC blocker is one per bus
  // (NAM DC can reach any output); ProcessorDuplicator runs one filter
  // instance per channel, so a single duplicator handles mono and stereo
  // buffers alike.
  struct OutputBusState {
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                   juce::dsp::IIR::Coefficients<float>>
        dcBlocker;
    // Host-rate channel pair of a bus that is not in place on the host
    // buffer. Sized in prepareToPlay.
    juce::AudioBuffer<float> work;
    // Output meters of a bus other than the main output strip (whose meters
    // are the faceplate's, outputMeterLevelL/R).
    std::atomic<float> meterL{-60.0f}, meterR{-60.0f};
  };
  std::array<OutputBusState, kMaxIoPairs> outputBuses;

  juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>
      bassFilter;
  juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>
      midFilter;
  juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>
      trebleFilter;

  // Per-lane dry-copy scratch for the block mix stage (chain-domain sized;
  // see chainDomainBlockSize). One buffer per lane so the stereo lanes own
  // disjoint scratch and can process concurrently; each stays 2-channel
  // because mono mode runs a (possibly stereo) buffer through lane 0 alone.
  std::array<juce::AudioBuffer<float>, kNumLanes> laneDryScratch;
  double hostSampleRate = 48000.0;  // Default, updated dynamically in prepareToPlay

  // Default NAM A2 size for new blocks (see setNamSlimSizeDefault). Atomic:
  // written on the message thread, read wherever loadTone stamps a block.
  static double readPersistedNamSlimSizeDefault();
  std::atomic<double> namSlimSizeDefault{readPersistedNamSlimSizeDefault()};

  // Constructor step: applyMachineDefaultParameters over the settings file,
  // unless disabled for tests (see disableMachineDefaultParametersForTesting).
  void seedMachineDefaultParameters();

  // Audio-callback load (timed around processBlock); ships to the UI as the
  // `cpu` field of getMeterLevels for the hint-bar readout.
  juce::AudioProcessLoadMeasurer loadMeasurer;

  // Meter level tracking, per channel (mono sources report L == R).
  mutable std::atomic<float> inputMeterLevelL{-60.0f};
  mutable std::atomic<float> inputMeterLevelR{-60.0f};
  mutable std::atomic<float> outputMeterLevelL{-60.0f};
  mutable std::atomic<float> outputMeterLevelR{-60.0f};

  // True when the plugin is being fed a real stereo source (stereo bus in a
  // host, or a stereo input device in the standalone app). A capability flag:
  // the input-mode selection doesn't affect it. Drives the UI's input-mode
  // button and dual input meters. Reported via getChainState; see
  // updateStereoIoDetection().
  std::atomic<bool> stereoInputDetected{false};

  // True when the plugin can drive two distinct output channels (stereo bus
  // in a host, 2+ output channels in the standalone app). When false, Spread
  // stays idle no matter what its parameter says (the UI greys it out), and
  // stereo chains keep running but are summed to mono by the image stage
  // (see processImageStage; the UI shows a MONO chip on the pan rail).
  // Starts true so an editor opening before the first prepareToPlay doesn't
  // flash those states. Reported via getChainState; see
  // updateStereoIoDetection().
  std::atomic<bool> stereoOutputDetected{true};

  // Input channel mode (see InputMode). Atomic: written by the message
  // thread (faceplate button / state restore), read by the audio thread.
  std::atomic<int> inputMode{static_cast<int>(InputMode::Stereo)};
  bool isStandalone() const { return wrapperType == wrapperType_Standalone; }
  void updateStereoIoDetection();

  // Tuner pitch detection (fed from processBlock when enabled)
  TunerDetector tuner;

  // Auto balance measurement state.
  // Lock-free audio↔message thread handshake. The audio thread only touches
  // the accumulators while `autoBalanceState == Listening`; the message
  // thread only reads them after flipping the state to Measured, so the two
  // sides never race on the same phase.
  enum class AutoBalanceState : int { Idle = 0, Listening, Measured, TimedOut };
  std::atomic<int> autoBalanceState{static_cast<int>(AutoBalanceState::Idle)};
  // Sum of squares per channel. Written by the audio thread only; plain
  // doubles are safe because the message thread reads them strictly after
  // observing state == Measured (the atomic state store/load provides the
  // release/acquire ordering). The sample counters are atomic because the UI
  // polls them for progress while the audio thread is still accumulating.
  double autoBalanceSumL = 0.0, autoBalanceSumR = 0.0;
  std::atomic<juce::int64> autoBalanceSamples{0};
  std::atomic<juce::int64> autoBalanceElapsed{0};  // wall samples since arm, for the timeout
  float autoBalanceMatchedDb = 0.0f;               // result, valid in Measured
  void runAutoBalanceStage(const juce::AudioBuffer<float>& buffer, int numSamples);

  // Post-chain stereo image engines, one per chain mode (the modes are
  // exclusive; the inactive one is force-idled, and mode switches happen under
  // the chain-edit fade, so the hard stop is inaudible):
  //  - Mono mode: the Spread builds a stereo image from the single chain
  //    (an ADT-style double; see Spread.h). Its output correlation ships to
  //    the UI via getMeterLevels. Idle on rigs that can't reproduce stereo.
  //  - Stereo mode: Align time-aligns the two chains via StereoOffset (see
  //    StereoOffset.h). Runs on any rig; on a mono rig it shapes the chains
  //    right before they are summed (see processImageStage).
  Spread spread;
  StereoOffset stereoOffset;

  // Auto-align probe engine (sweep schedule + capture + estimation all live
  // in AutoOffset; the processor injects the probe, taps the chain outputs,
  // applies the mute stage, and writes the result to the parameters; see
  // the public startAutoOffset/pollAutoOffset above).
  AutoOffset autoOffset;

  // Post-chain image matrix gains (per-chain balance trim × constant-power
  // pan, or the mono fold; see imageMatrixGains in Processor.cpp), smoothed
  // so balance/pan moves don't zipper. LtoR = how much of the Left chain
  // lands in the Right output, etc. At the centered/hard-panned default the
  // matrix is the identity and the image stage skips the mix loop entirely;
  // the fold configuration is never the identity, so it always runs.
  juce::SmoothedValue<float> imageGainLtoL, imageGainLtoR, imageGainRtoL, imageGainRtoR;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TONE3000Processor)
};
