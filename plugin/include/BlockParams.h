#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// How a block parameter's value is to be read. All of them travel as one
// float in the parameter's own unit: a Bool is 0 or 1, a Choice is the index
// into `choices`.
enum class BlockParamType { Float, Bool, Choice };

// One parameter a block kind declares. The table is the kind's contract with
// everything outside the audio path: the command layer lists and validates
// against it, saved state keys values by `id`, and a UI can build controls
// from it without knowing the kind. Ids are therefore permanent; a rename is
// a state migration (see BlockProcessor::kindStateVersion).
struct BlockParamSpec {
  // Stable id, unique within the kind and never one of the common block
  // parameter ids (see commonBlockParams in BlockKinds.h).
  juce::String id;
  juce::String name;
  // Display unit of the real value ("dB", "ms", "Hz", "%"); empty when the
  // number speaks for itself.
  juce::String unit;
  BlockParamType type{BlockParamType::Float};
  float min{0.0f};
  float max{1.0f};
  float defaultValue{0.0f};
  // Taper hint for controls and automation (juce::NormalisableRange's skew:
  // below 1 gives the low end more travel). Values are always set and stored
  // in real units, so it never changes what a number means.
  float skew{1.0f};
  // Grid the value snaps to; 0 = continuous.
  float step{0.0f};
  // Labels of a Choice parameter, in index order.
  juce::StringArray choices;

  // A host or a MIDI map may drive it. Off for values whose change is not
  // click-free (algorithm switches, buffer sizes).
  bool automatable{true};
  // Moving it changes BlockProcessor::tailSeconds, so the reported tail is
  // recomputed once the gesture settles.
  bool affectsTail{false};
  // Moving it changes BlockProcessor::latencySamples; same deferral, and the
  // change is spliced in under the chain-edit fade.
  bool affectsLatency{false};
  // The value can follow the tempo in BlockProcessContext::transport instead
  // of being read as an absolute time.
  bool tempoSync{false};

  static BlockParamSpec floatParam(const juce::String& id, const juce::String& name,
                                   const juce::String& unit, float min, float max,
                                   float defaultValue);
  static BlockParamSpec boolParam(const juce::String& id, const juce::String& name,
                                  bool defaultValue);
  static BlockParamSpec choiceParam(const juce::String& id, const juce::String& name,
                                    const juce::StringArray& choices, int defaultIndex);

  BlockParamSpec withSkew(float newSkew) const;
  BlockParamSpec withStep(float newStep) const;
  BlockParamSpec withAffectsTail() const;
  BlockParamSpec withAffectsLatency() const;
  BlockParamSpec withTempoSync() const;
  BlockParamSpec notAutomatable() const;

  // `value` brought into range and onto the parameter's grid. A non-finite
  // value becomes the default. No allocation: safe wherever a set is.
  float clamp(float value) const noexcept;

  // { id, name, type, unit?, min, max, default, skew?, step?, choices?,
  //   automatable, affectsTail, affectsLatency, tempoSync }
  juce::var toVar() const;
};

using BlockParamTable = std::vector<BlockParamSpec>;

// The id of the switch a kind with tail spillover carries (see
// spilloverParam). Reserved: a parameter of that id declares the capability,
// so it can be nothing but that switch.
inline constexpr const char* kSpilloverParamId = "spillover";

// The `spillover` switch, for the parameter table of a kind whose tail should
// ring out when the block is bypassed (BlockProcessor::supportsSpillover):
// a Bool, on by default, not automatable. Put it last in the table.
BlockParamSpec spilloverParam();

// Index of the parameter with that id, or -1.
int findBlockParam(const BlockParamTable& table, const juce::String& id);

// One read-only value a block kind reports while it runs: a compressor's gain
// reduction, say. Declared in a table beside the parameters so a client can
// learn what a kind reports without knowing it; written by the audio thread
// and read by meters.get. Ids follow the parameter rules (stable, lowerCamel)
// and live in their own namespace.
struct BlockReadingSpec {
  juce::String id;
  juce::String name;
  juce::String unit;
  // The range a meter should draw. A reading is clamped into it.
  float min{0.0f};
  float max{1.0f};

  // { id, name, unit?, min, max }
  juce::var toVar() const;
};

using BlockReadingTable = std::vector<BlockReadingSpec>;
