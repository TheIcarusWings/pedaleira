#include "Processor.h"

#include "AgentHub.h"
#include "EngineCommands.h"
#include "AppIdentity.h"
#if !HEADLESS
#include "NativeEditor.h"
#if !HEADLESS && !JUCE_IOS
#include "PedaleiraEditor.h"
#endif
#endif
#include "StandaloneStateAutosave.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <cstring>
#include <tuple>

// StandalonePluginHolder: used to inspect the audio device's active channels
// so we can detect a mono input or output (see standaloneMonoInput /
// standaloneMonoOutput).
#if !HEADLESS && JucePlugin_Build_Standalone && ! JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

// ##############
// MAIN PROCESSOR
// ##############
TONE3000Processor::TONE3000Processor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, juce::Identifier("PARAMETERS"), createParameterLayout()),
      // Unity-gain seeds so the duplicators own valid coefficients from birth;
      // prepareToPlay applies the real voicing via updateEqCoefficients().
      bassFilter(juce::dsp::IIR::Coefficients<float>::makeLowShelf(48000, 150.0f, 0.707f, 1.0f)),
      midFilter(juce::dsp::IIR::Coefficients<float>::makePeakFilter(48000, 425.0f, 0.7f, 1.0f)),
      trebleFilter(juce::dsp::IIR::Coefficients<float>::makeHighShelf(48000, 1800.0f, 0.707f, 1.0f)),
      // 2 threads for background loading, +1 so a chain-edit-fade release
      // waiter (releaseChainEditFadeWhenLoadsSettle) never serializes the
      // very loads it is waiting on.
      loadingThreadPool(3) {
  // Attach the file logger first thing: state restore (and the background
  // model loads it queues) runs before prepareToPlay, and its diagnostics
  // used to vanish because the logger didn't exist yet.
  if (!juce::Logger::getCurrentLogger()) {
    juce::Logger::setCurrentLogger(new juce::FileLogger(getLogFile(), "TONE3000 JUCE Log"));
  }

  // Heal the per-user app-data folder before anything writes to it: a
  // root-owned folder fails every settings save and drop-stash write while
  // reads keep working (github issue #76; see ensureWritableDir, which logs
  // whatever it does). Once per process, like the stash GC below.
  static std::once_flag appDataHealFlag;
  std::call_once(appDataHealFlag,
                 [] { ensureWritableDir(getSettingsFile().getParentDirectory()); });

  // One-line snapshot of everything read from the shared machine-wide
  // settings file at construction, plus the file's own path and whether its
  // folder can be written at all: the first things to check when a
  // "settings/login don't persist" or "can't store dropped files" report
  // comes in (wrong/unwritable path, or the file simply isn't there yet).
  juce::Logger::writeToLog(
      "[Processor] Settings file: " + getSettingsFile().getFullPathName() +
      " (exists=" + juce::String(getSettingsFile().existsAsFile() ? "yes" : "no") +
      ") | multiCore=" + juce::String(multiCoreEnabled.load() ? "on" : "off") +
      " | dataDir=" +
      (getSettingsFile().getParentDirectory().hasWriteAccess() ? "writable" : "NOT WRITABLE"));

  resolveParamRefs();

  // What hosted-plugin work posted to the message thread holds instead of
  // `this` (see HostedPluginLink).
  hostedPluginLink = std::make_shared<HostedPluginLink>();
  hostedPluginLink->processor.store(this);

  // Seed the Settings-page parameters (calibration, oversampling) from the
  // machine-wide defaults before the oversampling listeners attach: the
  // chain isn't prepared yet, so there is nothing to re-rate, and
  // prepareToPlay reads the factor fresh. A host restore that follows
  // (setStateInformation) overrides whatever lands here.
  seedMachineDefaultParameters();

  // Age out unused drop-loaded model stash files and sweep IR temp files
  // leaked by older builds (both no-ops after the process's first instance).
  cleanLocalModelStash();
  cleanLeakedIrTempFiles();

  // Oversampling settings apply through a message-thread bounce (see
  // applyOversamplingSettings); the relays can fire from any thread.
  parameters.addParameterListener("osEnabled", this);
  parameters.addParameterListener("osFactor", this);

  // The preset-managed faceplate params feed getChainState's atDefault flag,
  // so their changes must reach pollers (see parameterChanged).
  for (const auto& paramId : presetParameterIds())
    parameters.addParameterListener(paramId, this);
  // The solos are not preset-managed, but in a grid they are two rows'
  // solos (see legacyControlMoved).
  parameters.addParameterListener("chainSoloLeft", this);
  parameters.addParameterListener("chainSoloRight", this);

  // MIDI performance events (delivered on the message thread, see
  // MidiMapper): program changes and prev/next steps walk the preset list,
  // mapped block-power and stereo stomps route through the normal undoable
  // chain edit paths.
  midiMapper.onProgramChange = [this](int program) { loadPresetAtIndex(program); };
  midiMapper.onPresetStep = [this](int delta) { stepPreset(delta); };
  midiMapper.onBlockPowerToggle = [this](int index, bool right) {
    toggleBlockPower(index, right);
  };
  midiMapper.onStereoToggle = [this] { setStereoMode(!isStereoMode()); };
  // The fixed default controls (see MidiMapper): scene select, tap tempo,
  // tuner and footswitch mode. Delivered on the message thread as well.
  midiMapper.onSceneSelect = [this](int scene) { selectScene(scene); };
  midiMapper.onTapTempo = [this](double timeMs) { tapTempo(timeMs); };
  midiMapper.onTunerEnabled = [this](bool enabled) { setTunerEnabled(enabled); };
  midiMapper.onFootswitchMode = [this](int mode) {
    setFootswitchMode(static_cast<FootswitchMode>(juce::jlimit(0, 3, mode)));
  };

  // The two legacy lanes start at their minimum slot layout (kMinLaneSlots
  // pass-through insert placeholders). The right lane stays invisible until
  // stereo mode is enabled, but seeding it now keeps the invariant
  // unconditional. Rows 2 and 3 stay empty. The funnel also compiles the
  // first plan, so the chain stage has one from the first callback on.
  {
    juce::ScopedLock lock(chainMutex);
    for (auto& row : grid.rows) {
      row.meterIn[0].store(-60.0f);
      row.meterIn[1].store(-60.0f);
      row.meterOut[0].store(-60.0f);
      row.meterOut[1].store(-60.0f);
    }
    prepareGridRows();
    rebuildTopology();
    adoptPlanIo();
  }

  // iOS standalone only: nothing else on that platform ever saves the plugin
  // state, so the signal chain would not survive a relaunch (see
  // StandaloneStateAutosave.h). A no-op everywhere else.
  StandaloneStateAutosave::install();

  DBG("TONE3000Processor constructed");
}

// One-time string-keyed lookups; everything after this reads the atomics.
void TONE3000Processor::resolveParamRefs() {
  auto get = [this](const char* id) { return parameters.getRawParameterValue(id); };
  paramRefs.inputLevel = get("inputLevel");
  paramRefs.outputLevel = get("outputLevel");
  paramRefs.outputBalance = get("outputBalance");
  paramRefs.spreadEnabled = get("spreadEnabled");
  paramRefs.spreadOffset = get("spreadOffset");
  paramRefs.spreadWobble = get("spreadWobble");
  paramRefs.spreadWobbleEnabled = get("spreadWobbleEnabled");
  paramRefs.spreadCrossover = get("spreadCrossover");
  paramRefs.spreadCrossoverEnabled = get("spreadCrossoverEnabled");
  paramRefs.spreadDiffuseEnabled = get("spreadDiffuseEnabled");
  paramRefs.alignEnabled = get("alignEnabled");
  paramRefs.alignOffset = get("alignOffset");
  paramRefs.alignWobble = get("alignWobble");
  paramRefs.alignWobbleEnabled = get("alignWobbleEnabled");
  paramRefs.alignCrossover = get("alignCrossover");
  paramRefs.alignCrossoverEnabled = get("alignCrossoverEnabled");
  paramRefs.alignDiffuseEnabled = get("alignDiffuseEnabled");
  paramRefs.chainPanLeft = get("chainPanLeft");
  paramRefs.chainPanRight = get("chainPanRight");
  paramRefs.chainSoloLeft = get("chainSoloLeft");
  paramRefs.chainSoloRight = get("chainSoloRight");
  paramRefs.chainInvertLeft = get("chainInvertLeft");
  paramRefs.chainInvertRight = get("chainInvertRight");
  paramRefs.toneBass = get("toneBass");
  paramRefs.toneMid = get("toneMid");
  paramRefs.toneTreble = get("toneTreble");
  paramRefs.gateThreshold = get("gateThreshold");
  paramRefs.gateEnabled = get("gateEnabled");
  paramRefs.gateRelease = get("gateRelease");
  paramRefs.gateHold = get("gateHold");
  paramRefs.gateRange = get("gateRange");
  paramRefs.toneEqEnabled = get("toneEqEnabled");
  paramRefs.targetLoudness = get("targetLoudness");
  paramRefs.calibrateInput = get("calibrateInput");
  paramRefs.inputCalibrationLevel = get("inputCalibrationLevel");
  paramRefs.osEnabled = get("osEnabled");
  paramRefs.osFactor = get("osFactor");
  paramRefs.pitchEnabled = get("pitchEnabled");
  paramRefs.pitchSemitones = get("pitchSemitones");
  paramRefs.pitchStep = get("pitchStep");
  paramRefs.pitchTonality = get("pitchTonality");
  paramRefs.pitchWindow = get("pitchWindow");
}

juce::AudioProcessorValueTreeState::ParameterLayout TONE3000Processor::createParameterLayout() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;
  // The second ParameterID argument is the AU version hint. AU (Logic /
  // GarageBand) keys parameter identity on it, so once released: never reuse
  // or renumber a hint, and give every new parameter a fresh one.

  // Normalized 0..1 knob params get an explicit 1e-4 interval. The
  // (min, max, default) AudioParameterFloat constructor silently bakes a
  // 0.01 interval into the range, and convertFrom0to1 snaps every UI-set
  // value to that grid: 0.48 dB steps on the ±24 dB knobs, which
  // mangled typed values (-4.0 landed on -3.8; GitHub issue #16). 1e-4
  // matches the UI's own text-entry/fine-drag rounding (KnobControl rounds
  // to 4 decimals), so the snap never moves a value the UI can produce.
  const auto normParam = [](const char* id, int versionHint, float defaultValue) {
    return std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{id, versionHint}, id,
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), defaultValue);
  };

  // Host text for the log-mapped real-unit ranges below (gate release,
  // pitch tonality). A NormalisableRange with skew lambdas has no
  // interval, and JUCE's default stringFromValue then prints seven
  // decimals; a float near 20 kHz only carries about four, so
  // text -> value -> text drifted in the last digits and clap-validator's
  // param-conversions test failed. Whole units, like the UI's own readouts
  // (KnobScale.h), round-trip exactly. Parsing stays JUCE's default
  // (getFloatValue), so a typed "12.5" still lands on 12.5.
  const auto wholeUnitText = [](const char* label) {
    return juce::AudioParameterFloatAttributes()
        .withLabel(label)
        .withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v)); });
  };

  layout.add(normParam("inputLevel", 1, 0.5f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"toneBass", 2}, "toneBass", 0.0f, 10.0f, 5.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"toneMid", 3}, "toneMid", 0.0f, 10.0f, 5.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"toneTreble", 4}, "toneTreble", 0.0f, 10.0f, 5.0f));
  layout.add(normParam("outputLevel", 5, 0.5f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"gateThreshold", 6}, "gateThreshold", -100.0f, 0.0f, -80.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"targetLoudness", 7}, "targetLoudness", -60.0f, 0.0f, -18.0f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"calibrateInput", 8}, "calibrateInput", false));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"inputCalibrationLevel", 9}, "inputCalibrationLevel", -60.0f, 60.0f, 12.0f));

  // Faceplate power switches (gate + global 3-band tone stack).
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"gateEnabled", 10}, "gateEnabled", true));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"toneEqEnabled", 11}, "toneEqEnabled", true));

  // Balance trim: 0.5 = centered (no effect), otherwise an opposing ±12 dB
  // trim between the two chains, applied before the chain pan blend (see the
  // post-chain image stage in processBlock). Pre-pan placement is what makes
  // the knob mean "match chain A to chain B": a setting dialed in while
  // hard-panned stays correct at any pan position. In mono+spread it tilts
  // the dry/lag sides L/R. Only active in stereo mode or mono+spread; the UI
  // hides the knob otherwise.
  layout.add(normParam("outputBalance", 12, 0.5f));

  // Spread (mono chain mode; see Spread.h and plugin/docs/stereo-image.md).
  // Offset is bipolar: 0.5 = center = 0 ms; below center lags the left
  // channel, above center the right (0..24 ms). Wobble is the random-walk
  // delay modulation depth (0..±1.2 ms absolute). Stored normalized;
  // SpreadParams decodes. Defaults land a tight classic ADT (+15 ms R, 25%
  // wobble) so powering spread on is audible immediately.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"spreadEnabled", 13}, "spreadEnabled", false));
  layout.add(normParam("spreadOffset", 14, 0.8125f));
  // Spread advanced-panel deck. All switches default on: the deck is
  // spread's core sound, the panel just exposes its sections. The crossover
  // cutoff rides the shared log map (deckCrossoverHz: 32.5..520 Hz,
  // 0.5 = 130 Hz).
  layout.add(normParam("spreadWobble", 15, 0.25f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"spreadWobbleEnabled", 16}, "spreadWobbleEnabled", true));
  layout.add(normParam("spreadCrossover", 17, 0.5f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"spreadCrossoverEnabled", 18}, "spreadCrossoverEnabled", true));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"spreadDiffuseEnabled", 19}, "spreadDiffuseEnabled", true));

  // Align (stereo chain mode; see StereoOffset.h): corrective alignment
  // delay between the two chains, same bipolar encoding as the spread
  // offset. Defaults to center; a corrective tool has no useful nonzero
  // default. Off by default; the UI shows an advert pill until powered on.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"alignEnabled", 20}, "alignEnabled", false));
  layout.add(normParam("alignOffset", 21, 0.5f));
  // Align advanced-panel deck: the same sections and knob spans as the
  // spread deck, but every switch defaults OFF, so align stays purely
  // corrective until the deck is asked for.
  layout.add(normParam("alignWobble", 22, 0.25f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"alignWobbleEnabled", 23}, "alignWobbleEnabled", false));
  layout.add(normParam("alignCrossover", 24, 0.5f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"alignCrossoverEnabled", 25}, "alignCrossoverEnabled", false));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"alignDiffuseEnabled", 26}, "alignDiffuseEnabled", false));

  // Stereo-mode chain pans: constant-power positions for the Left/Right
  // chain outputs (0 = hard left, 1 = hard right). The UI constrains the
  // Left chain to [0, 0.5] and the Right chain to [0.5, 1]; the DSP takes
  // any absolute position and skips the mix entirely at the hard-panned
  // default. chainPanLinked is a UI behavior flag (mirrored knob moves),
  // persisted as a parameter so sessions and presets restore it.
  layout.add(normParam("chainPanLeft", 27, 0.0f));
  layout.add(normParam("chainPanRight", 28, 1.0f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"chainPanLinked", 29}, "chainPanLinked", true));

  // Per-chain solos (stereo chain mode): audition one chain by muting the
  // other inside the image matrix (see imageMatrixGains). Monitoring state,
  // not tone, so presets don't capture them.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"chainSoloLeft", 30}, "chainSoloLeft", false));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"chainSoloRight", 31}, "chainSoloRight", false));

  // Per-chain polarity flips (stereo chain mode): captures don't share a
  // polarity convention, so two chains fed one instrument can land 180° out
  // (hollow, comb-y, cancels on a mono sum). The flip negates that chain
  // inside the image matrix (see imageMatrixGains). Tone state, unlike the
  // solos, so presets capture it.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"chainInvertLeft", 32}, "chainInvertLeft", false));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"chainInvertRight", 33}, "chainInvertRight", false));

  // Oversampling (Advanced settings; see ChainOversampler.h). Deliberately
  // not automatable: a factor change rebuilds every NAM engine and
  // re-prepares the whole chain. Choice index i maps to factor 2^(i+1).
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"osEnabled", 34}, "osEnabled", false,
      juce::AudioParameterBoolAttributes().withAutomatable(false)));
  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"osFactor", 35}, "osFactor", juce::StringArray{"2x", "4x", "8x"}, 0,
      juce::AudioParameterChoiceAttributes().withAutomatable(false)));

  // Gate advanced-panel deck (right-click the Gate group; see
  // NoiseGate::Params for what each does and why the defaults are what they
  // are). Stored in real units like the threshold, so hosts and preset
  // files read ms / dB. Release rides a log map: the tight end (5-30 ms) is
  // where the ear resolves differences, and a linear knob would spend most
  // of its travel above 200 ms.
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"gateRelease", 36}, "gateRelease",
      juce::NormalisableRange<float>(
          5.0f, 500.0f,
          [](float start, float end, float norm) { return start * std::pow(end / start, norm); },
          [](float start, float end, float ms) {
            return std::log(ms / start) / std::log(end / start);
          }),
      50.0f, wholeUnitText("ms")));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"gateHold", 37}, "gateHold", 0.0f, 200.0f, 20.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"gateRange", 38}, "gateRange", 20.0f, 80.0f, 80.0f));

  // Pitch shift (faceplate, right of the Gate group; see PitchShift.h). Off
  // by default: powering it on is what adds latency, so a fresh chain stays
  // transparent. The shift is a continuous float: with STEP on (the
  // default) the processor rounds it to whole semitones on the way to the
  // engine and the knob detents, with STEP off the knob sweeps smoothly
  // like a whammy pedal (Shift-drag for fine control). Older builds stored
  // these as transpose*; LegacyParamIds.h renames them on load.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"pitchEnabled", 39}, "pitchEnabled", false));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"pitchSemitones", 40}, "pitchSemitones",
      static_cast<float>(-PitchShift::kSemitoneRange), static_cast<float>(PitchShift::kSemitoneRange), 0.0f));
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"pitchStep", 41}, "pitchStep", true));
  // Pitch advanced-panel deck, in real units like the gate deck's. The
  // tonality limit rides a log map over the range where it does something
  // on a guitar; its top end (20 kHz) is "off", the default: a pure shift.
  // The window (the engine's delay buffer, 20 / 30 / 40 / 60 ms) is a
  // 4-way choice and not automatable because, like the oversampling
  // factor, changing it changes the reported latency.
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"pitchTonality", 42}, "pitchTonality",
      juce::NormalisableRange<float>(
          PitchShift::kTonalityMinHz, PitchShift::kTonalityOffHz,
          [](float start, float end, float norm) { return start * std::pow(end / start, norm); },
          [](float start, float end, float hz) {
            return std::log(hz / start) / std::log(end / start);
          }),
      PitchShift::kTonalityOffHz, wholeUnitText("Hz")));
  juce::StringArray windows;
  for (const int ms : PitchShift::kWindowMs) windows.add(juce::String(ms) + " ms");
  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"pitchWindow", 43}, "pitchWindow", windows,
      static_cast<int>(PitchShift::kDefaultWindow),
      juce::AudioParameterChoiceAttributes().withAutomatable(false)));

  return layout;
}

