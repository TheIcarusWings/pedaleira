#pragma once
// amp/AmpDescription.h: an amp, described as data.
//
// An AmpDescription is everything the amp framework needs to build a guitar
// amp from its schematic: the input network, the gain stages in order (tube,
// plate load, cathode, grid and coupling networks, the gain pot with its
// bright capacitor), the voicings (interstage capacitors, the supply), the
// tone stack, the power amp (tube, topology, feedback network, presence and
// depth networks, transformer corners, supply), the front panel (which knob
// moves which part, with what taper and range) and metadata (name, author,
// the licence of the description itself, where each value came from).
//
// It is read from a JSON file by parseAmpDescription (AmpDescription.cpp); the
// format is documented in docs/amp-framework.md and described by
// docs/amp-description.schema.json. This header is plain C++ and std, with no
// JSON and no JUCE, so the structure can be built by hand too.
//
// Everything in an AmpDescription is a number or a short string. It is built
// once, off the audio thread, and is read-only afterwards; the audio engine
// (AmpCircuit.h and the amp processor) copies what it needs in prepare and
// does not allocate.
//
// Units: ohms, farads, henries, volts, amps, seconds, Hz.
#include <string>
#include <vector>

#include "amp/CouplingFilters.h"
#include "amp/GainStages.h"
#include "amp/PowerAmp.h"
#include "amp/SagModel.h"
#include "amp/ToneStack.h"
#include "amp/TriodeStage.h"

namespace amp {

/** The format version this build reads. */
constexpr int kAmpDescriptionVersion = 1;

constexpr int kMaxStages = 6;
constexpr int kMaxLinks = kMaxStages - 1;
constexpr int kMaxVoicings = 3;

/** How the linear networks around each triode are solved (see GainStages.h). */
enum class StageModel {
  Classic,  // one-pole filters and a static waveshaper (ClassicGainStage)
  Wdf,      // wave digital filter trees and a root solve (WdfGainStage)
};

/** Where a value came from. "schematic": read off the schematic or a datasheet.
    "measured": measured on the real amp or part. "fitted": no component behind
    it; chosen to make the model match a measurement or a capture. */
enum class Provenance { Schematic, Measured, Fitted };

/** The knobs and switches a panel can carry. The ids are fixed: they are the
    parameter ids of the amp.custom kind. */
enum class PanelId { Gain, Bass, Middle, Treble, Presence, Depth, Master, Bright, Voicing };
constexpr int kPanelControls = 9;

const char* panelIdName(PanelId id) noexcept;

/** One control on the panel. A control that is not present is not on the amp:
    it is held at the middle of its travel. */
struct PanelControl {
  bool present{false};
  std::string label;
  /** The part the knob moves, as a path in the description (validated: it must
      be the one that control can drive). */
  std::string target;
  /** position = low + (high - low) * knob ^ taper, with the knob 0..1. This is
      how the knob is wired to the part (a log-taper pot on a linear part, say);
      the gain pot's own resistance law is gain_pot.taper. */
  float taper{1.0f};
  float low{0.0f};
  float high{1.0f};
};

/** What kind of amp it is, for browsing: the family a block picker files it
    under. None for a description that does not say (a user's own). */
enum class AmpFamily { None, HighGain, Clean, Crunch };

/** The id used in files ("high_gain") and the name a player reads ("High gain"). */
const char* ampFamilyId(AmpFamily family) noexcept;
const char* ampFamilyName(AmpFamily family) noexcept;

// ---- The faceplate: how the panel is drawn --------------------------------
// None of this touches the sound. It says how a front end draws the panel's
// controls (docs/amp-framework.md, "The faceplate"); the controls themselves
// are the PanelControls above.

/** Where the nameplate sits on the faceplate. */
enum class Nameplate { Left, Right, Top, Bottom };
/** The cap of a knob: a round cap with a pointer line, a wedge pointer, a
    chicken head that turns with the knob, or a fader (a vertical slider). */
enum class KnobCap { Round, Pointer, ChickenHead, Fader };
/** The pattern of the signature strip on the nameplate. */
enum class StripPattern { None, Slash, Dots, Wave, Cells, Zigzag, Chevron, Bells, Arcs, Crosses, Ticks, Flames, Rings, Steps, Diamonds, Ripple };

/** The id used in files, and the parse in the other direction (false: unknown). */
const char* nameplateId(Nameplate value) noexcept;
const char* knobCapId(KnobCap value) noexcept;
const char* stripPatternId(StripPattern value) noexcept;

/** One box of the faceplate: a title and the controls in it, in order. */
struct PanelGroup {
  std::string title;
  std::vector<PanelId> controls;
  /** Columns the knobs of the box are laid out in; 0 = one column per knob. */
  int columns{0};
};

struct Faceplate {
  /** True when the description gave a faceplate; false: the defaults below
      (resolveFaceplate), which a front end draws as a plain panel. */
  bool styled{false};
  /** The accent colour as "#RRGGBB" (upper case); empty = the front end's own. */
  std::string accent;
  Nameplate nameplate{Nameplate::Left};
  /** The short style tag on the nameplate ("US Lead"), and the line under the name. */
  std::string tag;
  std::string character;
  KnobCap knobCap{KnobCap::Round};
  StripPattern strip{StripPattern::None};
  /** The boxes, left to right. */
  std::vector<PanelGroup> groups;
};

/** The origin of one value, for the values the description gave a note or a
    non-default source for (and for every "fitted" one). */
struct ValueNote {
  std::string path;
  Provenance source{Provenance::Schematic};
  std::string note;
  double value{0.0};
};

struct StageDescription {
  std::string name;
  /** "12AX7", "12AT7", "12AU7" or "custom". */
  std::string tubeType{"12AX7"};
  GainStageSpec spec;
};

struct InputDescription {
  /** Input coupling capacitor and the first grid leak. */
  CouplingNetwork network{6.8e-9f, 0.0f, 1.0e6f};
  float gridStopperOhms{68.0e3f};
  float millerFarads{160.0e-12f};
  /** The guitar's source impedance, for the grid charge path. */
  float sourceOhms{10.0e3f};
};

struct VoicingDescription {
  std::string name;
  /** Interstage coupling capacitor of link k (stage k to stage k + 1); 0 =
      the stage's own value. */
  float couplingFarads[kMaxLinks]{};
  SagSupply supply;
  /** "solid_state", "tube" or "none": what the supply is rectified with. The
      source resistance already contains its effect; the field is a label. */
  std::string rectifier{"solid_state"};
};

struct ToneStackDescription {
  /** "tmb": treble, middle, bass (ToneStack.h). */
  std::string type{"tmb"};
  ToneStackParts parts;
};

struct PowerDescription {
  /** "EL34", "6L6", "EL84", "6V6", "KT88", "6550" or "custom": a label for the
      output tubes; their behaviour is in the knee and crossover values. */
  std::string tube{"EL34"};
  /** "push_pull_ab" (crossover character), "push_pull_a" (none) or
      "solid_state" (no crossover, linear up to a hard clip at the rail). */
  std::string topology{"push_pull_ab"};
  PowerAmpParams params;
  /** The output transformer's low corner (the output DC blocker). The high
      corner is params.transformerLowpassHz. */
  float transformerLowCornerHz{12.0f};
  /** A tone cut network ahead of the output tubes (CouplingFilters.h), or none. */
  bool hasToneCut{false};
  ToneCutParams toneCut;
  /** The supply before a voicing's overrides. */
  SagSupply supply;
  std::string rectifier{"tube"};
};

/** Values with no component behind them: they tie the model to levels. */
struct CalibrationDescription {
  /** Digital full scale at the first grid, in volts. */
  float inputFullScaleVolts{2.0f};
  /** Volts at the tone stack output that drive the power stage to full swing. */
  float powerDriveVolts{40.0f};
  /** How much the interstage corners rise with gain (factor 1 + rise at full gain). */
  float couplingGainRise{1.8f};
  /** Scale from the power stage's swing to digital. */
  float outputTrim{0.2f};
  float controlSmoothSeconds{0.02f};
};

/** The cab an amp is meant to be heard through: the settings of a cab.model
    block (docs/cab-framework.md). Optional, and only a suggestion for whoever
    builds a rig around the amp: nothing of the amp's own sound depends on
    it. The strings are the cab.model choices as the format writes them. */
struct DefaultCab {
  bool present{false};
  /** The speaker's name, as its description gives it ("Granite"). */
  std::string speaker;
  /** "1x12", "2x12", "4x12", "1x10" or "2x10". */
  std::string size{"4x12"};
  /** "closed" or "open". */
  std::string back{"closed"};
  /** "dynamic", "ribbon" or "condenser". */
  std::string mic{"dynamic"};
  float positionPercent{35.0f};
  float distanceCm{2.5f};
  float angleDeg{0.0f};
  float roomPercent{10.0f};
  float lowCutHz{30.0f};
  float highCutHz{16000.0f};
};

struct AmpDescription {
  // ---- Metadata -----------------------------------------------------------
  int version{kAmpDescriptionVersion};
  std::string name;
  std::string author;
  /** The licence of this description file (an SPDX identifier or a name). */
  std::string licence;
  std::string summary;
  /** Which family a block picker files the amp under. */
  AmpFamily family{AmpFamily::None};

