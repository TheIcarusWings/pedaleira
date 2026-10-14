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

struct AmpDescription {
  // ---- Metadata -----------------------------------------------------------
  int version{kAmpDescriptionVersion};
  std::string name;
  std::string author;
  /** The licence of this description file (an SPDX identifier or a name). */
  std::string licence;
  std::string summary;

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

/** Reads a description from JSON text. On failure returns false and `error`
    says what is wrong and where (a path such as "stages[1].coupling"). Not for
    the audio thread. */
bool parseAmpDescription(const std::string& text, AmpDescription& out, std::string& error);

}  // namespace amp