// The plan CPU value's allowance for one input strip or output bus, as a
// fraction of one core at 48 kHz (see makePlanInput).
static constexpr float kPlanPortCost = 0.001f;

int TONE3000Processor::resolvedOversampleFactor() const {
  if (paramRefs.osEnabled == nullptr || paramRefs.osFactor == nullptr)
    return 1;
  if (paramRefs.osEnabled->load() < 0.5f)
    return 1;
  const int choiceIndex = static_cast<int>(std::lround(paramRefs.osFactor->load()));
  return 1 << (juce::jlimit(0, 2, choiceIndex) + 1);
}

void TONE3000Processor::parameterChanged(const juce::String& parameterID, float newValue) {
  juce::ignoreUnused(newValue);
  if (parameterID == "osEnabled" || parameterID == "osFactor") {
    // Defer to the message thread: this can fire from the UI relays or host
    // restore, and the apply is heavy.
    triggerAsyncUpdate();
    return;
  }
  if (parameterID == "pitchEnabled" || parameterID == "pitchWindow") {
    // The only runtime latency edges. Hosts want latency changes off the
    // audio thread (VST3 restarts the component), so they ride the same
    // deferral as the oversampling settings; updateLatency() there is
    // idempotent, so a spurious run costs nothing.
    triggerAsyncUpdate();
  }
  // In a grid the legacy image controls are either two rows' own settings
  // or held at neutral (see legacyControlMoved).
  if (gridModeActive.load(std::memory_order_relaxed) &&
      legacyControlSyncDepth.load(std::memory_order_relaxed) == 0)
    legacyControlMoved(parameterID, newValue);
  // A preset-managed faceplate param moved, so getChainState's atDefault may
  // have flipped. Deferred like block-param drags: a real bump per change
  // would re-ship the whole chain state at knob-drag/automation rates.
  deferredRevisionBump();
}

void TONE3000Processor::handleAsyncUpdate() {
  applyOversamplingSettings();
  refreshStripPad();
  updateLatency();

  // A legacy image control was moved in a grid that holds it at neutral, or
  // that no longer binds it to a row: put it back where the rig has it.
  if (legacyControlResyncPending.exchange(false) && isGridMode()) {
    juce::ScopedLock lock(chainMutex);
    if (routingMode == RoutingMode::Grid)
      syncLegacyControlsFromGrid();
  }
  // What the rig queued for the legacy image controls under the chain lock
  // (the sync above, or any path that did not write them itself), now that
  // nothing is held.
  flushLegacyParamWrites();

  // A host called setCurrentProgram off the message thread: apply the
  // deferred program change here (last one wins, like MidiMapper's PC path).
  if (const int program = pendingHostProgram.exchange(-1); program >= 0)
    applyHostProgram(program);

  // Footswitch presses and releases made on other threads.
  drainFootswitchQueue();
}

// Message thread. Boundary plus a powered pitch shifter's window; read from
// the parameters, not the audio thread's engine, so a change is reported
// exactly once and the report doesn't depend on a callback having run. Plus
// whatever the chain's blocks add (zero for a rig of NAM and IR blocks; see
// refreshLatencyCompensation), converted from chain-rate to host samples.
void TONE3000Processor::updateLatency() {
  int latency = chainBoundaryLatency;
  if (paramRefs.pitchEnabled->load() >= 0.5f) {
    const auto window =
        PitchShift::windowFromIndex(static_cast<int>(std::lround(paramRefs.pitchWindow->load())));
    latency += PitchShift::latencySamples(window, hostSampleRate);
  }
  if (const int blockLatency = chainBlockLatency.load(); blockLatency > 0)
    latency += juce::roundToInt(blockLatency * hostSampleRate / chainSampleRate());
  setLatencySamples(latency);  // no-op (no host notification) when unchanged
}

// See the declaration. The lanes are read as they stand, so every structural
// mutator reaches this through rebuildTopology and every install through
// applyPreparedModelToChainBlock.
void TONE3000Processor::refreshLatencyCompensation() {
  if (latencyRefreshSuspended)
    return;  // a restore is mid-rebuild; it refreshes once when it is done

  // The dry share of every block's mix, whether or not its row is heard: a
  // dormant lane's blocks are sized too, so waking the lane needs nothing.
  for (const auto& l : lanes)
    for (const auto& block : l)
      block->dryDelay.configure(
          block->processor != nullptr ? juce::jmax(0, block->processor->chainLatencySamples()) : 0);

  // A mutator that has changed the lanes always recompiles before it gets
  // here (rebuildTopology). The only way in with a plan of another shape is
  // from inside a mutator that has not finished yet (an install made while
  // it is still moving blocks); it refreshes when it is done.
  if (!planMatchesLanes())
    return;

  // The plan's latency pass: segment latencies, the pad of every path that
  // meets another, and the total. For the legacy shapes that is lane
  // against lane, or with a branch the trunk's remainder against the
  // branch lane (the prefix up to the tap is common to both).
  computePlanLatency(activePlan, makePlanInput());

  std::array<int, kGridRows> pad{};
  for (int i = 0; i < activePlan.numOps; ++i) {
    const PlanOp& op = activePlan.ops[static_cast<size_t>(i)];
    if ((op.type == PlanOp::Delay || op.type == PlanOp::RowOut || op.type == PlanOp::FeedRow) &&
        op.row < kGridRows)
      pad[op.row] = op.latency;
  }
  // A row without a Delay operation (or, in a grid plan, a send or a feed)
  // waits for nobody.
  for (int row = 0; row < kGridRows; ++row)
    grid.rows[static_cast<size_t>(row)].outAlign.configure(pad[static_cast<size_t>(row)]);
  // A mixer's two paths: the one that arrives early waits. A split without
  // a rejoin has no mixer, and its lines are let go.
  for (int upper = 0; upper < kGridRows; ++upper)
    if (SplitJunction* junction = rtSplits[static_cast<size_t>(upper)]) {
      const ProcessingPlan::SplitInfo& info = activePlan.splitInfo[static_cast<size_t>(upper)];
      const bool mixes = !activePlan.legacy && info.present && info.mixCol >= 0;
      junction->setAlignment(mixes ? info.alignA : 0, mixes ? info.alignB : 0);
    }

  const int total = activePlan.reportedChainLatency;
  if (chainBlockLatency.exchange(total) != total)
    triggerAsyncUpdate();  // handleAsyncUpdate reports it, off this lock
}

void TONE3000Processor::syncReportedLatency() {
  if (juce::MessageManager::existsAndIsCurrentThread())
    updateLatency();
}

void TONE3000Processor::timerCallback() {
  stopTimer();
  flushDeferredBlockRefresh();
}

void TONE3000Processor::flushDeferredBlockRefresh() {
  // A hosted plugin that changed its latency or tail by itself only says so
  // through this (it raises blockRefreshPending).
  serviceHostedPlugins();

  if (blockRefreshPending.exchange(false)) {
    bool latencyPending = false;
    {
      juce::ScopedLock lock(chainMutex);
      refreshBlockTailLength();
      for (const auto& l : lanes)
        for (const auto& block : l)
          if (block->processor != nullptr &&
              (block->processor->latencyChangePending() ||
               juce::jmax(0, block->processor->chainLatencySamples()) !=
                   block->dryDelay.delaySamples()))
            latencyPending = true;
    }

    // A moved latency shifts the block's whole lane in time, which no
    // single block's fade can hide: mute-splice like a structural edit.
    // Inside the mute and under one hold of the lock, the kinds adopt
    // their new latency and the chain resizes its compensation, so no
    // callback ever runs a wet path and a dry path that disagree.
    if (latencyPending) {
      ChainEditFade editFade(*this);
      juce::ScopedLock lock(chainMutex);
      for (const auto& l : lanes)
        for (const auto& block : l)
          if (block->processor != nullptr)
            block->processor->commitLatency();
      refreshLatencyCompensation();
    }
  }

  // Also for whatever a background install changed since the last report:
  // without a message loop this call is the only thing that delivers it.
  syncReportedLatency();
}

void TONE3000Processor::setTempo(double bpm) {
  if (!std::isfinite(bpm))
    return;
  internalTempoBpm.store(juce::jlimit(kMinTempoBpm, kMaxTempoBpm, bpm));
}

// Message thread. Re-rates the whole chain domain after an osEnabled/osFactor
// change: the oversampler and every linear engine (EQ/spectrum/smoothers,
// plus each IR block's base-rate island; the convolvers themselves stay at
// the base rate untouched) re-prepare in place under the chain-edit fade;
// NAM engines need a different phase count, so they drop to dry passthrough
// and rebuild off-thread from the block's in-memory model cache, fading back
// in as each lands.
void TONE3000Processor::applyOversamplingSettings() {
  const int newFactor = resolvedOversampleFactor();
  if (newFactor == chainOversampleFactor.load())
    return;

  juce::Logger::writeToLog("[Processor] Oversampling ×" +
                           juce::String(chainOversampleFactor.load()) + " -> ×" +
                           juce::String(newFactor) + " (chain rate " +
                           juce::String(kChainBaseSampleRate * newFactor) + " Hz)");

  // Glide the chain output to silence, splice the rate change in between
  // callbacks, glide back: the same fade structural chain edits use.
  ChainEditFade fade(*this);
  juce::ScopedLock lock(chainMutex);

  chainOversampleFactor.store(newFactor);
  prepareChainOversamplers(newFactor);

  // The chain-domain scratch grows with the factor; the RT path never
  // resizes it.
  for (auto& scratch : laneDryScratch) {
    scratch.setSize(2, juce::jmax(1, chainDomainBlockSize()), false, false, true);
    scratch.clear();
  }
  sizeGridBuffers();
  prepareGridRows();
  prepareGridSplits();

  for (int row = 0; row < kNumLanes; ++row)
    prepareChain(row);
  // A base-rate kind's latency is a different number of chain-rate samples
  // at the new factor.
  refreshLatencyCompensation();

  // IR blocks need nothing here: their convolvers run at the base rate
  // behind per-block islands (re-prepared by prepareChain above), so neither
  // the kernel nor the tail report moves with the factor. In-flight NAM
  // loads are left alone: the apply path's factor-drift guard re-queues
  // them itself.
  for (auto& l : lanes)
    for (auto& block : l)
      if (block->type == ChainBlockType::NAM && block->loaded && !block->modelLoading)
        requeueLoadedNamEngine(*block);

  bumpChainRevision();
}

// Every port's oversampler, used or not: a plan with more strips or buses
// then needs no buffer the audio thread would have to wait for.
void TONE3000Processor::prepareChainOversamplers(int factor) {
  for (auto& oversampler : chainOversamplers)
    oversampler.prepare(factor, juce::jmax(1, chainBaseBlockSize()));
}

void TONE3000Processor::requeueLoadedNamEngine(ChainBlock& block) {
  block.loaded = false;
  block.modelLoading = true;
  queueActiveModelLoad(block);
}

bool TONE3000Processor::requeueNamEnginesForVoiceCount() {
  bool queued = false;
  // The count logged: every row's in Legacy mode, where they all agree.
  int wantedLogged = 1;
  for (int row = 0; row < kNumLanes; ++row) {
    const int wanted = wantedNamVoices(row);
    for (auto& block : lanes[static_cast<size_t>(row)]) {
      // In-flight loads are left alone: the apply path's voice-drift guard
      // re-queues them itself (see applyPreparedModelToChainBlock).
      const NamBlockProcessor* nam = block->nam();
      if (nam == nullptr || !block->loaded || block->modelLoading || nam->engine() == nullptr ||
          nam->engine()->getVoiceCount() == wanted)
        continue;
      requeueLoadedNamEngine(*block);
      queued = true;
      wantedLogged = wanted;
    }
  }
  if (queued) {
    juce::Logger::writeToLog("[Processor] NAM engines rebuilding for " +
                             juce::String(wantedLogged) +
                             (wantedLogged == 1 ? " voice" : " voices"));
    bumpChainRevision();
  }
  return queued;
}

int TONE3000Processor::rowOfBlock(const ChainBlock& block) const noexcept {
  for (int row = 0; row < kNumLanes; ++row)
    for (const auto& b : lanes[static_cast<size_t>(row)])
      if (b.get() == &block)
        return row;
  return -1;
}

BlockPrepareSpec TONE3000Processor::blockPrepareSpecForId(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  int row = -1;
  if (const ChainBlock* block = findBlockById(blockId))
    row = rowOfBlock(*block);
  if (row < 0)
    row = static_cast<int>(&activeChain() - lanes.data());
  return blockPrepareSpecFor(row);
}

RowFormat TONE3000Processor::rowFormat(int row) const noexcept {
  if (routingMode == RoutingMode::Grid) {
    // The row's own, except that two independent signals stay two: below a
    // split from a dual-mono row the lower row is dual mono as well
    // (effectiveRowFormat in ProcessingPlan.cpp is the same rule).
    row = juce::jlimit(0, kGridRows - 1, row);
    for (int r = row;; --r) {
      if (grid.rows[static_cast<size_t>(r)].format == RowFormat::DualMono)
        return RowFormat::DualMono;
      if (r == 0 || splitOfRow(r - 1) == nullptr)
        break;
    }
    return grid.rows[static_cast<size_t>(row)].format;
  }
  // Legacy: two mono lanes in stereo mode; one lane otherwise, whose
  // buffer is a stereo pair or, with dual mono selected, two mono signals.
  if (stereoEnabled.load(std::memory_order_relaxed))
    return RowFormat::Mono;
  return wantedNamVoices(row) == 2 ? RowFormat::DualMono : RowFormat::Stereo;
}

bool TONE3000Processor::planMatchesLanes() const noexcept {
  if (!activePlan.valid || activePlan.topologyRevision != topologyRevision)
    return false;
  for (int row = 0; row < kNumLanes; ++row)
    if (activePlan.rowSlots[static_cast<size_t>(row)] !=
        static_cast<int>(lanes[static_cast<size_t>(row)].size()))
      return false;
  return true;
}

// The rig as the plan compiler sees it (ProcessingPlan.h): the routing
// fields and, per slot, whether a block sits there and how late it makes its
// wet path. Caller holds chainMutex.
const PlanInput& TONE3000Processor::makePlanInput() const {
  PlanInput& input = planInputScratch;
  input.mode = routingMode;
  input.topologyRevision = topologyRevision;
  input.legacy.stereoEnabled = stereoEnabled.load();
  input.legacy.trunkRow = laneIndex(branchSourceSide);
  input.legacy.branchTapIndex = rtBranchTapIndex;
  for (int row = 0; row < kNumLanes; ++row) {
    PlanInput::Row& out = input.rows[static_cast<size_t>(row)];
    const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
    // The row's own setting: the compiler works out what a split makes of
    // it. (A legacy row has no setting; its format follows the mode.)
    out.format = routingMode == RoutingMode::Grid ? gridRow.format : rowFormat(row);
    out.input = gridRow.input;
    out.outputType = gridRow.output.type;
    out.outputPair = gridRow.output.pair;
    out.outputToRow = gridRow.output.toRow;
    const Lane& l = lanes[static_cast<size_t>(row)];
    out.slots.clear();  // keeps its storage
    out.slots.reserve(l.size());
    for (const auto& block : l) {
      PlanSlot slot;
      slot.isBlock = block->type != ChainBlockType::INSERT;
      slot.latency =
          block->processor != nullptr ? juce::jmax(0, block->processor->chainLatencySamples()) : 0;
      slot.costEstimate = block->cpuCost.load(std::memory_order_relaxed);
      out.slots.push_back(slot);
    }
  }

  input.numSplits = 0;
  if (routingMode == RoutingMode::Grid)
    for (const auto& junction : grid.splits)
      if (input.numSplits < kGridRows) {
        PlanInput::Split& split = input.splits[static_cast<size_t>(input.numSplits++)];
        split.upperRow = junction->upperRow;
        split.splitCol = junction->splitCol;
        split.mixCol = junction->mixCol;
      }

  // What the device offers. Before the first prepareToPlay nothing is known
  // and every channel counts as present.
  input.io.known = deviceChannels.numInputs > 0 || deviceChannels.numOutputs > 0;
  input.io.inputChannels = 0;
  input.io.outputChannels = 0;
  for (int i = 0; i < deviceChannels.numInputs; ++i)
    if (const int physical = deviceChannels.inputPhysical[static_cast<size_t>(i)];
        physical >= 0 && physical < 32)
      input.io.inputChannels |= 1u << physical;
  for (int i = 0; i < deviceChannels.numOutputs; ++i)
    if (const int physical = deviceChannels.outputPhysical[static_cast<size_t>(i)];
        physical >= 0 && physical < 32)
      input.io.outputChannels |= 1u << physical;
  input.io.padAcrossBuses = gridPadAcrossBuses();
  // A port is a strip or a bus with its share of the boundary and the
  // oversampler: a fixed allowance, larger with the factor, since nothing
  // measures it.
  input.io.portCost = kPlanPortCost * static_cast<float>(chainOversampleFactor.load());
  return input;
}

// Compile the plan for the rig as it stands and make it the one the chain
// stage runs. Caller holds chainMutex, which is what makes the swap safe:
// the audio thread only reads the plan with the lock held.
void TONE3000Processor::compileActivePlan() {
  ++topologyRevision;
  refreshSplitTable();
  syncGridAtomics();
  const PlanInput& input = makePlanInput();
  const ProcessingPlan compiled = compilePlan(input);
  // A legacy rig always compiles, and so does a grid whose rows hold eight
  // slots each (padGridRows) and whose splits and feeds keep the routing
  // rules, which the mutators check before they change anything (editGrid)
  // and a restore repairs (sanitizeRestoredRouting). Should it ever not,
  // the plan in place stays: it still runs (its ranges are clamped to the
  // lanes), and the stale-plan count says that it no longer describes
  // them.
  jassert(compiled.valid && validatePlan(compiled, input).empty());
  if (compiled.valid)
    activePlan = compiled;
}

int TONE3000Processor::namEngineVoiceCount(const std::string& blockId) const {
  juce::ScopedLock lock(chainMutex);
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->id == blockId) {
        const NamBlockProcessor* nam = block->nam();
        return nam != nullptr && block->loaded && nam->engine() != nullptr
                   ? nam->engine()->getVoiceCount()
                   : 0;
      }
  return 0;
}

int TONE3000Processor::blockLatencySamples(const std::string& blockId) const {
  juce::ScopedLock lock(chainMutex);
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->id == blockId)
        return block->processor != nullptr ? block->processor->chainLatencySamples() : -1;
  return -1;
}

double TONE3000Processor::blockTailSeconds(const std::string& blockId) const {
  juce::ScopedLock lock(chainMutex);
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->id == blockId)
        return block->processor != nullptr ? block->processor->reportedTailSeconds() : -1.0;
  return -1.0;
}

TONE3000Processor::~TONE3000Processor() {
  // The command layer first: it holds a server whose threads call into this
  // processor, and sandbox rigs made from it.
  appCommands.reset();

  parameters.removeParameterListener("osEnabled", this);
  parameters.removeParameterListener("osFactor", this);
  for (const auto& paramId : presetParameterIds())
    parameters.removeParameterListener(paramId, this);
  parameters.removeParameterListener("chainSoloLeft", this);
  parameters.removeParameterListener("chainSoloRight", this);
  cancelPendingUpdate();
  stopTimer();
  // Plugin creations still on their way find nobody and let go.
  hostedPluginLink->processor.store(nullptr);
  hostedPluginTimer.stopTimer();

  releaseResources();

  // Clean up both lanes only when the processor is actually being destroyed
  {
    juce::ScopedLock lock(chainMutex);
    for (auto& l : lanes)
      l.clear();
  }

  juce::Logger::writeToLog("[Processor] Destructor called");

  // Clean up the logger to prevent leaks
  juce::Logger::setCurrentLogger(nullptr);
}