  StageModel model{StageModel::Classic};

  InputDescription input;
  int numStages{0};
  StageDescription stages[kMaxStages];
  int numVoicings{0};
  VoicingDescription voicings[kMaxVoicings];
  /** Index of the voicing a new amp starts on. */
  int defaultVoicing{0};
  ToneStackDescription tone;
  /** The effects loop is a placeholder: it is read and kept, and the signal
      does not pass through one. */
  bool effectsLoopPresent{false};
  PowerDescription power;
  CalibrationDescription calibration;
  PanelControl panel[kPanelControls];
  /** The cab the amp is meant for; `present` false when the file names none. */
  DefaultCab defaultCab;
  /** How the panel is drawn (panel.faceplate); `faceplate.styled` says whether the file gave one. */
  Faceplate faceplate;

  /** The stage that carries the gain pot, or -1. */
  int potStage{-1};

  /** True when the presence control drives the tone cut (target
      "power_amp.tone_cut") instead of the presence network, which is then held
      at the middle of its travel. */
  bool presenceDrivesToneCut{false};

  /** Values with an explicit provenance or note, and every fitted value. */
  std::vector<ValueNote> notes;

  const PanelControl& control(PanelId id) const noexcept { return panel[static_cast<int>(id)]; }
};

/** The faceplate to draw: the description's own when it has one, otherwise
    a plain default (round caps, no strip, the nameplate on the left, a
    Preamp box and a Power box). Either way every control the panel has is in
    exactly one group, in order. */
Faceplate resolveFaceplate(const AmpDescription& description);

/** Reads a description from JSON text. On failure returns false and `error`
    says what is wrong and where (a path such as "stages[1].coupling"). Not for
    the audio thread. */
bool parseAmpDescription(const std::string& text, AmpDescription& out, std::string& error);

}  // namespace amp