// #############
// JUCE SETTINGS
// #############
const juce::String TONE3000Processor::getName() const {
  return "Pedaleira";
}

bool TONE3000Processor::acceptsMidi() const {
  // MIDI in feeds the mapping engine only (CC/note → parameter, see
  // MidiMapper); the plugin is still an audio effect, not a synth.
  return true;
}
bool TONE3000Processor::producesMidi() const {
  return false;
}
bool TONE3000Processor::isMidiEffect() const {
  return false;
}
double TONE3000Processor::getTailLengthSeconds() const {
  // Two tail sources, report the longer one:
  //  - The longest block tail, which today means the longest loaded IR
  //    (reverb IRs run whole seconds; cab IRs sit far below the DC-blocker
  //    floor). Tracked lock-free in longestBlockTail (this potentially
  //    RT-adjacent query must not take the chain lock) and refreshed
  //    wherever the set of live engines changes (see
  //    refreshBlockTailLength).
  //  - The 5 Hz first-order DC blocker decays over ~10 cycles (2 s); the
  //    reference NAM plugin reports the same allowance for VST3 tail checks.
  const double dcBlockerTailSeconds = 10.0 / 5.0;
  return std::max(longestBlockTail.load(), dcBlockerTailSeconds);
}

// The host program API (getNumPrograms and friends) lives in
// ProcessorPresets.cpp: it exposes the internal preset list as host programs,
// which is the only route MIDI program changes can take in VST3 hosts.

// #############################
// PREPARE A SINGLE CHAIN
// #############################
// Everything in a chain lives in the chain domain: chainSampleRate(), block
// sizes up to chainDomainBlockSize(). Host-rate changes only ever re-prepare
// because the domain block size depends on the host block size; oversampling
// changes re-prepare because the rate itself moves.
void TONE3000Processor::prepareChain(int row) {
  const BlockPrepareSpec spec = blockPrepareSpecFor(row);
  const double chainRate = spec.chainSampleRate;

  for (auto& block : lanes[static_cast<size_t>(row)]) {
    // The engine and its own smoothers (NAM re-prepares for the domain block
    // size; IR convolvers and their base-rate island track the base block
    // and the live factor). Insert placeholders have no processor.
    if (block->processor != nullptr)
      block->processor->prepareForChain(spec);
    // Whatever the dry path was holding belongs to the audio before the
    // re-prepare.
    block->dryDelay.clear();

    // Initialize the shared per-block smoothers (input gain, output gain,
    // mix). The RT path only ever calls setTargetValue on these; reset()
    // belongs here and in the model-apply path, never per block.
    block->inputGainSmoother.reset(chainRate, 0.05f);
    block->outputGainSmoother.reset(chainRate, 0.05f);
    block->mixSmoother.reset(chainRate, 0.05f);
    block->inputGainSmoother.setCurrentAndTargetValue(1.0f);   // updated on first process
    block->outputGainSmoother.setCurrentAndTargetValue(1.0f);  // updated on first process
    block->mixSmoother.setCurrentAndTargetValue(block->mixNormalized);
    block->wetFadeGain.reset(chainRate, kWetFadeSeconds);
    block->wetFadeGain.setCurrentAndTargetValue(block->enabled ? 1.0f : 0.0f);
    block->swapWetMuteGain.reset(chainRate, kWetFadeSeconds);
    block->swapWetMuteGain.setCurrentAndTargetValue(1.0f);
    // Spillover starts settled: a block that is on has its input open, and
    // nothing is ringing out (the engines were just reset by their prepare).
    block->spillInputGain.reset(chainRate, kWetFadeSeconds);
    block->spillInputGain.setCurrentAndTargetValue(1.0f);
    block->spillTailGain.reset(chainRate, kWetFadeSeconds);
    block->spillTailGain.setCurrentAndTargetValue(0.0f);
    block->spillElapsedSamples = ChainBlock::kSpillDone;
    block->engineHoldsAudio = false;

    prepareBlockForChainRate(*block);
  }
}

void TONE3000Processor::prepareBlockForChainRate(ChainBlock& block) {
  const double chainRate = chainSampleRate();
  block.eq.prepare(chainRate);
  block.spectrum.prepare(chainRate);
}

// See the declaration. Both lanes are scanned regardless of stereo mode:
// counting a disabled right lane's IR is a harmless over-report, and it means
// stereo toggles can never truncate a host's tail rendering mid-session.
void TONE3000Processor::refreshBlockTailLength() {
  double maxSeconds = 0.0;
  for (const auto& l : lanes)
    for (const auto& b : l)
      if (b->processor != nullptr)
        maxSeconds = std::max(maxSeconds, b->processor->reportedTailSeconds());
  longestBlockTail.store(maxSeconds);
}

// Constant-power pan gains for a chain at position `pan` (0 = hard left,
// 1 = hard right): cos into the left output, sin into the right.
static std::pair<float, float> constantPowerPanGains(float pan) {
  const float angle = juce::jlimit(0.0f, 1.0f, pan) * juce::MathConstants<float>::halfPi;
  return {std::cos(angle), std::sin(angle)};
}

// Peak absolute sample across the buffer's first `numChannels` channels.
static float bufferPeak(const juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) {
  float peak = 0.0f;
  for (int ch = 0; ch < numChannels; ++ch) {
    const auto* data = buffer.getReadPointer(ch);
    for (int i = 0; i < numSamples; ++i)
      peak = std::max(peak, std::abs(data[i]));
  }
  return peak;
}

// Main-stage level as a linear gain: 0.5 = unity, full range ±24 dB.
static float mainStageGain(float level) {
  return juce::Decibels::decibelsToGain((level - 0.5f) * 48.0f);
}

// Per-chain balance gain: the balance trim (0.5 = centered) applies up to
// ±12 dB opposing between chain 0 (Left) and chain 1 (Right).
static float balanceChainGain(float balance, int chain) {
  const float trimDb = (balance - 0.5f) * 24.0f * (chain == 0 ? -1.0f : 1.0f);
  return juce::Decibels::decibelsToGain(trimDb);
}

// The four gains of the post-chain image matrix: per-chain balance trims
// multiplied into the constant-power pan gains. The balance applies to the
// *chains* (pre-pan), so it matches chain levels rather than tilting the
// output bus; an output-channel trim couldn't re-balance the chains once
// the pan blend has mixed them. When pan is inactive (mono+spread) the pan
// part is the identity and the matrix reduces to a diagonal L/R tilt.
// A solo zeroes the other chain's trim (engaging one clears the other in
// the UI; both on via MIDI leaves both audible), and a polarity invert
// negates its chain's trim. Riding the matrix smoothers makes both
// click-free: the mute glides, and a sign flip glides through zero.
//
// `foldToMono` (stereo chains on a rig that can't reproduce stereo, see the
// image stage) reconfigures the matrix into a mono sum: both chains land on
// both outputs at half their trim, ½(balL·L + balR·R), exactly what a host
// produces when it folds a stereo bus down to mono at the default hard
// pans. The ½ keeps levels consistent across rigs: a rig moved from a
// stereo to a mono track doesn't jump, and one chain duplicated into both
// lanes sums back to its mono-mode level. Balance/solo/invert shape the
// blend as usual; the pans are inert (the UI dims them).
struct ImageGains { float lToL, lToR, rToL, rToR; };
static ImageGains imageMatrixGains(bool panActive, bool foldToMono, float balance,
                                   float panLeft, float panRight, bool soloLeft,
                                   bool soloRight, bool invertLeft, bool invertRight) {
  float balL = soloRight && !soloLeft ? 0.0f : balanceChainGain(balance, 0);
  float balR = soloLeft && !soloRight ? 0.0f : balanceChainGain(balance, 1);
  if (invertLeft)
    balL = -balL;
  if (invertRight)
    balR = -balR;
  if (foldToMono)
    return {0.5f * balL, 0.5f * balL, 0.5f * balR, 0.5f * balR};
  float pLtoL = 1.0f, pLtoR = 0.0f, pRtoL = 0.0f, pRtoR = 1.0f;
  if (panActive) {
    std::tie(pLtoL, pLtoR) = constantPowerPanGains(panLeft);
    std::tie(pRtoL, pRtoR) = constantPowerPanGains(panRight);
  }
  return {balL * pLtoL, balL * pLtoR, balR * pRtoL, balR * pRtoR};
}

// Stereo input = stereo main bus, minus the standalone case where it isn't
// really: a mono input device. Pure capability; the input-mode selection
// doesn't affect it (the UI needs the button to stay visible so the user can
// cycle back to stereo). Stereo output is the same idea on the way out: a
// mono host bus or a one-channel output device can't reproduce a stereo
// image, so Spread stays idle (the UI greys it out) and stereo chains are
// summed to mono (see processImageStage; the UI shows a MONO chip). Both
// reported through getChainState, so bump the revision on change.
void TONE3000Processor::updateStereoIoDetection() {
  const bool stereoIn = getMainBusNumInputChannels() >= 2 && !standaloneMonoInput.load();
  const bool stereoOut = getMainBusNumOutputChannels() >= 2 && !standaloneMonoOutput.load();
  const bool inChanged = stereoInputDetected.exchange(stereoIn) != stereoIn;
  const bool outChanged = stereoOutputDetected.exchange(stereoOut) != stereoOut;
  if (inChanged || outChanged)
    bumpChainRevision();
}

void TONE3000Processor::setInputMode(InputMode mode) {
  // A legacy notion: a grid goes back to the legacy rig first when it can
  // (the conversion derives the mode the rows amount to), and is left alone
  // when it cannot.
  if (isGridMode() && !static_cast<bool>(convertRouting(false)["ok"]))
    return;

  if (mode == getInputMode())
    return;

  // A change into or out of dual mono moves the NAM voice requirement, so
  // the engines rebuild (see requeueNamEnginesForVoiceCount). Mute-splice
  // the whole transition like an oversampling change: the fold, the
  // stereo-IR routing and the Spread gate all flip under the held mute, and
  // the chain glides back in once the rebuilt engines have landed. Other
  // mode changes are a plain fold switch, as before. The fade is armed
  // before the lock (the audio thread needs chainMutex to run it down) and
  // only when needed: no fade, no dip for a Left ↔ Right pick.
  const bool revoice = (mode == InputMode::DualMono) != (getInputMode() == InputMode::DualMono);
  std::optional<ChainEditFade> fade;
  if (revoice)
    fade.emplace(*this);

  bool rebuilding = false;
  {
    juce::ScopedLock lock(chainMutex);
    if (isStereoFeed(mode) && rtBranchTapIndex >= 0) {
      // An *active* branch has a single (mono) source; a stereo feed would
      // silently drop the non-trunk channel. The UI hides the options; this
      // guards MIDI/stale callers. A dormant branch (mono mode) doesn't
      // constrain the fold; re-enabling stereo re-enforces it.
      DBG("setInputMode: stereo feed unavailable while the chain is branched");
      return;
    }
    inputMode.store(static_cast<int>(mode));
    rebuilding = revoice && requeueNamEnginesForVoiceCount();
    bumpChainRevision();
  }
  // Hold the mute until the rebuilt engines land (bounded), like a preset
  // load; a chain with no NAM blocks has nothing to wait for and the fade
  // releases at scope exit.
  if (rebuilding)
    fade->releaseWhenChainLoadsSettle();
  DBG("Input mode: " << inputModeToString(mode));
}

// Tone-stack corners are fixed by design, but hosts can run rates low enough
// to put them above Nyquist (clap-validator probes 1234.5678 Hz, where even
// the 1 kHz mid peak is out of range) and JUCE's bilinear designs return
// unstable coefficients there (the guarding jassert compiles out in release,
// and the filter output runs away to ±inf). Pin the corner just under
// Nyquist instead: magnitude response degrades gracefully, stability holds.
static float clampBelowNyquist(double sampleRate, float frequency) {
  return juce::jmin(frequency, static_cast<float>(sampleRate * 0.49));
}

// #############################
// PREPARATIONS BEFORE RT THREAD
// #############################
void TONE3000Processor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  {
    // A grid edit on the message thread reopens the boundary with these
    // (syncPlanPorts), under this lock.
    juce::ScopedLock lock(chainMutex);
    hostSampleRate = sampleRate;
    maxBlockSize = samplesPerBlock;
  }

  tuner.prepare(sampleRate);

  // CPU readout: proportion of the callback budget spent in processBlock.
  loadMeasurer.reset(sampleRate, samplesPerBlock);

  juce::Logger::writeToLog("[Processor] prepareToPlay: sampleRate=" + juce::String(sampleRate) +
                           ", samplesPerBlock=" + juce::String(samplesPerBlock));

  // Prime the cached parameter values from the resolved atomics.
  updateCachedParameters();


  // Detect mono input/output devices in the standalone app. The device
  // restarts (and re-runs prepareToPlay) whenever the user changes the audio
  // setup, so this stays in sync with the selected device. Hosts (VST3/AU)
  // never take this path; channel layouts there come from the bus
  // configuration.
  standaloneMonoInput.store(false);
  standaloneMonoOutput.store(false);
#if !HEADLESS && JucePlugin_Build_Standalone && ! JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
  if (wrapperType == wrapperType_Standalone) {
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
      if (auto* device = holder->deviceManager.getCurrentAudioDevice()) {
        standaloneMonoInput.store(device->getActiveInputChannels().countNumberOfSetBits() == 1);
        standaloneMonoOutput.store(device->getActiveOutputChannels().countNumberOfSetBits() ==
                                   1);
      }
  }
#endif
  // Asked of the device before the lock is taken; published under it.
  DeviceChannelMap deviceMap = readDeviceChannelMap();
  rtMonoMainInput = getMainBusNumInputChannels() == 1 && getMainBusNumOutputChannels() > 1;

  updateStereoIoDetection();

  // The strips and buses the callbacks from here on run, and with them how
  // many ports the boundary opens. One hold of the lock for the device map,
  // the plan compiled for it and the boundary opened for that plan: a grid
  // edit on the message thread compiles a plan and reopens the boundary
  // under the same lock (syncPlanPorts), so the two can only happen one
  // after the other, and whichever comes last leaves a boundary that
  // matches the plan in place.
  {
    juce::ScopedLock lock(chainMutex);
    deviceChannels = std::move(deviceMap);
    int numUpPorts = 1, numDownPorts = 1;
    // A grid plan's strips and buses depend on which channels the device
    // has (a missing input is silence, a missing output pair falls back to
    // Out 1/2), so it is compiled again for the map just recorded. A legacy
    // plan does not look at the device.
    if (routingMode == RoutingMode::Grid)
      compileActivePlan();
    adoptPlanIo();
    lastPlanStrips = activePlan.numStrips;
    lastPlanBuses = activePlan.numBuses;
    numUpPorts = juce::jlimit(1, ChainBoundaryMulti::kMaxPorts, rtIo.numStrips);
    numDownPorts = juce::jlimit(1, ChainBoundaryMulti::kMaxPorts, rtIo.numBuses);

    // Chain-domain resampling boundary.
    // Engaged whenever the host rate differs from the chain base rate, even
    // for an empty chain, so reported latency is a constant per host rate
    // and chain edits never trigger a PDC change. At a 48k host the
    // boundary is dropped entirely and the chain stage runs directly on the
    // host buffer.
    if (std::abs(sampleRate - kChainBaseSampleRate) > 0.1) {
      if (chainBoundary == nullptr)
        chainBoundary = std::make_unique<ChainBoundaryMulti>();
      // With the rates, block size and port counts unchanged this only
      // clears the audio history, as the single-port container did.
      const bool opened = chainBoundary->reset(sampleRate, kChainBaseSampleRate,
                                               juce::jmax(1, samplesPerBlock), numUpPorts,
                                               numDownPorts);
      jassert(opened);
      // The measured delay of the round trip, the same for every port: the
      // warm-up prefill alone under-reports by the Lanczos kernels' group
      // delay, and hosts align dry paths against this number.
      chainBoundaryLatency = opened ? chainBoundary->latencySamples() : 0;
      if (!opened)
        chainBoundary.reset();
    } else {
      chainBoundary.reset();
      chainBoundaryLatency = 0;
    }
  }
  const bool boundaryNeeded = std::abs(sampleRate - kChainBaseSampleRate) > 0.1;
  juce::ignoreUnused(boundaryNeeded);  // only the debug line below reads it
  // The oversampler is minimum-phase (zero reported latency), so the boundary
  // and a powered pitch shifter are the only latency sources at any factor.
  pitchShift.prepare(sampleRate, juce::jmax(1, samplesPerBlock));
  {
    // Room for the longest window the shifter has.
    int longest = 0;
    for (int w = 0; w < static_cast<int>(PitchShift::kWindowMs.size()); ++w)
      longest = juce::jmax(longest,
                           PitchShift::latencySamples(PitchShift::windowFromIndex(w), sampleRate));
    for (auto& strip : inputStrips)
      strip.preparePad(longest + 1);
    stripPadSamples.store(paramRefs.pitchEnabled->load() >= 0.5f
                              ? PitchShift::latencySamples(
                                    PitchShift::windowFromIndex(static_cast<int>(
                                        std::lround(paramRefs.pitchWindow->load()))),
                                    sampleRate)
                              : 0);
  }
  updateLatency();
  DBG("Chain boundary " << (boundaryNeeded ? "engaged" : "bypassed")
      << " (latency: " << chainBoundaryLatency << " samples)");

  // Chain oversampler.
  // Resolve the requested factor before anything chain-domain is sized: the
  // domain block size and rate both depend on it. Hosts re-run prepareToPlay
  // freely, so this also picks up a factor restored from session state.
  // Prepare every engine in both lanes for the chain domain (fixed rate; the
  // domain block size depends on the host rate/block size).
  {
    juce::ScopedLock lock(chainMutex);
    chainOversampleFactor.store(resolvedOversampleFactor());
    prepareChainOversamplers(chainOversampleFactor.load());
    DBG("Chain oversampling ×" << chainOversampleFactor.load() << " (chain rate: "
        << chainSampleRate() << " Hz)");

    for (int row = 0; row < kNumLanes; ++row) {
      // Restore-time loads can land before the host resolves the saved
      // factor here. prepare() cannot change an engine's phase count.
      // In-flight loads already have a factor guard at installation.
      for (auto& block : lanes[static_cast<size_t>(row)]) {
        const NamBlockProcessor* nam = block->nam();
        if (nam != nullptr && block->loaded && !block->modelLoading && nam->engine() != nullptr &&
            nam->engine()->getOversampleFactor() != chainOversampleFactor.load()) {
          block->loaded = false;
          block->modelLoading = true;
          queueActiveModelLoad(*block);
        }
      }
      prepareChain(row);
    }
    for (auto& gridRow : grid.rows)
      gridRow.outAlign.clear();
    sizeGridBuffers();
    prepareGridRows();
    prepareGridSplits();
    refreshLatencyCompensation();
  }
  // Again now that the chain is prepared: block latency is counted in
  // chain-rate samples, and the factor was only just resolved.
  updateLatency();

  juce::dsp::ProcessSpec spec{sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2};
  bassFilter.prepare(spec);
  midFilter.prepare(spec);
  trebleFilter.prepare(spec);
  for (auto& bus : outputBuses)
    bus.dcBlocker.prepare(spec);
  spread.prepare(sampleRate, samplesPerBlock);
  stereoOffset.prepare(sampleRate, samplesPerBlock);
  autoOffset.prepare(sampleRate);
  for (auto& strip : inputStrips)
    strip.gate.prepare(sampleRate);

  // Chain-edit fade: host-rate. Primed audible normally, but a device can
  // start while a fade session holds the chain (launch: state restore arms
  // the mute, then the audio device opens while models still load). Priming
  // to 1 then would blast the half-loaded chain for the glide-down; honor
  // the pending mute instead and mark it landed (pre-callback, so snapping
  // is safe; this also unblocks any requester waiting out a device
  // restart).
  chainEditFadeGain.reset(sampleRate, kWetFadeSeconds);
  // One value more than the glide has steps (the smoother's own count, and
  // one to spare): see advanceChainEditFade.
  chainEditFadeRamp.assign(
      static_cast<size_t>(juce::jmax(0, static_cast<int>(std::floor(kWetFadeSeconds * sampleRate))) +
                          2),
      1.0f);
  const bool editFadeHeld = chainEditFadePending.load();
  chainEditFadeGain.setCurrentAndTargetValue(editFadeHeld ? 0.0f : 1.0f);
  if (editFadeHeld)
    chainEditFadeDone.store(true);

  // Output-stage gain, primed from the current parameters so a restored
  // session doesn't glide in from the wrong level.
  outputGainSmoother.reset(sampleRate, 0.02);
  outputGainSmoother.setCurrentAndTargetValue(mainStageGain(cacheOutputLevel));

  // Post-chain image matrix (balance × pan, or the mono fold): 20 ms ramps,
  // primed from the current parameters and rig so a restored session doesn't
  // fade in from the wrong image, chain balance or fold. Mirrors the gain
  // resolution in processImageStage (stereoOutputDetected was just updated
  // above).
  {
    const bool isStereo = stereoEnabled.load();
    const bool stereoRig = stereoOutputDetected.load();
    const bool monoFold = isStereo && !stereoRig;
    const bool applyBalance = isStereo || (cacheSpreadEnabled && stereoRig) || dualMonoEngaged();
    const auto g = imageMatrixGains(isStereo && !monoFold, monoFold,
                                    applyBalance ? cacheOutputBalance : 0.5f,
                                    cacheChainPanLeft, cacheChainPanRight,
                                    isStereo && cacheChainSoloLeft,
                                    isStereo && cacheChainSoloRight,
                                    isStereo && cacheChainInvertLeft,
                                    isStereo && cacheChainInvertRight);
    for (auto* smoother : {&imageGainLtoL, &imageGainLtoR, &imageGainRtoL, &imageGainRtoR})
      smoother->reset(sampleRate, 0.02);
    imageGainLtoL.setCurrentAndTargetValue(g.lToL);
    imageGainLtoR.setCurrentAndTargetValue(g.lToR);
    imageGainRtoL.setCurrentAndTargetValue(g.rToL);
    imageGainRtoR.setCurrentAndTargetValue(g.rToR);
  }

  // First-order high-pass at 5 Hz: removes DC offset from nonlinear NAM models
  // while staying audibly and phase-wise transparent down to the lowest bass
  // fundamentals (matches the reference NeuralAmpModelerPlugin behavior).
  for (auto& bus : outputBuses)
    *bus.dcBlocker.state =
        *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass(sampleRate, 5.0f);

  bassFilter.reset();
  midFilter.reset();
  trebleFilter.reset();
  for (auto& bus : outputBuses)
    bus.dcBlocker.reset();

  // Scratch buffers, sized once here; the RT path never resizes them.
  // The lane dry scratches live in the chain domain, where a callback can
  // carry more frames than the host block (e.g. a 44.1k host upsampled to 48k).
  for (auto& scratch : laneDryScratch) {
    scratch.setSize(2, chainDomainBlockSize(), false, false, true);
    scratch.clear();
  }
  chainScratchChannel.setSize(1, samplesPerBlock, false, false, true);
  chainScratchChannel.clear();
  // Host-rate working pairs of the strips and buses that are not in place
  // on the host buffer. A legacy rig uses none of them.
  for (auto& strip : inputStrips) {
    strip.work.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
    strip.work.clear();
  }
  for (auto& bus : outputBuses) {
    bus.work.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
    bus.work.clear();
  }

  // (Re)start the worker pool with the new callback geometry. It idles until
  // a callback actually forks (lanes or NAM phases); starting it here
  // unconditionally keeps the multi-core toggle a pure dispatch gate.
  rtWorkerPool.start(sampleRate, samplesPerBlock);

  // Tone stack coefficients at the real host rate (the construction-time
  // seeds are unity at 48 kHz).
  updateEqCoefficients();
}

// #################
// RELEASE RESOURCES
// #################
void TONE3000Processor::releaseResources() {
  juce::Logger::writeToLog("[Processor] releaseResources() called");

  // The worker pool only lives while the host is running audio callbacks
  // (prepareToPlay restarts it). Stopping here also guarantees no worker
  // outlives the buffers/lanes a stale job could reference.
  rtWorkerPool.stop();

  // DO NOT clear chain blocks here! They should persist across bypass/unbypassed states.
  // Chain blocks are managed by the plugin's state system and should only be cleared
  // when the plugin is actually destroyed or when explicitly requested by the user.
  
  // Only reset the audio processing components, not the plugin state
  bassFilter.reset();
  midFilter.reset();
  trebleFilter.reset();
  for (auto& bus : outputBuses)
    bus.dcBlocker.reset();
  for (auto& strip : inputStrips)
    strip.gate.reset();
}

bool TONE3000Processor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  return isMainBusLayoutSupported(wrapperType, layouts);
}

bool TONE3000Processor::isMainBusLayoutSupported(WrapperType wrapper,
                                                 const BusesLayout& layouts) {
  // The standalone app's player offers the device's active channel counts
  // as one main-bus layout, so a wide main bus is how more than two device
  // channels reach processBlock (extra buses never do: the standalone
  // holder disables them). A processor made outside any wrapper (the tests,
  // the render tool) takes the same layouts so they can be exercised
  // without a device. Counts only: a wide set is discrete channels, not a
  // surround format.
  if (wrapper == wrapperType_Standalone || wrapper == wrapperType_Undefined) {
    const int numIns = layouts.getMainInputChannelSet().size();
    const int numOuts = layouts.getMainOutputChannelSet().size();
    return numIns >= 1 && numIns <= kMaxIoChannels && numOuts >= 1 && numOuts <= kMaxIoChannels;
  }

  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

#if !JucePlugin_IsSynth
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
#endif

  return true;
}

// See getDeviceChannelMap. The standalone player packs the device's active
// channels into the buffer in ascending order, the first as channel 0.
TONE3000Processor::DeviceChannelMap TONE3000Processor::getDeviceChannelMap() const {
  juce::ScopedLock lock(chainMutex);
  return deviceChannels;
}

TONE3000Processor::DeviceChannelMap TONE3000Processor::readDeviceChannelMap() const {
  DeviceChannelMap map;
  map.numInputs = juce::jlimit(0, kMaxIoChannels, getMainBusNumInputChannels());
  map.numOutputs = juce::jlimit(0, kMaxIoChannels, getMainBusNumOutputChannels());
  for (int i = 0; i < map.numInputs; ++i) {
    map.inputPhysical[static_cast<size_t>(i)] = i;
    map.inputNames.add("Input " + juce::String(i + 1));
  }
  for (int i = 0; i < map.numOutputs; ++i) {
    map.outputPhysical[static_cast<size_t>(i)] = i;
    map.outputNames.add("Output " + juce::String(i + 1));
  }

#if !HEADLESS && JucePlugin_Build_Standalone && ! JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP
  if (wrapperType == wrapperType_Standalone)
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
      if (auto* device = holder->deviceManager.getCurrentAudioDevice()) {
        const auto fill = [](const juce::BigInteger& active, const juce::StringArray& deviceNames,
                             int busChannels, std::array<int, kMaxIoChannels>& physical,
                             juce::StringArray& names) {
          names.clear();
          physical.fill(-1);
          int count = 0;
          for (int bit = active.findNextSetBit(0); bit >= 0 && count < busChannels;
               bit = active.findNextSetBit(bit + 1)) {
            physical[static_cast<size_t>(count++)] = bit;
            names.add(bit < deviceNames.size() ? deviceNames[bit]
                                               : "Channel " + juce::String(bit + 1));
          }
          return count;
        };
        map.numInputs = fill(device->getActiveInputChannels(), device->getInputChannelNames(),
                             map.numInputs, map.inputPhysical, map.inputNames);
        map.numOutputs = fill(device->getActiveOutputChannels(), device->getOutputChannelNames(),
                              map.numOutputs, map.outputPhysical, map.outputNames);
        map.fromDevice = true;
      }
#endif

  return map;
}

// ######################
// UPDATE EQ COEFFICIENTS
// ######################
// Voicing follows the reference NeuralAmpModelerPlugin tone stack
// (BasicNamToneStack, https://github.com/sdatkinson/NeuralAmpModelerPlugin):
// knobs run 0-10 with 5 flat, each band maps linearly to dB with its own
// sweep (bass ±20 dB, mid ±15 dB, treble ±10 dB), and the mid bell widens
// when boosting (Q 0.7 vs 1.5) so pushed mids don't turn honky.
void TONE3000Processor::updateEqCoefficients() {
  const double rate = getSampleRate();
  const float bassDb = 4.0f * (cacheBassTone - 5.0f);
  const float midDb = 3.0f * (cacheMidTone - 5.0f);
  const float trebleDb = 2.0f * (cacheTrebleTone - 5.0f);

  *bassFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(
      rate, clampBelowNyquist(rate, 150.0f), 0.707f,
      juce::Decibels::decibelsToGain(bassDb));
  *midFilter.state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
      rate, clampBelowNyquist(rate, 425.0f), midDb < 0.0f ? 1.5f : 0.7f,
      juce::Decibels::decibelsToGain(midDb));
  *trebleFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
      rate, clampBelowNyquist(rate, 1800.0f), 0.707f,
      juce::Decibels::decibelsToGain(trebleDb));
}

// ######################
// GLOBAL 3-BAND TONE STACK
// ######################
// Runs once per block, after the DC blocker (post-chain). Skipped entirely
// while powered off; filters reset on re-enable so no stale state rings in.
void TONE3000Processor::processToneStack(juce::AudioBuffer<float>& buffer) {
  if (cacheToneEqEnabled) {
    if (!toneEqWasEnabled) {
      bassFilter.reset();
      midFilter.reset();
      trebleFilter.reset();
    }
    if (eqParamsDirty) {
      updateEqCoefficients();
      eqParamsDirty = false;
    }

    juce::dsp::AudioBlock<float> eqBlock(buffer);
    juce::dsp::ProcessContextReplacing<float> eqContext(eqBlock);
    bassFilter.process(eqContext);
    midFilter.process(eqContext);
    trebleFilter.process(eqContext);
  }
  toneEqWasEnabled = cacheToneEqEnabled;
}

// ####################
// UPDATE CACHED PARAMS
// ####################
void TONE3000Processor::updateCachedParameters() {
  constexpr float epsilon = 1e-5f;

  // Plain atomic loads; the string-keyed lookups happened once in
  // resolveParamRefs(). `tone` marks the tone-stack floats whose changes
  // must dirty the EQ coefficients.
  auto updateFloat = [&](float& cached, const std::atomic<float>* param, bool tone = false) {
    const float value = param->load();
    if (std::abs(value - cached) > epsilon) {
      cached = value;
      if (tone)
        eqParamsDirty = true;
    }
  };

  updateFloat(cacheInputLevel, paramRefs.inputLevel);
  updateFloat(cacheOutputLevel, paramRefs.outputLevel);
  updateFloat(cacheOutputBalance, paramRefs.outputBalance);
  updateFloat(cacheSpreadOffset, paramRefs.spreadOffset);
  updateFloat(cacheSpreadWobble, paramRefs.spreadWobble);
  updateFloat(cacheSpreadCrossover, paramRefs.spreadCrossover);
  updateFloat(cacheAlignOffset, paramRefs.alignOffset);
  updateFloat(cacheAlignWobble, paramRefs.alignWobble);
  updateFloat(cacheAlignCrossover, paramRefs.alignCrossover);
  updateFloat(cacheChainPanLeft, paramRefs.chainPanLeft);
  updateFloat(cacheChainPanRight, paramRefs.chainPanRight);
  updateFloat(cacheBassTone, paramRefs.toneBass, true);
  updateFloat(cacheMidTone, paramRefs.toneMid, true);
  updateFloat(cacheTrebleTone, paramRefs.toneTreble, true);
  updateFloat(cacheGateThreshold, paramRefs.gateThreshold);
  updateFloat(cacheGateRelease, paramRefs.gateRelease);
  updateFloat(cacheGateHold, paramRefs.gateHold);
  updateFloat(cacheGateRange, paramRefs.gateRange);
  updateFloat(cacheTargetLoudness, paramRefs.targetLoudness);
  updateFloat(cacheInputCalibrationLevel, paramRefs.inputCalibrationLevel);

  auto loadBool = [](const std::atomic<float>* param) { return param->load() > 0.5f; };
  cacheCalibrateInput = loadBool(paramRefs.calibrateInput);
  cacheGateEnabled = loadBool(paramRefs.gateEnabled);
  cacheToneEqEnabled = loadBool(paramRefs.toneEqEnabled);
  cacheSpreadEnabled = loadBool(paramRefs.spreadEnabled);
  cacheSpreadWobbleEnabled = loadBool(paramRefs.spreadWobbleEnabled);
  cacheSpreadCrossoverEnabled = loadBool(paramRefs.spreadCrossoverEnabled);
  cacheSpreadDiffuseEnabled = loadBool(paramRefs.spreadDiffuseEnabled);
  cacheAlignEnabled = loadBool(paramRefs.alignEnabled);
  cacheAlignWobbleEnabled = loadBool(paramRefs.alignWobbleEnabled);
  cacheAlignCrossoverEnabled = loadBool(paramRefs.alignCrossoverEnabled);
  cacheAlignDiffuseEnabled = loadBool(paramRefs.alignDiffuseEnabled);
  cacheChainSoloLeft = loadBool(paramRefs.chainSoloLeft);
  cacheChainSoloRight = loadBool(paramRefs.chainSoloRight);
  cacheChainInvertLeft = loadBool(paramRefs.chainInvertLeft);
  cacheChainInvertRight = loadBool(paramRefs.chainInvertRight);

  // Pitch shift, in the engine's units. STEP rounds the shift to whole
  // semitones here, so the engine never sees the toggle and a host that
  // automates the knob with STEP on still gets semitones. The choice's raw
  // value is already denormalised (stored as a float); round so it lands
  // exactly on its step. The tonality knob's top end means off.
  cachePitchEnabled = loadBool(paramRefs.pitchEnabled);
  const float semitones = paramRefs.pitchSemitones->load();
  cachePitch.semitones = loadBool(paramRefs.pitchStep) ? std::round(semitones) : semitones;
  const float tonalityHz = paramRefs.pitchTonality->load();
  cachePitch.tonalityHz = tonalityHz < PitchShift::kTonalityOffHz ? tonalityHz : 0.0f;
  cachePitch.window =
      PitchShift::windowFromIndex(static_cast<int>(std::lround(paramRefs.pitchWindow->load())));
}

// See the declaration: the "is there an amp after me" scan of one stretch of
// a row, which the plan executor runs for each segment before it calls the
// block loop below.
int TONE3000Processor::lastActiveNamIndex(const Lane& l, int scanBegin, int scanEnd) noexcept {
  int lastNamIndex = -1;
  const int last = juce::jmin(scanEnd, static_cast<int>(l.size()));
  for (int i = juce::jmax(0, scanBegin); i < last; ++i) {
    const auto& b = l[static_cast<size_t>(i)];
    if (b->type == ChainBlockType::NAM && b->loaded && b->enabled)
      lastNamIndex = i;
  }
  return lastNamIndex;
}

// ##########################
// RT PROCESS A SINGLE CHAIN
// ##########################
// Runs the per-block chain loop on `buffer`. The buffer may be mono (a single side in stereo
// mode) or 1-2 channels (mono mode). All per-channel work is keyed on buffer.getNumChannels().
// Must be called while holding chainMutex.
void TONE3000Processor::processChainOnBuffer(std::vector<std::unique_ptr<ChainBlock>>& blocks,
                                             juce::AudioBuffer<float>& buffer,
                                             juce::AudioBuffer<float>& dryScratch, int beginIdx,
                                             int endIdx, const SegmentCtx& segment) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  beginIdx = juce::jlimit(0, static_cast<int>(blocks.size()), beginIdx);
  endIdx = juce::jlimit(beginIdx, static_cast<int>(blocks.size()), endIdx);

  // segment.lastNamIndex: highest index of an enabled+loaded NAM block on this segment's
  // signal path. Used twice: a stereo IR is only worth processing in true stereo when there
  // is no NAM downstream to collapse the image back to mono, and NAM blocks *before* this
  // index hand off at calibrated output level (see the post-model gain stage below) while the
  // last one keeps loudness normalization. The caller works it out, because the path is more
  // than this range: a branched trunk is still one chain split around the tap, not two
  // chains, so its two segments both answer for the whole lane.

  // What every block reads from the lane and the callback's cached
  // parameters; only the index moves per block.
  BlockProcessContext ctx;
  ctx.dualMono = segment.dualMono;
  ctx.transport = rtTransport;
  ctx.nam.lastNamIndex = segment.lastNamIndex;
  ctx.nam.calibrateInput = cacheCalibrateInput;
  ctx.nam.inputCalibrationLevel = cacheInputCalibrationLevel;
  ctx.nam.targetLoudness = cacheTargetLoudness;
  ctx.nam.workerPool = rtPhasePool;

  for (int idx = beginIdx; idx < endIdx; ++idx) {
    const auto& block = blocks[idx];
    if (block->type == ChainBlockType::INSERT) {
      continue;  // Insert block is pass-through, no audio effect
    }

    // Wet-path fade (see ChainBlock.h): power toggles and pending engine
    // swaps glide the block's wet mix to silence instead of splicing the
    // waveform. Bypass-bound transitions ride wetFadeGain (output crossfades
    // toward dry); engine swaps ride swapWetMuteGain (wet term mutes, the
    // dry share of the user's mix holds and never exposes the un-processed
    // input). A disabled block keeps processing until the glide reaches
    // bypass, then is skipped exactly like before.
    const bool hasLatency = block->dryDelay.delaySamples() > 0;
    const bool swapPending = block->swapFadePending.load();
    const bool muteSwap = swapPending && block->swapMuteWet.load();
    const bool wantsWet = block->enabled && !block->released.load(std::memory_order_relaxed) &&
                          !(swapPending && !muteSwap);
    block->wetFadeGain.setTargetValue(block->loaded && wantsWet ? 1.0f : 0.0f);
    block->swapWetMuteGain.setTargetValue(muteSwap ? 0.0f : 1.0f);

    // Tail spillover (see ChainBlock.h), for the kinds that declare it. A
    // block that is switched off with its spillover switch on is "releasing"
    // until its reported tail has passed: the input closes, the wet term
    // stays up. A pending splice (removal, replacement) is not a release:
    // the block is about to go, so the tail closes with the fade. Kinds
    // without the capability never enter here and take the paths below
    // exactly as before.
    BlockProcessor* const engine = block->processor.get();
    const bool spillCapable = engine != nullptr && engine->supportsSpillover();
    if (spillCapable) {
      const bool on = block->loaded && wantsWet;
      const bool spillOn = engine->spilloverEnabled();
      bool releasing = false;
      if (on) {
        block->spillElapsedSamples = 0;
      } else if (block->loaded && spillOn && !swapPending) {
        // The tail, the wet path's own latency and the input fade, in
        // chain-rate frames. Read every callback: a tail can follow a
        // parameter or the tempo. The reported tail stops at
        // kMaxBlockTailSeconds; a tail still ringing there is not cut but
        // faded, because running out of time closes the tail gain below
        // over its ramp like any other end of a release.
        const double chainRate = chainSampleRate();
        const double tailSamples = (engine->reportedTailSeconds() + kWetFadeSeconds) * chainRate +
                                   static_cast<double>(engine->chainLatencySamples());
        releasing = static_cast<double>(block->spillElapsedSamples) < tailSamples;
      }
      // Held open while the block is on, so a release starts with the wet
      // term already at its level; it has no effect on the mix until the
      // wet fade moves (see the spillover mix below).
      block->spillTailGain.setTargetValue((on && spillOn) || releasing ? 1.0f : 0.0f);
      // The input closes for a release and stays closed once the tail is
      // over; a cut (switch off, or a splice) leaves it to the wet fade.
      block->spillInputGain.setTargetValue(on || !spillOn || swapPending ? 1.0f : 0.0f);
    }
    // Never touched for other kinds: settled at zero, so this is true.
    const bool tailSilent =
        !block->spillTailGain.isSmoothing() && block->spillTailGain.getCurrentValue() <= 0.0f;

    const bool wetSilent = !block->wetFadeGain.isSmoothing() &&
                           block->wetFadeGain.getCurrentValue() <= 0.001f && tailSilent;
    if (wetSilent)
      block->swapFadeDone.store(true);  // a waiting requester may splice now

    if (!block->loaded || (!wantsWet && wetSilent)) {
      // Going idle: let the engine drop what it holds, so a later power-on
      // starts from silence rather than from audio of before. Once per
      // transition, and a no-op for the kinds that keep nothing to replay
      // (the default BlockProcessor::reset).
      if (block->engineHoldsAudio && block->loaded && engine != nullptr) {
        engine->reset();
        block->engineHoldsAudio = false;
      }
      block->spillElapsedSamples = ChainBlock::kSpillDone;

      // Not processing: park the block's meters at the floor. The EQ view can
      // still be open, so keep its analyzer fed with the pass-through audio.
      block->inputMeterDb.store(-60.0f);
      block->outputMeterDb.store(-60.0f);
      if (block->spectrum.isEnabled())
        block->spectrum.pushSamples(buffer.getReadPointer(0),
                                    numChannels > 1 ? buffer.getReadPointer(1) : nullptr,
                                    numSamples, segment.dualMono);
      // A block with latency keeps delaying while it is bypassed: the lane's
      // latency (and what the host compensates) follows the chain's
      // structure, not the power switch, and the bypass fade then lands on
      // exactly this signal.
      if (hasLatency)
        block->dryDelay.process(buffer.getArrayOfWritePointers(), numChannels, numSamples);
      continue;
    }

    // Prepare dry copy before processing for mix (reuse the lane's scratch)
    jassert(dryScratch.getNumChannels() >= numChannels);
    jassert(dryScratch.getNumSamples() >= numSamples);
    dryScratch.copyFrom(0, 0, buffer, 0, 0, numSamples);
    if (numChannels > 1) {
      dryScratch.copyFrom(1, 0, buffer, 1, 0, numSamples);
    }
    // The dry share of the mix waits for the wet path, or the two would
    // comb-filter at any mix below 100%.
    if (hasLatency)
      block->dryDelay.process(dryScratch.getArrayOfWritePointers(), numChannels, numSamples);

    // Per-block input gain (0.5 == unity, ±24 dB), applied after the dry copy
    // so Mix still blends against the untouched signal; this drives the
    // block's DSP harder/softer like a drive control. The block input meter
    // reads the post-gain signal (what the model actually receives).
    {
      const float inputGainDbBlock = (block->inputGainNormalized - 0.5f) * 48.0f;
      block->inputGainSmoother.setTargetValue(juce::Decibels::decibelsToGain(inputGainDbBlock));

      float blockInputPeak = 0.0f;
      auto* left = buffer.getWritePointer(0);
      auto* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
      for (int i = 0; i < numSamples; ++i) {
        const float g = block->inputGainSmoother.getNextValue();
        left[i] *= g;
        blockInputPeak = std::max(blockInputPeak, std::abs(left[i]));
        if (right) {
          right[i] *= g;
          blockInputPeak = std::max(blockInputPeak, std::abs(right[i]));
        }
      }

      // EQ in the PRE position: between the block's input gain and its model,
      // shaping what drives the amp/IR. Skipped entirely when flat/bypassed
      // (or in the default post position; see the POST stage below).
      if (block->eq.isPre() && block->eq.isActive()) {
        block->eq.process(buffer);
        // Re-measure so the input meter still reads what the model receives.
        blockInputPeak = bufferPeak(buffer, numChannels, numSamples);
      }

      const float blockInputDb =
          blockInputPeak > 0.0f ? juce::Decibels::gainToDecibels(blockInputPeak) : -60.0f;
      block->inputMeterDb.store(std::max(-60.0f, blockInputDb));
    }

    // The block's own engine, on the wet path (see BlockProcessor.h), at
    // the rate its kind runs at. What a failure means is the kind's call:
    // an IR that throws carries on, an engine that would throw again on
    // every callback reports Failed.
    if (spillCapable) {
      auto& inputFade = block->spillInputGain;
      if (!block->engineHoldsAudio) {
        // Waking up (first block after going idle, a prepare or an install):
        // the engine is empty, so there is no tail to hold and the two
        // spillover gains can take whatever position makes the way in one
        // single fade. With the switch on that is the input fade, starting
        // where the wet fade stands (closed after idle, open after a
        // prepare) under a wet term already at its level; with it off, the
        // plain wet fade over an open input, as for any other kind.
        const bool spillOn = engine->spilloverEnabled();
        block->spillTailGain.setCurrentAndTargetValue(spillOn ? 1.0f : 0.0f);
        inputFade.setCurrentAndTargetValue(spillOn ? block->wetFadeGain.getCurrentValue() : 1.0f);
        inputFade.setTargetValue(1.0f);
      }
      // The input fade of a release (and of the power-on that follows one):
      // the engine hears silence while its tail rings out. Applied after the
      // meter, which goes on showing what arrives at the block.
      if (inputFade.isSmoothing()) {
        auto* left = buffer.getWritePointer(0);
        auto* right = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
        for (int i = 0; i < numSamples; ++i) {
          const float g = inputFade.getNextValue();
          left[i] *= g;
          if (right)
            right[i] *= g;
        }
      } else if (inputFade.getCurrentValue() <= 0.0f) {
        buffer.clear(0, numSamples);
      }
    }

    if (block->processor != nullptr) {
      ctx.blockIndex = idx;
      block->engineHoldsAudio = true;
      // Two clock reads around the block: its share of one core, averaged
      // (the plan CPU value). Only here, where the block really runs, so a
      // bypassed block keeps the figure it had.
      const juce::int64 cpuStart = juce::Time::getHighResolutionTicks();
      const BlockProcessResult result = block->processor->processInChain(buffer, ctx);
      if (numSamples > 0 && result != BlockProcessResult::Idle) {
        const double seconds = juce::Time::highResolutionTicksToSeconds(
            juce::Time::getHighResolutionTicks() - cpuStart);
        const float share = static_cast<float>(seconds * chainSampleRate() / numSamples);
        const float previous = block->cpuCost.load(std::memory_order_relaxed);
        block->cpuCost.store(block->cpuCostMeasured ? previous + 0.05f * (share - previous) : share,
                             std::memory_order_relaxed);
        block->cpuCostMeasured = true;
      }
      if (result == BlockProcessResult::Idle)
        continue;
      if (result == BlockProcessResult::Failed) {
        // RT-safe failure path, the same for every kind: take the block out
        // of processing (stops it failing every callback) and flag it; the
        // message thread writes the log line and marks the block failed
        // when it next serializes the chain; string building/logging can't
        // run here.
        block->loaded = false;
        block->rtProcessingFailed.store(true);
        bumpChainRevision();  // wake the UI poll so the flag is drained
        buffer.copyFrom(0, 0, dryScratch, 0, 0, numSamples);
        if (numChannels > 1) {
          buffer.copyFrom(1, 0, dryScratch, 1, 0, numSamples);
        }
        continue;
      }
    }

    // EQ in the POST position (default): shapes the wet signal after the
    // model, before the dry/wet mix, so the dry share of Mix passes
    // untouched. Skipped entirely when flat/bypassed (the PRE position ran
    // before the model).
    if (!block->eq.isPre() && block->eq.isActive()) {
      block->eq.process(buffer);
    }

    // Per-block output stage: blend the wet signal with dry, then apply Out
    // Gain (centered at 0.5 == unity, ±24 dB) to the combined result. The
    // knob is the block's output fader, not a wet trim, so it has to move
    // the dry share of Mix too.
    //
    // Short (cab-like) IR blocks pad the wet term by a fixed -18 dB: cab
    // files are peak-normalized to 0 dBFS and spectrally concentrated, far
    // too hot at unity. The pad stays on the wet term, never the blend; at
    // mix < 100% the dry share passes at its natural level. Long
    // (reverb-like) IRs get no pad; unit-energy normalization already puts
    // them at ≈ dry level (see IrBlockProcessor::isLong). The UI knob still
    // reads relative dB (0 at center); the pad is invisible chain gain
    // staging (see gainDbScale in knobScale.ts). Classified at load, so pad
    // steps land while the engine-swap fade holds the wet term silent. The
    // loop only knows it as the kind's wet pad (BlockProcessor::wetPadDb).
    const float irOffsetDb = block->processor != nullptr ? block->processor->wetPadDb() : 0.0f;
    const float cabPadGain = juce::Decibels::decibelsToGain(irOffsetDb);
    const float gainDb = (block->outputGainNormalized - 0.5f) * 48.0f;
    block->outputGainSmoother.setTargetValue(juce::Decibels::decibelsToGain(gainDb));
    block->mixSmoother.setTargetValue(juce::jlimit(0.0f, 1.0f, block->mixNormalized));

    float blockOutputPeak = 0.0f;
    const BlockMixLaw mixLaw = block->mixLaw;
    if (spillCapable || mixLaw != BlockMixLaw::Crossfade) {
      // The general mix, for kinds with spillover or with a mix law of their
      // own (every other kind runs the loop below, as it always has).
      //
      // The mix knob gives a dry and a wet share (BlockMixLaw). The dry
      // coefficient glides from that share to exactly 1 with the bypass
      // fade, and Out Gain to unity in step, as below: so a block that is
      // off, released or not, passes the dry signal at unity. The wet term
      // keeps its level (wet share x Out Gain) for as long as either the
      // block is on (fade) or its tail is held open by spillover (tail),
      // instead of following the fade down. For a kind without spillover the
      // tail gain rests at zero and this is the plain bypass crossfade.
      for (int i = 0; i < numSamples; ++i) {
        const float wetGain = block->swapWetMuteGain.getNextValue() * cabPadGain;
        const float outGain = block->outputGainSmoother.getNextValue();
        const float fade = block->wetFadeGain.getNextValue();
        const float tail = block->spillTailGain.getNextValue();
        const float mix = block->mixSmoother.getNextValue();
        const float postGain = 1.0f + (outGain - 1.0f) * fade;
        const float dryCoef = (1.0f + (blockMixDryShare(mixLaw, mix) - 1.0f) * fade) * postGain;
        const float wetCoef = blockMixWetShare(mixLaw, mix) * outGain *
                              (fade + (1.0f - fade) * tail) * wetGain;
        for (int ch = 0; ch < numChannels; ++ch) {
          float* out = buffer.getWritePointer(ch);
          out[i] = dryScratch.getReadPointer(ch)[i] * dryCoef + out[i] * wetCoef;
          blockOutputPeak = std::max(blockOutputPeak, std::abs(out[i]));
        }
      }
      // The release clock runs while the block is off (it is zeroed while on).
      if (block->spillElapsedSamples < ChainBlock::kSpillDone - numSamples &&
          !(block->loaded && wantsWet))
        block->spillElapsedSamples += numSamples;
    } else {
      for (int i = 0; i < numSamples; ++i) {
        // wetFadeGain rides the mix (bypass-bound glides crossfade toward
        // dry) and glides the post-mix Out Gain to unity in step, so a
        // completed fade lands exactly on the skipped block's pass-through.
        // swapWetMuteGain and the cab pad ride the wet term only, pre-mix
        // (engine swaps dip the wet path to silence without exposing the dry
        // input); see ChainBlock.h.
        const float wetGain = block->swapWetMuteGain.getNextValue() * cabPadGain;
        const float outGain = block->outputGainSmoother.getNextValue();
        const float fade = block->wetFadeGain.getNextValue();
        const float m = block->mixSmoother.getNextValue() * fade;
        const float postGain = 1.0f + (outGain - 1.0f) * fade;
        float wetL = buffer.getWritePointer(0)[i] * wetGain;
        float dryL = dryScratch.getReadPointer(0)[i];
        buffer.getWritePointer(0)[i] = (dryL * (1.0f - m) + wetL * m) * postGain;
        blockOutputPeak = std::max(blockOutputPeak, std::abs(buffer.getWritePointer(0)[i]));
        if (numChannels > 1) {
          float wetR = buffer.getWritePointer(1)[i] * wetGain;
          float dryR = dryScratch.getReadPointer(1)[i];
          buffer.getWritePointer(1)[i] = (dryR * (1.0f - m) + wetR * m) * postGain;
          blockOutputPeak = std::max(blockOutputPeak, std::abs(buffer.getWritePointer(1)[i]));
        }
      }
    }

    // Fade handshake: tell a waiting requester the wet path is fully
    // silent and its change (swap/removal) can splice in silently. Which
    // gain carried the fade depends on the swap's shape (see ChainBlock.h).
    {
      const auto& fadeGain = muteSwap ? block->swapWetMuteGain : block->wetFadeGain;
      // A bypass-shaped fade is only over once a tail held open by
      // spillover has closed too (always true for the other kinds).
      const bool tailClosed = muteSwap || (!block->spillTailGain.isSmoothing() &&
                                           block->spillTailGain.getCurrentValue() <= 0.0f);
      if (block->swapFadePending.load() && !fadeGain.isSmoothing() &&
          fadeGain.getCurrentValue() <= 0.001f && tailClosed)
        block->swapFadeDone.store(true);
    }

    // Block output meter: post EQ + mix + Out Gain, i.e. what this block
    // hands to the next one in the chain.
    const float blockOutputDb =
        blockOutputPeak > 0.0f ? juce::Decibels::gainToDecibels(blockOutputPeak) : -60.0f;
    block->outputMeterDb.store(std::max(-60.0f, blockOutputDb));

    // Feed the EQ editor's analyzer with the block's final output, only while
    // that block's EQ view is actually open in the UI. In dual mono the two
    // channels are different takes, so the analyzer keeps them apart and
    // shows the louder one per bin (see BlockSpectrum::pushSamples).
    if (block->spectrum.isEnabled())
      block->spectrum.pushSamples(buffer.getReadPointer(0),
                                  numChannels > 1 ? buffer.getReadPointer(1) : nullptr,
                                  numSamples, segment.dualMono);
  }
}

// A plan port as an array index. The compiler numbers ports below
// kMaxIoPairs (validatePlan); the clamp keeps a corrupt plan in bounds.
static size_t planPortIndex(std::uint8_t port) noexcept {
  return static_cast<size_t>(juce::jmin<int>(port, kMaxIoPairs - 1));
}

// ##############################
// RT CHAIN STAGE (chain rate)
// ##############################
// The oversampled entry to the chain stage: the callable both invocation
// paths share (the boundary callback and the direct 48k-host path). Raises
// every input port by the current factor, runs processChainStage once on
// the raised blocks, and lowers every output port; with oversampling off
// the ports go to the chain stage as they are. Port p's oversampler serves
// both directions, and its chain-rate block is where the chain stage works
// in place, which for the one main pair is what ChainOversampler::process
// did. Called with chainMutex held.
void TONE3000Processor::processOversampledChainStage(float* const* const* inputs,
                                                     float* const* const* outputs,
                                                     int numFrames) {
  const int factor = chainOversampleFactor.load(std::memory_order_relaxed);
  if (factor <= 1) {
    processChainStage(inputs, outputs, numFrames);
    return;
  }

  const int numUp = juce::jlimit(0, kMaxIoPairs, rtIo.numStrips);
  const int numDown = juce::jlimit(0, kMaxIoPairs, rtIo.numBuses);
  std::array<float* const*, kMaxIoPairs> chainPorts{};
  for (size_t p = 0; p < static_cast<size_t>(kMaxIoPairs); ++p)
    chainPorts[p] = chainOversamplers[p].chainBuffer();

  int chainFrames = numFrames * factor;
  for (int p = 0; p < numUp; ++p)
    chainFrames = chainOversamplers[static_cast<size_t>(p)].processUp(
        inputs[p], chainPorts[static_cast<size_t>(p)], numFrames);

  if (rtIo.legacy) {
    processChainStage(chainPorts.data(), chainPorts.data(), chainFrames);

    for (int p = 0; p < numDown; ++p)
      chainOversamplers[static_cast<size_t>(p)].processDown(chainPorts[static_cast<size_t>(p)],
                                                            outputs[p], chainFrames);
    return;
  }

  // A grid plan reads the strips and writes the buses, and port p's raised
  // block is strip p's input: a bus must not land on it while a row may
  // still have to read it. So the buses are summed in buffers of their own
  // and lowered from there.
  std::array<std::array<float*, 2>, kMaxIoPairs> busChannels{};
  std::array<float* const*, kMaxIoPairs> busPorts{};
  for (size_t b = 0; b < static_cast<size_t>(kMaxIoPairs); ++b) {
    busChannels[b] = {busWork[b].getWritePointer(0), busWork[b].getWritePointer(1)};
    busPorts[b] = busChannels[b].data();
  }
  if (chainFrames > busWork[0].getNumSamples()) {
    // More frames than the buffers were sized for (never, with the slicing
    // in processBlock): silence rather than an overrun.
    unresolvedPlanRefs.fetch_add(1, std::memory_order_relaxed);
    for (int p = 0; p < numDown; ++p)
      for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::clear(outputs[p][ch], numFrames);
    return;
  }

  processChainStage(chainPorts.data(), busPorts.data(), chainFrames);

  for (int p = 0; p < numDown; ++p)
    chainOversamplers[static_cast<size_t>(p)].processDown(busPorts[static_cast<size_t>(p)],
                                                          outputs[p], chainFrames);
}

// The encapsulated side of the chain-domain boundary: the active plan over
// the given ports (see ProcessorPlanExec.cpp). Invoked at the chain rate
// (48 kHz × oversampling factor) via processOversampledChainStage.
// Called with chainMutex held (processBlock takes it).
void TONE3000Processor::processChainStage(float* const* const* inputs,
                                          float* const* const* outputs, int numFrames) {
  const int numUp = juce::jlimit(0, kMaxIoPairs, rtIo.numStrips);
  const int numDown = juce::jlimit(0, kMaxIoPairs, rtIo.numBuses);

  // The boundary hands us distinct input/output buffers; a legacy plan
  // processes in place on the output side, so move the audio there first.
  // On the direct path and behind the oversampler the pointers alias and
  // the copies are skipped.
  if (rtIo.legacy)
    for (int p = 0; p < juce::jmin(numUp, numDown); ++p)
      for (int ch = 0; ch < 2; ++ch) {
        if (outputs[p][ch] != inputs[p][ch])
          std::memcpy(outputs[p][ch], inputs[p][ch],
                      sizeof(float) * static_cast<size_t>(numFrames));
      }

  // The plan was compiled for these lanes under this lock (rebuildTopology),
  // so the two can only disagree if a structural mutator skipped the funnel.
  // Should that ever happen the plan still runs (every range is clamped to
  // the lane it names), and the count says so.
  const bool planIsCurrent = planMatchesLanes();
  jassert(planIsCurrent);
  if (!planIsCurrent)
    stalePlanCallbacks.fetch_add(1, std::memory_order_relaxed);

  PlanStageIo io;
  io.numFrames = numFrames;
  if (rtIo.legacy) {
    for (int p = 0; p < numDown; ++p)
      io.ports[static_cast<size_t>(p)] = outputs[p];
  } else {
    // A grid plan reads the strips' ports and writes the buses' (other
    // buffers, see processOversampledChainStage).
    for (int p = 0; p < numUp; ++p)
      io.ports[static_cast<size_t>(p)] = inputs[p];
    for (int p = 0; p < numDown; ++p)
      io.busPorts[static_cast<size_t>(p)] = outputs[p];

    // A bus no row writes is silence.
    for (int b = 0; b < numDown; ++b) {
      const ProcessingPlan::Bus& bus = activePlan.buses[static_cast<size_t>(b)];
      if (bus.hasWriter)
        continue;
      float* const* channels = outputs[planPortIndex(bus.port)];
      for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::clear(channels[ch], numFrames);
    }

    // Solo acts at the physical outputs: one soloed row silences the sends
    // of the rows that are not soloed and carry no soloed row.
    rtGridSoloMask = gridSoloMask();
  }
  runPlanSeq(activePlan, 0, io);
}

// ##############################
// POST-CHAIN STEREO IMAGE (host rate)
// ##############################
// Runs right after each chain-stage slice, before the downstream stages
// (DC / tone stack / output gain + meters) so they see the real image.
// Order matters: the mode's image engine runs first so it always shapes
// full chain output, then the balance/pan matrix mixes the result.
//  - Mono chain mode: the Spread builds the stereo image from the channel-0
//    chain output (an ADT-style double, see Spread.h). Engage/bypass is its
//    internal ~25 ms crossfade against the untouched buffer; fully skipped
//    once idle. It needs a rig that can reproduce the double (`stereoRig`);
//    otherwise it stays idle no matter what its parameter says, so a preset
//    arriving with spread on plays as the plain mono chain (the UI greys
//    the group out).
//  - Stereo chain mode: Align delays one chain in place via StereoOffset,
//    corrective by default (see StereoOffset.h): 0 ms = identity, all
//    transitions glide through identity; fully skipped once idle. It runs
//    on any rig: on a mono rig it shapes the sum below (aligning two chains
//    matters most when they are about to be summed).
//  - The opposite mode's engine is force-idled (no fade needed): mode
//    switches always ride the chain-edit fade, so the hard stop lands on
//    silence.
//  - Balance + pan (one 2×2 matrix, see imageMatrixGains): the balance trim
//    scales each *chain*, then the constant-power pan blend mixes them
//    across the output bus. The centered/hard-panned default is the
//    identity and skips the loop. On a mono rig with stereo chains the same
//    matrix becomes the mono fold, ½(balL·L + balR·R) onto both channel
//    pointers; solo and polarity keep working inside the sum, the pans are
//    inert. Balance is forced center whenever it can't do anything (mono
//    chain with neither a running spread nor dual mono), so a leftover Bal
//    setting can't skew a plain mono bus, matching the UI hiding the knob.
void TONE3000Processor::processImageStage(float* chL, float* chR, int numFrames,
                                          bool stereoRig) {
  // Allocation-free stereo view over the two chain channels, for the
  // engines' AudioBuffer interfaces.
  float* imageChannels[2] = {chL, chR};
  juce::AudioBuffer<float> image(imageChannels, 2, numFrames);

  // Spread builds a stereo double from channel 0; in dual mono the chain
  // already outputs two real channels, so it stays idle (the parameter keeps
  // its value and the UI dims the group, as on a mono rig).
  const bool spreadActive = cacheSpreadEnabled && stereoRig && !rtDualMono;
  const bool monoFold = rtStereoChains && !stereoRig;

  if (rtStereoChains) {
    spread.forceIdle();

    // Auto-align probe capture: the raw chain outputs BEFORE the align
    // delay and the image matrix's polarity flips, so the measurement is
    // the chains' absolute misalignment and relative polarity, independent
    // of the current corrections (a second run measures the total, not the
    // residual). Zero work unless a probe is running.
    autoOffset.captureChainOutputs(chL, chR, numFrames);

    stereoOffset.setTarget(
        StereoOffsetParams::fromNormalized(cacheAlignOffset, cacheAlignWobble,
                                           cacheAlignCrossover, cacheAlignWobbleEnabled,
                                           cacheAlignCrossoverEnabled, cacheAlignDiffuseEnabled),
        cacheAlignEnabled);
    if (stereoOffset.isRunning())
      stereoOffset.process(image);
  } else {
    stereoOffset.forceIdle();
    spread.setTarget(
        SpreadParams::fromNormalized(cacheSpreadOffset, cacheSpreadWobble, cacheSpreadCrossover,
                                     cacheSpreadWobbleEnabled, cacheSpreadCrossoverEnabled,
                                     cacheSpreadDiffuseEnabled),
        spreadActive);
    if (spread.isRunning())
      spread.process(image);
  }

  // Auto balance listening tap: the raw chain outputs, before the balance
  // and pan gains, so the measurement is the chains' true mismatch. It must
  // sit pre-pan: post-pan the two channels converge as the pans approach
  // center even when the chains are badly mismatched, which would starve
  // the measurement. Zero work unless armed.
  if (autoBalanceState.load(std::memory_order_acquire) ==
      static_cast<int>(AutoBalanceState::Listening))
    runAutoBalanceStage(image, numFrames);

  // The matrix has work only when two real output channels exist, or when
  // folding stereo chains onto a mono rig (mono chain mode there needs no
  // matrix: channel 0 already is the output). Solo and polarity key on the
  // chain mode, not the rig: two chains are worth auditioning and
  // re-polarizing inside the sum too. All four gains are smoothed so knob
  // moves AND the solo/invert/fold gating glide instead of stepping (pop).
  if (stereoRig || monoFold) {
    // Balance trims the two things on the bus against each other: the two
    // chains, Spread's two sides, or dual mono's two voices (the matrix is
    // a diagonal L/R tilt there, like the Spread case: pan inactive).
    const bool applyBalance = rtStereoChains || spreadActive || rtDualMono;
    const auto g = imageMatrixGains(rtStereoChains && !monoFold, monoFold,
                                    applyBalance ? cacheOutputBalance : 0.5f,
                                    cacheChainPanLeft, cacheChainPanRight,
                                    rtStereoChains && cacheChainSoloLeft,
                                    rtStereoChains && cacheChainSoloRight,
                                    rtStereoChains && cacheChainInvertLeft,
                                    rtStereoChains && cacheChainInvertRight);
    imageGainLtoL.setTargetValue(g.lToL);
    imageGainLtoR.setTargetValue(g.lToR);
    imageGainRtoL.setTargetValue(g.rToL);
    imageGainRtoR.setTargetValue(g.rToR);

    const bool smoothing = imageGainLtoL.isSmoothing() || imageGainLtoR.isSmoothing() ||
                           imageGainRtoL.isSmoothing() || imageGainRtoR.isSmoothing();
    const bool identity = std::abs(g.lToL - 1.0f) < 1.0e-4f &&
                          std::abs(g.rToR - 1.0f) < 1.0e-4f && std::abs(g.lToR) < 1.0e-4f &&
                          std::abs(g.rToL) < 1.0e-4f;
    if (smoothing || !identity) {
      for (int i = 0; i < numFrames; ++i) {
        const float ll = imageGainLtoL.getNextValue();
        const float lr = imageGainLtoR.getNextValue();
        const float rl = imageGainRtoL.getNextValue();
        const float rr = imageGainRtoR.getNextValue();
        const float chainL = chL[i];
        const float chainR = chR[i];
        chL[i] = chainL * ll + chainR * rl;
        chR[i] = chainL * lr + chainR * rr;
      }
    }
  }
}

// ##############################
// INPUT STRIPS AND OUTPUT BUSES (host rate)
// ##############################
// processBlock is the input strips, the chain stage and the output strips.
// A legacy rig has one strip and
// one bus, both the host buffer's first two channels in place, and for
// those every operation below is the one processBlock always ran, in the
// same order on the same samples: the reference renders are bit-exact
// across this split and must stay so.

static float peakToMeterDb(float peak) {
  return peak > 0.0f ? std::max(-60.0f, juce::Decibels::gainToDecibels(peak)) : -60.0f;
}

static bool samePlanStrip(const ProcessingPlan::Strip& a, const ProcessingPlan::Strip& b) noexcept {
  return a.in.type == b.in.type && a.in.channel == b.in.channel && a.in.stereo == b.in.stereo &&
         a.in.fold == b.in.fold && juce::exactlyEqual(a.in.trimDb, b.in.trimDb) &&
         a.in.gate == b.in.gate &&
         a.in.pitch == b.in.pitch && a.port == b.port && a.padHostSamples == b.padHostSamples &&
         a.hostMainPair == b.hostMainPair;
}

static bool samePlanBus(const ProcessingPlan::Bus& a, const ProcessingPlan::Bus& b) noexcept {
  return a.pair == b.pair && a.port == b.port && a.mainStrip == b.mainStrip &&
         a.hostMainPair == b.hostMainPair;
}

bool TONE3000Processor::planIoMatchesRt() const noexcept {
  if (activePlan.numStrips != rtIo.numStrips || activePlan.numBuses != rtIo.numBuses ||
      activePlan.legacy != rtIo.legacy)
    return false;
  for (int s = 0; s < rtIo.numStrips; ++s)
    if (!samePlanStrip(activePlan.strips[static_cast<size_t>(s)], rtIo.strips[static_cast<size_t>(s)]))
      return false;
  for (int b = 0; b < rtIo.numBuses; ++b)
    if (!samePlanBus(activePlan.buses[static_cast<size_t>(b)], rtIo.buses[static_cast<size_t>(b)]))
      return false;
  return true;
}

void TONE3000Processor::adoptPlanIo() noexcept {
  rtIo.strips = activePlan.strips;
  rtIo.buses = activePlan.buses;
  rtIo.numStrips = juce::jlimit(0, kMaxIoPairs, activePlan.numStrips);
  rtIo.numBuses = juce::jlimit(0, kMaxIoPairs, activePlan.numBuses);
  rtIo.legacy = activePlan.legacy;
  rtIo.pitchStrip = -1;
  for (int s = rtIo.numStrips - 1; s >= 0; --s)
    if (rtIo.strips[static_cast<size_t>(s)].in.pitch)
      rtIo.pitchStrip = s;
}

void TONE3000Processor::adoptPlanIoOnAudioThread() noexcept {
  adoptPlanIo();
  // Cheap resets only: zeroing filter state and clearing preallocated
  // rings. A legacy rig never comes here after its first callback (its
  // strip and bus are always the same main pair).
  for (auto& strip : inputStrips) {
    strip.gate.reset();
    strip.gateWasEnabled = true;
    strip.clearPad();
  }
  for (auto& bus : outputBuses)
    bus.dcBlocker.reset();
}

void TONE3000Processor::bindHostIo(juce::AudioBuffer<float>& buffer, HostIo& io) noexcept {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // The main pair is the host buffer itself while that has no more than two
  // channels, so whatever a strip does to it is literally what processBlock
  // did to the buffer; on a wider buffer it is a view of the first two.
  io.mainPair = &buffer;
  if (numChannels > 2) {
    io.mainPairView.setDataToReferTo(buffer.getArrayOfWritePointers(), 2, numSamples);
    io.mainPair = &io.mainPairView;
  }
  bool mainPairInPlace = false;

  for (int s = 0; s < rtIo.numStrips; ++s) {
    const ProcessingPlan::Strip& strip = rtIo.strips[static_cast<size_t>(s)];
    io.strips[static_cast<size_t>(s)] = nullptr;
    if (strip.hostMainPair) {
      io.strips[static_cast<size_t>(s)] = io.mainPair;
      mainPairInPlace = true;
      continue;
    }

    juce::AudioBuffer<float>& work = inputStrips[static_cast<size_t>(s)].work;
    if (work.getNumChannels() < 2 || work.getNumSamples() < numSamples) {
      // The host handed over more than it promised and the working pair
      // cannot hold it: the strip sits this callback out, silent. (The
      // main pair is in place and never comes here.)
      work.clear();
      continue;
    }
    float* const* dest = work.getArrayOfWritePointers();
    for (int ch = 0; ch < 2; ++ch) {
      // A mono source goes onto both channels, like a host feeding a mono
      // source to a stereo bus, and so does the first channel of a pair
      // whose second the device lacks (a mono device). A strip whose first
      // channel is missing is silence.
      int source = -1;
      if (strip.in.type == RowInput::Type::Device) {
        source = deviceChannels.inputBufferChannel(strip.in.channel + (strip.in.stereo ? ch : 0));
        if (ch == 1 && (source < 0 || source >= numChannels))
          source = deviceChannels.inputBufferChannel(strip.in.channel);
        if (deviceChannels.inputBufferChannel(strip.in.channel) < 0)
          source = -1;
      }
      if (source >= 0 && source < numChannels)
        juce::FloatVectorOperations::copy(dest[ch], buffer.getReadPointer(source), numSamples);
      else
        juce::FloatVectorOperations::clear(dest[ch], numSamples);
    }
    io.stripViews[static_cast<size_t>(s)].setDataToReferTo(dest, 2, numSamples);
    io.strips[static_cast<size_t>(s)] = &io.stripViews[static_cast<size_t>(s)];
  }

  for (int b = 0; b < rtIo.numBuses; ++b) {
    const ProcessingPlan::Bus& bus = rtIo.buses[static_cast<size_t>(b)];
    io.buses[static_cast<size_t>(b)] = nullptr;
    if (bus.hostMainPair) {
      io.buses[static_cast<size_t>(b)] = io.mainPair;
      mainPairInPlace = true;
      continue;
    }
    juce::AudioBuffer<float>& work = outputBuses[static_cast<size_t>(b)].work;
    // Silent until the chain stage fills it, so a callback that skips the
    // stage outputs silence here too.
    work.clear();
    if (work.getNumChannels() < 2 || work.getNumSamples() < numSamples)
      continue;
    io.busViews[static_cast<size_t>(b)].setDataToReferTo(work.getArrayOfWritePointers(), 2,
                                                         numSamples);
    io.buses[static_cast<size_t>(b)] = &io.busViews[static_cast<size_t>(b)];
  }

  // The inputs have been taken; every host channel that is not an in-place
  // pair is an output from here on. In a legacy rig nothing writes them:
  // inputs past the first two are ignored and outputs past the first two
  // are silent.
  for (int ch = mainPairInPlace ? 2 : 0; ch < numChannels; ++ch)
    buffer.clear(ch, 0, numSamples);
}

void TONE3000Processor::scatterOutputBuses(juce::AudioBuffer<float>& buffer,
                                           const HostIo& io) noexcept {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();
  for (int b = 0; b < rtIo.numBuses; ++b) {
    const ProcessingPlan::Bus& bus = rtIo.buses[static_cast<size_t>(b)];
    const juce::AudioBuffer<float>* view = io.buses[static_cast<size_t>(b)];
    if (bus.hostMainPair || view == nullptr)
      continue;
    // Copied: every bus has its own pair (a pair the device lacks was put
    // on Out 1/2's bus when the plan was compiled).
    const int destL = deviceChannels.outputBufferChannel(2 * bus.pair);
    const int destR = deviceChannels.outputBufferChannel(2 * bus.pair + 1);
    const bool hasL = destL >= 0 && destL < numChannels;
    const bool hasR = destR >= 0 && destR < numChannels;
    if (hasL && hasR) {
      juce::FloatVectorOperations::copy(buffer.getWritePointer(destL), view->getReadPointer(0),
                                        numSamples);
      juce::FloatVectorOperations::copy(buffer.getWritePointer(destR), view->getReadPointer(1),
                                        numSamples);
    } else if (hasL) {
      // One channel for a pair: the mono fold a host applies, half of
      // (L + R). A centred signal passes exactly.
      const float* l = view->getReadPointer(0);
      const float* r = view->getReadPointer(1);
      float* out = buffer.getWritePointer(destL);
      for (int i = 0; i < numSamples; ++i)
        out[i] = (l[i] + r[i]) * 0.5f;
    }
  }
}

// Input fold-down of the legacy main pair, so everything downstream (meters,
// tuner, chains) sees the effective source:
// - Mono source (a mono input device in the standalone app, or a main bus
//   with one input channel on a wider buffer): signal only arrives on
//   channel 0, so mirror it.
// - Input mode L/R on a stereo source: duplicate the chosen channel onto
//   both, exactly like a host feeding a mono source to a stereo bus.
// - Stereo on a mono chain: sum to mono, ½(L+R), the host's own fold law
//   (a mono source on a stereo track, L == R, passes bit-identically; a
//   real stereo source reaches the chain whole instead of left-only).
//   Dual Mono folds the same way whenever it can't engage (mono rig; see
//   dualMonoEngaged), so the two modes only differ when both takes can
//   actually be processed and heard. With stereo chains neither folds:
//   channel 0 feeds the Left chain and channel 1 the Right.
TONE3000Processor::InputFold TONE3000Processor::legacyInputFold() const noexcept {
  if (standaloneMonoInput.load() || rtMonoMainInput)
    return InputFold::MirrorFirst;
  const auto mode = static_cast<InputMode>(inputMode.load());
  const bool sumToMono =
      !stereoEnabled.load() &&
      (mode == InputMode::Stereo || (mode == InputMode::DualMono && !rtDualMono));
  switch (mode) {
    case InputMode::Left: return InputFold::MirrorFirst;
    case InputMode::Right: return InputFold::MirrorSecond;
    case InputMode::Stereo:
    case InputMode::DualMono: return sumToMono ? InputFold::SumToMono : InputFold::None;
  }
  return InputFold::None;
}

void TONE3000Processor::runInputStrip(int index, juce::AudioBuffer<float>& buffer) {
  const ProcessingPlan::Strip& plan = rtIo.strips[static_cast<size_t>(index)];
  InputStripState& state = inputStrips[static_cast<size_t>(index)];
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();
  // The first strip is the faceplate's: its meters, and the tuner's source.
  const bool firstStrip = index == 0;

  // Fold, up front. The legacy main pair follows the input mode (see
  // legacyInputFold); any other strip folds as its row asks, and its
  // channels were gathered from the device already (see bindHostIo).
  if (numChannels > 1) {
    const InputFold fold = plan.hostMainPair
                               ? legacyInputFold()
                               : plan.in.fold == RowInput::Fold::SumToMono ? InputFold::SumToMono
                                                                            : InputFold::None;
    switch (fold) {
      case InputFold::None: break;
      case InputFold::MirrorFirst: buffer.copyFrom(1, 0, buffer, 0, 0, numSamples); break;
      case InputFold::MirrorSecond: buffer.copyFrom(0, 0, buffer, 1, 0, numSamples); break;
      case InputFold::SumToMono:
        buffer.applyGain(0, 0, numSamples, 0.5f);
        buffer.addFrom(0, 0, buffer, 1, 0, numSamples, 0.5f);
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
        break;
    }
  }

  // #########################
  // Input gain + noise gate
  // #########################

  // Input gain (level ±24 dB), constant across the block. Computed up front
  // so the meters can show the post-gain level without a second pass over
  // the samples. The level is the rig's; a row adds its own trim.
  float inputGain = mainStageGain(cacheInputLevel);
  if (!juce::exactlyEqual(plan.in.trimDb, 0.0f))
    inputGain *= juce::Decibels::decibelsToGain(plan.in.trimDb);

  // Per-channel input meters: raw peaks scaled by the input gain, so the
  // meter tracks the knob. Pre-gate on purpose: a closed gate would
  // otherwise read as a dead input. Mono sources report the same level on
  // both channels.
  {
    float peakL = 0.0f, peakR = 0.0f;
    const auto* l = buffer.getReadPointer(0);
    for (int i = 0; i < numSamples; ++i)
      peakL = std::max(peakL, std::abs(l[i]));
    if (numChannels > 1) {
      const auto* r = buffer.getReadPointer(1);
      for (int i = 0; i < numSamples; ++i)
        peakR = std::max(peakR, std::abs(r[i]));
    } else {
      peakR = peakL;
    }
    (firstStrip ? inputMeterLevelL : state.meterL).store(peakToMeterDb(peakL * inputGain));
    (firstStrip ? inputMeterLevelR : state.meterR).store(peakToMeterDb(peakR * inputGain));
  }

  // Feed the tuner from the raw input (pre-gain, pre-gate) while the tuner
  // screen is open. Channel 0 only: guitar sources are mono, and mixing
  // channels risks phase cancellation.
  if (firstStrip && tuner.isEnabled())
    tuner.pushSamples(buffer.getReadPointer(0), numSamples);

  // Apply the input gain (vectorized).
  for (int ch = 0; ch < numChannels; ++ch)
    buffer.applyGain(ch, 0, numSamples, inputGain);

  // Noise gate, post input gain so the threshold knob's dB meaning matches
  // the level heading into the chain. Envelope/hysteresis gate (NoiseGate.h);
  // re-enabling resets the detector so a stale envelope never gates the
  // first block. The parameters are the rig's; a row can switch its strip's
  // gate off.
  const bool gateEnabled = cacheGateEnabled && plan.in.gate;
  if (gateEnabled) {
    if (!state.gateWasEnabled)
      state.gate.reset();
    state.gate.setParams({cacheGateThreshold, cacheGateRelease, cacheGateHold, cacheGateRange});
    state.gate.process(buffer);
  }
  state.gateWasEnabled = gateEnabled;

  // Pitch shift (PitchShift.h): shifts the instrument before the chain, so
  // the amp sees a down-tuned (or whammy-bent) guitar. After the gate so it
  // decides on the real transients; before the auto-align probe below,
  // whose sweep must never be shifted. Runs while powered and through the
  // power-off blend; once that lands it is a bit-exact, zero-latency
  // passthrough. The latency report rides the power parameter
  // (updateLatency), not this path. There is one shifter, so one strip
  // runs it.
  if (index == rtIo.pitchStrip) {
    pitchShift.setEnabled(cachePitchEnabled);
    if (pitchShift.isRunning()) {
      pitchShift.setParams(cachePitch);
      pitchShift.process(buffer);
    }
  } else {
    // This strip does not run the shifter: wait as long as the shifter
    // makes a strip late, so the rows stay in step and the one latency the
    // host is told (updateLatency, which follows the power parameter and
    // not the plan) holds for every row. Also when no strip runs it at all
    // (no row asks for it, or the one that does is on an input this device
    // lacks): what is reported and what is heard must never differ. Zero (a
    // pass-through) while the shifter is off. A legacy rig's one strip is
    // always the shifter's and never comes here.
    state.processPad(buffer, stripPadSamples.load(std::memory_order_relaxed));
  }

  // #########################
  // Auto-align probe injection (see AutoOffset.h): while a measurement is
  // running, both chains eat the pre-generated sweep instead of the
  // instrument. Injected downstream of the whole input stage, so the meters
  // and gate keep tracking the live signal but none of it reaches the
  // measurement; the output is muted for the duration (the probe mute stage
  // of the main output strip). Stereo chain mode only (any rig: on a mono
  // buffer the scratch mirror in the chain-stage loop feeds the probe to
  // the Right lane); losing the mode mid-run cancels (the atomic flip is
  // audio-thread safe). A legacy construct, so the legacy main pair only.
  // #########################
  if (plan.hostMainPair) {
    if (autoOffset.state() != AutoOffset::State::Idle && !stereoEnabled.load())
      autoOffset.cancel();
    if (autoOffset.renderProbeInput(buffer.getWritePointer(0), numSamples) && numChannels > 1)
      buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
  } else if (firstStrip) {
    // A grid has neither the probe nor the image stage the two legacy
    // measurements tap. One still in flight when the rig became a grid
    // (a preset, an undo, a conversion) would never finish, and the
    // auto-align's output mute would never lift: end both here.
    cancelLegacyMeasurements();
  }
}

// The gain is one smoother for the whole rig, so it is advanced once per
// callback and the values kept: every bus is then multiplied by the very
// same per-sample gains, however many buses there are.
TONE3000Processor::EditFade TONE3000Processor::advanceChainEditFade(int numSamples) noexcept {
  EditFade fade;
  const bool editPending = chainEditFadePending.load();
  chainEditFadeGain.setTargetValue(editPending ? 0.0f : 1.0f);
  if (chainEditFadeGain.isSmoothing()) {
    fade.state = EditFade::State::Ramp;
    fade.rampSamples = juce::jmin(numSamples, static_cast<int>(chainEditFadeRamp.size()));
    for (int i = 0; i < fade.rampSamples; ++i)
      chainEditFadeRamp[static_cast<size_t>(i)] = chainEditFadeGain.getNextValue();
    // A callback longer than the array (a host exceeding its promised block
    // size) has reached the target by the array's end, because the array
    // holds more values than the glide has steps. The rest of the callback
    // is at the target, which is what the smoother would keep returning.
    jassert(fade.rampSamples == numSamples || !chainEditFadeGain.isSmoothing());
    fade.tailGain = chainEditFadeGain.getCurrentValue();
  } else if (editPending && chainEditFadeGain.getCurrentValue() <= 0.001f) {
    fade.state = EditFade::State::Silent;
  }
  return fade;
}

void TONE3000Processor::runOutputStrip(int index, juce::AudioBuffer<float>& buffer,
                                       const EditFade& fade) {
  const ProcessingPlan::Bus& plan = rtIo.buses[static_cast<size_t>(index)];
  OutputBusState& state = outputBuses[static_cast<size_t>(index)];
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // ##########
  // DC blocker
  // ##########
  {
    // ProcessorDuplicator runs an independent filter instance per channel, so a
    // single duplicator covers the whole (mono or stereo) buffer. Running a
    // second one here would high-pass every channel twice.
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    state.dcBlocker.process(context);
  }

  // ##########
  // Chain-edit fade (see ChainEditFade): structural edits that can't be
  // expressed as one block's wet fade (reorder, cross-lane move, preset /
  // undo restores) glide the whole chain output to silence, splice the edit
  // in between callbacks, and glide back. Idle cost: one branch (the gain
  // was advanced once for all buses, see advanceChainEditFade).
  //
  // The tap deliberately sits AFTER the image stage and DC blocker:
  //  - Image stage: its hard stops on mode switches (forceIdle) land
  //    upstream of this gain, so they still splice into silence.
  //  - DC blocker: NAM models can idle at a DC offset, and a restored rig's
  //    blocks fade in while the chain is held muted. With the tap upstream
  //    of the blocker, the blocker would settle to zero state during the
  //    hold and the new rig's DC would step through it at release, an
  //    audible thump on every preset/undo switch. Downstream of the
  //    blocker, the blocker tracks the live chain (including its DC)
  //    throughout the hold, so release ramps in an already-centered signal.
  // ##########
  if (fade.state == EditFade::State::Ramp) {
    const float* ramp = chainEditFadeRamp.data();
    for (int ch = 0; ch < numChannels; ++ch) {
      float* samples = buffer.getWritePointer(ch);
      for (int i = 0; i < fade.rampSamples; ++i)
        samples[i] *= ramp[i];
      for (int i = fade.rampSamples; i < numSamples; ++i)
        samples[i] *= fade.tailGain;
    }
  } else if (fade.state == EditFade::State::Silent) {
    // Fully faded: hold silence until the editor thread finishes its splice.
    buffer.clear();
    chainEditFadeDone.store(true);
  }

  if (!plan.mainStrip) {
    // Any other bus stops here: the tone stack and the master level belong
    // to the main output. Its own meters only.
    float peakL = 0.0f, peakR = 0.0f;
    const auto* l = buffer.getReadPointer(0);
    const auto* r = numChannels > 1 ? buffer.getReadPointer(1) : l;
    for (int i = 0; i < numSamples; ++i) {
      peakL = std::max(peakL, std::abs(l[i]));
      peakR = std::max(peakR, std::abs(r[i]));
    }
    state.meterL.store(peakToMeterDb(peakL));
    state.meterR.store(peakToMeterDb(peakR));
    return;
  }

  // ##########
  // EQ section (global 3-band tone stack), post-chain.
  // ##########
  processToneStack(buffer);

  // ##########
  // Auto-align probe mute: fades the output before the probe starts, holds
  // silence through capture and analysis, ramps back after the result is
  // applied (see AutoOffset.h). Sits before the output stage so the meters
  // show the mute honestly. Idle cost: one atomic load.
  // ##########
  autoOffset.applyOutputGain(buffer);

  // ###########
  // Output gain (level ±24 dB, same on both channels; the balance trim
  // lives in the post-chain image matrix, pre-pan). Smoothed so knob
  // moves glide instead of stepping once per block. Per-channel output
  // meters ride the same pass.
  // ###########
  {
    outputGainSmoother.setTargetValue(mainStageGain(cacheOutputLevel));

    float peakL = 0.0f, peakR = 0.0f;
    auto* l = buffer.getWritePointer(0);
    auto* r = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;
    for (int i = 0; i < numSamples; ++i) {
      const float g = outputGainSmoother.getNextValue();
      l[i] *= g;
      peakL = std::max(peakL, std::abs(l[i]));
      if (r) {
        r[i] *= g;
        peakR = std::max(peakR, std::abs(r[i]));
      }
    }
    if (numChannels < 2)
      peakR = peakL;
    outputMeterLevelL.store(peakToMeterDb(peakL));
    outputMeterLevelR.store(peakToMeterDb(peakR));
  }
}

// ################
// RT PROCESS BLOCK
// ################
void TONE3000Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  // Times this whole callback against its real-time budget (the CPU readout).
  juce::AudioProcessLoadMeasurer::ScopedTimer loadTimer(loadMeasurer, buffer.getNumSamples());

  // Mapped MIDI first, so parameter moves (bypass stomps, expression sweeps)
  // land before this block's cached-parameter refresh below.
  midiMapper.processMidi(midi);

  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  if (numSamples <= 0 || numChannels <= 0) {
    DBG("Invalid buffer: samples=" << numSamples << ", channels=" << numChannels);
    buffer.clear();
    outputMeterLevelL.store(-60.0f);
    outputMeterLevelR.store(-60.0f);
    return;
  }

  // A capture run (CaptureAudio.h) listens to the input first, hands the rig
  // silence, and owns the output when the callback is done, whichever way it
  // leaves. Idle, this is one atomic read.
  CaptureAudio::BlockScope captureScope(captureAudio, buffer);

  // The raw input, before anything touches it, for a guitar profile take
  // that is being recorded (nothing happens otherwise).
  inputRecorder.capture(buffer);

  updateCachedParameters();

  // Tempo and transport for this callback's blocks (see BlockTransport): the
  // host's playhead when it reports one, else the rig's own tempo with no
  // transport. Read once here; every block sees the same copy.
  {
    BlockTransport transport;
    transport.bpm = internalTempoBpm.load(std::memory_order_relaxed);
    transport.samplePosition = rtProcessedSamples;
    juce::Optional<double> hostSeconds, hostPpq, hostBarStart;
    if (auto* playHead = getPlayHead()) {
      if (const auto position = playHead->getPosition()) {
        if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0) {
          transport.bpm = *bpm;
          transport.fromHost = true;
          transport.playing = position->getIsPlaying();
          if (const auto signature = position->getTimeSignature()) {
            transport.timeSigNumerator = signature->numerator;
            transport.timeSigDenominator = signature->denominator;
          }
          if (const auto samples = position->getTimeInSamples())
            transport.samplePosition = *samples;
          hostSeconds = position->getTimeInSeconds();
          hostPpq = position->getPpqPosition();
          hostBarStart = position->getPpqPositionOfLastBarStart();
        }
      }
    }
    // The musical position (see BlockTransport): the host's where it gives
    // one, else what the sample position and the tempo imply.
    const double rate = hostSampleRate > 0.0 ? hostSampleRate : kChainBaseSampleRate;
    transport.timeSeconds =
        hostSeconds.hasValue() ? *hostSeconds : static_cast<double>(transport.samplePosition) / rate;
    transport.ppqPosition =
        hostPpq.hasValue() ? *hostPpq : transport.timeSeconds * transport.bpm / 60.0;
    if (hostBarStart.hasValue()) {
      transport.ppqLastBarStart = *hostBarStart;
    } else {
      const double barLength = transport.timeSigNumerator * 4.0 /
                               juce::jmax(1, transport.timeSigDenominator);
      transport.ppqLastBarStart =
          barLength > 0.0 ? std::floor(transport.ppqPosition / barLength) * barLength : 0.0;
    }
    rtProcessedSamples += numSamples;
    rtTransport = transport;
    lastEffectiveBpm.store(transport.bpm, std::memory_order_relaxed);
    lastTempoFromHost.store(transport.fromHost, std::memory_order_relaxed);
  }

  // Heartbeat for isAudioActive(): fade handshakes skip their bounded waits
  // when no callbacks are running (nothing is audible then).
  lastAudioCallbackMs.store(juce::Time::currentTimeMillis());

  // Dual mono is resolved once per callback, here, so the fold and the chain
  // stage below always agree on it (a mode change landing between the two
  // reads could otherwise fold the input and then run two voices on it).
  rtDualMono = numChannels > 1 && dualMonoEngaged();

  // A plan with other strips or buses (a grid edit, a conversion) is taken
  // over here when the lock is free, before the strips run, so the callback
  // already runs the new layout whole. When the lock is busy the chain
  // stage below notices instead and spends one silent callback on it.
  {
    juce::ScopedTryLock adoptLock(chainMutex);
    if (adoptLock.isLocked() && !planIoMatchesRt())
      adoptPlanIoOnAudioThread();
  }

  // A grid rig's strips and buses work in pairs of their own, as long as
  // the block size prepareToPlay was given. A host that hands over more
  // gets it in slices of that size. (The legacy main pair is in place on
  // the host buffer and takes any length; it slices the chain stage only,
  // as it always has.)
  if (!rtIo.legacy && maxBlockSize > 0 && numSamples > maxBlockSize) {
    for (int offset = 0; offset < numSamples; offset += maxBlockSize) {
      juce::AudioBuffer<float> slice(buffer.getArrayOfWritePointers(), numChannels, offset,
                                     juce::jmin(maxBlockSize, numSamples - offset));
      processHostBlock(slice);
    }
    return;
  }
  processHostBlock(buffer);
}

void TONE3000Processor::processHostBlock(juce::AudioBuffer<float>& buffer) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();

  // ####################
  // INPUT STRIPS (host rate): fold, input gain, meters, tuner, gate, pitch
  // ####################
  // The strips and buses are the ones the last chain stage saw the plan
  // with (rtIo); a legacy rig has the main pair of each, in place on the
  // buffer's first two channels.
  HostIo io;
  bindHostIo(buffer, io);
  for (int s = 0; s < rtIo.numStrips; ++s)
    if (auto* strip = io.strips[static_cast<size_t>(s)])
      runInputStrip(s, *strip);

  // ####################
  // MODULAR CHAIN PROCESSING (chain domain: 48 kHz × OS factor, see ChainDomain.h)
  // ####################
  // Runs under chainMutex, but the render thread must never *block* behind a
  // long splice. Preset/undo restores hold chainMutex on the message thread
  // while they decode megabytes of embedded model bytes; a blocking lock here
  // stalls the CoreAudio render thread for 100+ ms, which overloads the
  // driver and can restart the device (observed in the field: repeated
  // prepareToPlay/releaseResources cycles and a fallback to the OS default
  // device right after heavy preset loads). So: try the lock first. If it's
  // contended while the chain-edit fade has fully landed (pending && done ⇒
  // the tap below outputs silence no matter what the chain produces), skip
  // the stage wait-free: inaudible, and the splice can take as long as it
  // needs. Contention outside a landed fade is ordinary and brief (UI state
  // pulls, engine installs), so fall back to the blocking lock as before.

  // The rig can reproduce a stereo image: two buffer channels AND a detected
  // stereo output (a standalone mono output device still hands us a stereo
  // buffer but plays only channel 0). Gates Spread, and flips the image
  // matrix into the mono fold for stereo chains (see processImageStage).
  const bool stereoRig =
      numChannels >= 2 && stereoOutputDetected.load(std::memory_order_relaxed);

  const auto runChainStage = [&] {
    // The input strips above ran the strips this callback started with. A
    // plan with other strips or buses can only have arrived inside a
    // chain-edit fade: take them over for the output strips and the
    // callbacks that follow, and let this one be silent.
    if (!planIoMatchesRt()) {
      adoptPlanIoOnAudioThread();
      buffer.clear();
      bindHostIo(buffer, io);
      return;
    }
    // The boundary is opened for the plan's ports off the audio thread
    // (prepareToPlay). Never run it with others.
    if (chainBoundary != nullptr && (chainBoundary->numUpPorts() != rtIo.numStrips ||
                                     chainBoundary->numDownPorts() != rtIo.numBuses)) {
      jassertfalse;
      buffer.clear();
      bindHostIo(buffer, io);
      return;
    }

    // Stereo chains follow the mode alone: on a mono rig both lanes still
    // run (the Right lane on the scratch channel) and the image stage sums
    // them, so a two-chain rig is heard in full instead of half.
    rtStereoChains = stereoEnabled.load();
    rtChainChannels = juce::jmin(numChannels, 2);
    // rtDualMono was resolved up front, with the input fold.

    // One multi-core resolution per callback (under chainMutex): the
    // setting and live workers. The NAM phase forks need nothing more. A
    // plan fork additionally wants work on at least two of its sides
    // (otherwise the handoff costs more than the empty loop it would hide),
    // which runPlanFork checks where it forks: for stereo chains that is
    // both lanes, for branched routing the trunk's blocks after the tap and
    // the branch lane.
    const bool multiCore =
        multiCoreEnabled.load(std::memory_order_relaxed) && rtWorkerPool.isRunning();
    rtPhasePool = multiCore ? &rtWorkerPool : nullptr;

    // One channel pair per port, for the slice at hand. A pair in place on
    // the host buffer is a port always two channels wide, so a mono host
    // buffer gets the scratch as its second channel: silent in mono chain
    // mode, the Right lane's working channel with stereo chains (mirrored
    // input in, lane output out, consumed by the fold below). A pair that
    // sits the callback out runs on its (silent) working buffer from the
    // start: no slice is longer than that.
    const auto pairChannels = [&](bool hostMainPair, juce::AudioBuffer<float>* bound,
                                  juce::AudioBuffer<float>& work,
                                  int offset) -> std::array<float*, 2> {
      if (hostMainPair)
        return {io.mainPair->getWritePointer(0) + offset,
                io.mainPair->getNumChannels() > 1 ? io.mainPair->getWritePointer(1) + offset
                                                  : chainScratchChannel.getWritePointer(0)};
      if (bound == nullptr)
        return {work.getWritePointer(0), work.getWritePointer(1)};
      return {bound->getWritePointer(0) + offset, bound->getWritePointer(1) + offset};
    };
    std::array<std::array<float*, 2>, kMaxIoPairs> upChannels{}, downChannels{};
    std::array<float* const*, kMaxIoPairs> upPorts{}, downPorts{};
    for (size_t p = 0; p < static_cast<size_t>(kMaxIoPairs); ++p) {
      upPorts[p] = upChannels[p].data();
      downPorts[p] = downChannels[p].data();
    }

    // Hosts occasionally exceed the block size they promised in prepareToPlay.
    // Feed the chain stage in prepared-size slices so the boundary's internal
    // buffers (and the chain-domain scratch) can never overflow; a single
    // pass in the normal case.
    const int maxSlice = juce::jmax(1, maxBlockSize);
    for (int offset = 0; offset < numSamples; offset += maxSlice) {
      const int sliceLen = juce::jmin(maxSlice, numSamples - offset);

      for (int s = 0; s < rtIo.numStrips; ++s) {
        const ProcessingPlan::Strip& strip = rtIo.strips[static_cast<size_t>(s)];
        std::array<float*, 2>& channels = upChannels[planPortIndex(strip.port)];
        channels = pairChannels(strip.hostMainPair, io.strips[static_cast<size_t>(s)],
                                inputStrips[static_cast<size_t>(s)].work, offset);
        if (strip.hostMainPair && io.mainPair->getNumChannels() == 1 && rtStereoChains)
          std::memcpy(channels[1], channels[0], sizeof(float) * static_cast<size_t>(sliceLen));
      }
      for (int b = 0; b < rtIo.numBuses; ++b) {
        const ProcessingPlan::Bus& bus = rtIo.buses[static_cast<size_t>(b)];
        downChannels[planPortIndex(bus.port)] =
            pairChannels(bus.hostMainPair, io.buses[static_cast<size_t>(b)],
                         outputBuses[static_cast<size_t>(b)].work, offset);
      }

      if (chainBoundary != nullptr)
        chainBoundary->process(upPorts.data(), downPorts.data(), sliceLen,
                               [this](float* const* const* chainIns,
                                      float* const* const* chainOuts, int chainFrames) {
                                 processOversampledChainStage(chainIns, chainOuts, chainFrames);
                               });
      else
        processOversampledChainStage(upPorts.data(), downPorts.data(), sliceLen);

      // Image stage per slice: on a mono host buffer the scratch channel
      // only holds the Right lane's output for this slice, so it must be
      // folded before the next slice reuses it. A legacy construct, on the
      // legacy plan's main output only.
      for (int b = 0; b < rtIo.numBuses; ++b) {
        const ProcessingPlan::Bus& bus = rtIo.buses[static_cast<size_t>(b)];
        if (!rtIo.legacy || !bus.mainStrip)
          continue;
        const std::array<float*, 2>& channels =
            downChannels[planPortIndex(bus.port)];
        processImageStage(channels[0], channels[1], sliceLen, stereoRig);
      }
    }
  };

  {
    juce::ScopedTryLock tryLock(chainMutex);
    if (tryLock.isLocked()) {
      runChainStage();
    } else if (chainEditFadePending.load() && chainEditFadeDone.load()) {
      // A splice owns the lock and the output is held at silence: hand the
      // downstream stages a cleared buffer instead of raw input (the tap
      // would zero it anyway, but the DC blocker sits before the tap).
      buffer.clear();
    } else {
      juce::ScopedLock lock(chainMutex);
      runChainStage();
    }
  }

  // ####################
  // OUTPUT STRIPS (host rate): DC blocker, chain-edit fade, and on the main
  // output the tone stack, the auto-align mute, output gain and meters
  // ####################
  const EditFade fade = advanceChainEditFade(numSamples);
  for (int b = 0; b < rtIo.numBuses; ++b)
    if (auto* bus = io.buses[static_cast<size_t>(b)])
      runOutputStrip(b, *bus, fade);
  scatterOutputBuses(buffer, io);
}

// ##################
// ENABLE EDITOR / UI
// ##################
bool TONE3000Processor::hasEditor() const {
  return true;
}

// ##############
// CREATE EDITOR
// ##############
juce::AudioProcessorEditor* TONE3000Processor::createEditor() {
#if !HEADLESS
  // The standalone application opens the Pedaleira UI; the plugin formats
  // keep upstream's editor for now (PedaleiraEditor.h).
#if !JUCE_IOS
  if (pedaleira::ui::PedaleiraEditor::wanted(*this))
    return new pedaleira::ui::PedaleiraEditor(*this);
#endif
  return new t3k::ui::NativeEditor(*this);
#else
  return nullptr;
#endif
}

EngineCommands& TONE3000Processor::commandLayer() {
  JUCE_ASSERT_MESSAGE_THREAD
  if (appCommands == nullptr)
    appCommands = std::make_unique<EngineCommands>(*this);
  return *appCommands;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  auto* processor = new TONE3000Processor();
  // The standalone application is the one place with a single rig that is
  // "the rig being played", so it is where agents can connect: the command
  // server starts here when the user has switched it on (off by default; see
  // AgentHub::applyServerSetting). A plugin instance in a host does not
  // listen. Not on iOS, where nothing could connect to it.
#if !JUCE_IOS
  if (processor->wrapperType == juce::AudioProcessor::wrapperType_Standalone &&
      juce::MessageManager::existsAndIsCurrentThread())
    processor->commandLayer().agents().applyServerSetting();
#endif
  return processor;
}

// #########################
// AUTO BALANCE (one-shot chain energy match)
// #########################
// The workflow is "click =, play for a couple of seconds, done": we measure
// the user's real playing rather than injecting a test signal, because NAM
// chains are nonlinear (a noise burst at an arbitrary level says little about
// how the chains compare under a real pick attack) and a burst would be
// audible at the output. Continuous AGC is deliberately avoided; it would
// chase the player's dynamics instead of correcting a static chain mismatch.
// The tap sits on the raw chain outputs, before the balance/pan matrix, so
// the measured mismatch (and the balance value it produces) is independent
// of the current pan positions.

namespace {
// Signal gate: blocks whose loudest channel is below this RMS don't count
// toward the measurement, so silence between phrases can't dilute it.
constexpr double kAutoBalanceFloorRms = 3.16e-3;  // -50 dBFS
constexpr double kAutoBalanceMeasureSeconds = 2.0;
constexpr double kAutoBalanceTimeoutSeconds = 15.0;
}  // namespace

void TONE3000Processor::startAutoBalance() {
  // It listens at the legacy image stage, which a grid does not run.
  if (isGridMode())
    return;
  // Reset is safe from the message thread: the audio thread only touches the
  // accumulators while the state is Listening, and the release-store below
  // publishes the zeroed accumulators together with the state flip.
  autoBalanceSumL = 0.0;
  autoBalanceSumR = 0.0;
  autoBalanceSamples.store(0, std::memory_order_relaxed);
  autoBalanceElapsed.store(0, std::memory_order_relaxed);
  autoBalanceState.store(static_cast<int>(AutoBalanceState::Listening),
                         std::memory_order_release);
}

// See the declaration. Atomic flips only: any thread.
void TONE3000Processor::cancelLegacyMeasurements() noexcept {
  if (autoOffset.state() != AutoOffset::State::Idle)
    autoOffset.cancel();
  int listening = static_cast<int>(AutoBalanceState::Listening);
  autoBalanceState.compare_exchange_strong(listening, static_cast<int>(AutoBalanceState::Idle),
                                           std::memory_order_acq_rel);
}

void TONE3000Processor::cancelAutoBalance() {
  autoBalanceState.store(static_cast<int>(AutoBalanceState::Idle), std::memory_order_release);
}

// Audio thread, on the raw chain outputs (pre-balance/pan), only while
// Listening.
void TONE3000Processor::runAutoBalanceStage(const juce::AudioBuffer<float>& buffer,
                                            int numSamples) {
  autoBalanceElapsed.fetch_add(numSamples, std::memory_order_relaxed);

  if (buffer.getNumChannels() >= 2) {
    const float* l = buffer.getReadPointer(0);
    const float* r = buffer.getReadPointer(1);
    double sumL = 0.0, sumR = 0.0;
    for (int i = 0; i < numSamples; ++i) {
      sumL += static_cast<double>(l[i]) * l[i];
      sumR += static_cast<double>(r[i]) * r[i];
    }

    const double blockRms = std::sqrt(std::max(sumL, sumR) / std::max(1, numSamples));
    if (blockRms > kAutoBalanceFloorRms) {
      autoBalanceSumL += sumL;
      autoBalanceSumR += sumR;
      autoBalanceSamples.fetch_add(numSamples, std::memory_order_relaxed);
    }
  }

  const auto needed =
      static_cast<juce::int64>(kAutoBalanceMeasureSeconds * hostSampleRate);
  if (autoBalanceSamples.load(std::memory_order_relaxed) >= needed) {
    const double energyL = std::max(autoBalanceSumL, 1.0e-12);
    const double energyR = std::max(autoBalanceSumR, 1.0e-12);
    // Positive = left chain louder. The balance trim corrects up to ±12 dB
    // per chain (±24 dB relative), so clamp to what the knob can express.
    autoBalanceMatchedDb = static_cast<float>(
        juce::jlimit(-24.0, 24.0, 10.0 * std::log10(energyL / energyR)));
    autoBalanceState.store(static_cast<int>(AutoBalanceState::Measured),
                           std::memory_order_release);
  } else if (autoBalanceElapsed.load(std::memory_order_relaxed) >
             static_cast<juce::int64>(kAutoBalanceTimeoutSeconds * hostSampleRate)) {
    autoBalanceState.store(static_cast<int>(AutoBalanceState::TimedOut),
                           std::memory_order_release);
  }
}

// Message thread (UI poll). Applying the result here, not on the audio
// thread, keeps setValueNotifyingHost off the RT path and on the thread
// hosts expect parameter gestures from.
juce::var TONE3000Processor::pollAutoBalance() {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  const auto state =
      static_cast<AutoBalanceState>(autoBalanceState.load(std::memory_order_acquire));

  switch (state) {
    case AutoBalanceState::Listening: {
      const auto needed =
          static_cast<juce::int64>(kAutoBalanceMeasureSeconds * hostSampleRate);
      const auto samples = autoBalanceSamples.load(std::memory_order_relaxed);
      obj->setProperty("state", "listening");
      obj->setProperty("progress",
                       juce::jlimit(0.0, 1.0, static_cast<double>(samples) /
                                                  static_cast<double>(std::max<juce::int64>(1, needed))));
      break;
    }
    case AutoBalanceState::Measured: {
      // diff dB → knob position: the trim applies ∓diff/2 to the Left chain
      // and ±diff/2 to the Right, and the knob maps (value - 0.5) · 24 to
      // the per-chain trim dB (see balanceChainGain).
      const float diffDb = autoBalanceMatchedDb;
      const float balance = juce::jlimit(0.0f, 1.0f, 0.5f + diffDb / 48.0f);
      if (auto* param = parameters.getParameter("outputBalance")) {
        param->beginChangeGesture();
        param->setValueNotifyingHost(balance);
        param->endChangeGesture();
      }
      autoBalanceState.store(static_cast<int>(AutoBalanceState::Idle),
                             std::memory_order_release);
      obj->setProperty("state", "done");
      obj->setProperty("matchedDb", diffDb);
      juce::Logger::writeToLog("[AutoBalance] Matched L/R (diff " +
                               juce::String(diffDb, 2) + " dB)");
      break;
    }
    case AutoBalanceState::TimedOut:
      autoBalanceState.store(static_cast<int>(AutoBalanceState::Idle),
                             std::memory_order_release);
      obj->setProperty("state", "timeout");
      break;
    case AutoBalanceState::Idle:
      obj->setProperty("state", "idle");
      break;
  }
  return juce::var(obj.get());
}

// #########################
// AUTO ALIGN (probe-based chain time alignment, stereo chain mode)
// #########################
// One [=] press runs a deterministic internal sweep through both chains
// with the output muted for under half a second; the whole measurement
// (probe schedule, capture, GCC-PHAT estimation) lives in AutoOffset
// (AutoOffset.h). The processor's share is the probe injection / capture
// tap / mute stage in processBlock, and applying the result to the host
// parameters here on the message thread.

namespace {
// Rejection gate: peak sharpness is the PHAT-native quality metric and sits
// far above this on every healthy run (5+ measured across real NAM/IR rig
// pairs; junk in-window peaks land near 1), so a failure means the capture
// was disturbed (a chain edit spliced mid-probe, a silently broken chain)
// or the true misalignment is beyond the ±24 ms the knob can express.
// Reject and log instead of setting a junk offset. The raw-waveform
// confidence is deliberately NOT gated: two differently voiced rigs
// legitimately read low there even when perfectly aligned (0.14 measured);
// it rides along in the log and poll payload as a diagnostic.
constexpr float kAutoOffsetMinSharpness = 2.0f;
// Lags under this are already aligned for any practical purpose (well under
// a sample's worth of imaging); don't power Align on over nothing.
constexpr float kAutoOffsetSilentMs = 0.05f;
}  // namespace

void TONE3000Processor::startAutoOffset() {
  // Offline renders must never print the probe's silence into the bounce,
  // and the measurement needs two live chains.
  if (isNonRealtime() || !stereoEnabled.load())
    return;
  autoOffset.arm();
}

void TONE3000Processor::cancelAutoOffset() { autoOffset.cancel(); }

// Message thread (UI poll). The analysis (a one-shot FFT over the capture)
// also runs here, never on the audio thread; the output stays muted until
// resume(), so the parameters below are always in place before the unmute.
juce::var TONE3000Processor::pollAutoOffset() {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();

  switch (autoOffset.state()) {
    case AutoOffset::State::FadeOut:
    case AutoOffset::State::Probing:
    case AutoOffset::State::Tail:
    case AutoOffset::State::Analyzing:
      obj->setProperty("state", "listening");
      obj->setProperty("progress", static_cast<double>(autoOffset.progress()));
      break;
    case AutoOffset::State::Captured: {
      const auto result = autoOffset.analyze();
      if (result.peakSharpness < kAutoOffsetMinSharpness) {
        autoOffset.resume();
        obj->setProperty("state", "timeout");
        // The metrics ride along for diagnostics (log scrapes, tests); the
        // UI only reads the state.
        obj->setProperty("confidence", result.confidence);
        obj->setProperty("peakSharpness", result.peakSharpness);
        juce::Logger::writeToLog("[AutoOffset] Rejected: confidence " +
                                 juce::String(result.confidence, 3) + ", sharpness " +
                                 juce::String(result.peakSharpness, 2) +
                                 " (disturbed capture or misalignment out of range)");
        break;
      }
      // ms → knob position, the StereoOffsetParams::fromNormalized inverse:
      // (value - 0.5) · 2 · 24 ms, positive = right chain delayed.
      const float norm = juce::jlimit(
          0.0f, 1.0f, 0.5f + result.offsetMs / (2.0f * StereoOffsetParams::kMaxOffsetMs));
      if (auto* param = parameters.getParameter("alignOffset")) {
        param->beginChangeGesture();
        param->setValueNotifyingHost(norm);
        param->endChangeGesture();
      }
      // Power Align on when there's a real correction to hear. An
      // effectively-zero result still rewrites the time (clearing a stale
      // knob value) but leaves the power switch alone.
      if (std::abs(result.offsetMs) >= kAutoOffsetSilentMs) {
        if (auto* param = parameters.getParameter("alignEnabled")) {
          param->beginChangeGesture();
          param->setValueNotifyingHost(1.0f);
          param->endChangeGesture();
        }
      }
      // Polarity: the tap sits upstream of the image matrix, so
      // result.inverted is the chains' absolute relative polarity and the
      // flip parameters must end up XOR-matching it. Toggling only the
      // Right flip preserves an absolute both-chain flip the user may have
      // set against the rest of the mix.
      bool polarityFlipped = false;
      auto* invLeft = parameters.getParameter("chainInvertLeft");
      auto* invRight = parameters.getParameter("chainInvertRight");
      if (invLeft != nullptr && invRight != nullptr) {
        const bool invertedNow = (invLeft->getValue() > 0.5f) != (invRight->getValue() > 0.5f);
        polarityFlipped = result.inverted != invertedNow;
        if (polarityFlipped) {
          invRight->beginChangeGesture();
          invRight->setValueNotifyingHost(invRight->getValue() > 0.5f ? 0.0f : 1.0f);
          invRight->endChangeGesture();
        }
      }
      // Everything is applied; let the output ramp back in.
      autoOffset.resume();
      obj->setProperty("state", "done");
      obj->setProperty("matchedMs", result.offsetMs);
      obj->setProperty("polarityFlipped", polarityFlipped);
      juce::Logger::writeToLog("[AutoOffset] Aligned chains (offset " +
                               juce::String(result.offsetMs, 3) + " ms, confidence " +
                               juce::String(result.confidence, 3) + ", sharpness " +
                               juce::String(result.peakSharpness, 1) +
                               (polarityFlipped ? ", polarity flipped)" : ")"));
      break;
    }
    case AutoOffset::State::RampBack:
    case AutoOffset::State::Idle:
      obj->setProperty("state", "idle");
      break;
  }
  return juce::var(obj.get());
}

// #########################
// DIAGNOSTIC LOG FILE
// #########################
// Mirrors juce::FileLogger::createDefaultAppLogger's path so the logger and the
// UI's copy/reveal actions always target the same file.
juce::File TONE3000Processor::getLogFile() {
  return juce::FileLogger::getSystemLogFileFolder()
      .getChildFile(kAppDataFolderName)
      .getChildFile(juce::String(kAppDataFolderName) + ".log");
}
