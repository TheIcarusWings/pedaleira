#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

// Scenes, stomp assignments and footswitch modes: the plain data the
// processor keeps for them (ProcessorScenes.cpp does the work). Everything
// here is keyed by block id and parameter id, never by lane or position, so
// it survives whatever the routing around the blocks turns into.
//
// The model is a floor modeller's. A rig has eight scenes, A to H. A scene
// holds the on/off state of every block and one value for each parameter
// that has been scene-assigned; every other parameter has one value shared
// by all scenes. Eight footswitches plus Bank Up, Bank Down and Tempo act on
// presets, scenes or stomp assignments depending on the footswitch mode.

inline constexpr int kNumScenes = 8;
inline constexpr int kNumFootswitches = 8;

// The eight switches A to H are 0 to 7; the three others follow.
inline constexpr int kFootswitchBankUp = 8;
inline constexpr int kFootswitchBankDown = 9;
inline constexpr int kFootswitchTempo = 10;
inline constexpr int kNumFootswitchInputs = 11;

// "A" to "H" for 0 to 7.
juce::String sceneLetter(int index);
// 0 to 7 for "A" to "H" (either case), else -1.
int sceneIndexFromLetter(const juce::String& letter);
// "A".."H", "bankUp", "bankDown", "tempo" for a footswitch input, and back
// (-1 for anything else).
juce::String footswitchIdToString(int footswitch);
int footswitchIdFromString(const juce::String& id);

// One parameter of one block.
struct SceneParamKey {
  std::string blockId;
  juce::String paramId;

  bool operator<(const SceneParamKey& other) const {
    if (blockId != other.blockId)
      return blockId < other.blockId;
    return paramId < other.paramId;
  }
  bool operator==(const SceneParamKey& other) const {
    return blockId == other.blockId && paramId == other.paramId;
  }
};

/**
 * What the eight scenes of a rig hold. Guarded by the processor's chainMutex
 * and only touched off the audio thread: a scene switch copies values from
 * here onto the blocks, which is all the audio thread ever sees.
 *
 * The blocks themselves always carry the ACTIVE scene (their `enabled`
 * switch, their gains and mix, their processor's parameter values), which is
 * also what the block nodes of saved state hold, so a build without scenes
 * opens a rig as its active scene. The entries here for the active scene are
 * therefore only current once the processor has copied the live values in
 * (it does so before it leaves a scene and before it saves).
 *
 * Both maps are sparse:
 *  - `blockStates` has an entry for a block once its switch has been moved.
 *    A block without one is in the same state in every scene, the state it
 *    is in right now. That is the rule for new blocks: a block enters every
 *    scene the way it was added (on, or off for a copy of a bypassed block),
 *    and for rigs saved before scenes existed: every scene equals the saved
 *    state.
 *  - `assigned` has an entry for each scene-assigned parameter, with its
 *    value in each scene: a knob position from 0 to 1 for the common ones
 *    (inputGain, outputGain, mix), the kind's real unit for a kind
 *    parameter, the plugin's normalised 0 to 1 for a hosted plugin's.
 *
 * `expression` flags parameters handed to an expression pedal. The pedal is
 * not here yet; the flag is, because it decides what a scene may hold: a
 * flagged parameter cannot be scene-assigned (a scene switch must not pull
 * a pedal-driven value away from the pedal), and flagging an assigned one
 * unassigns it.
 */
class SceneStore {
public:
  int active{0};
  // Empty = the default name ("Scene A").
  std::array<juce::String, kNumScenes> names;
  std::map<std::string, std::array<bool, kNumScenes>> blockStates;
  std::map<SceneParamKey, std::array<float, kNumScenes>> assigned;
  std::set<SceneParamKey> expression;

  static juce::String defaultName(int index);
  juce::String nameOf(int index) const;

  bool isAssigned(const SceneParamKey& key) const { return assigned.count(key) != 0; }
  bool isExpression(const SceneParamKey& key) const { return expression.count(key) != 0; }

  // Nothing a rig without scenes would not have: scene A active, default
  // names, no block state of its own, nothing assigned or flagged. Such a
  // store is not written, so a rig that never used scenes saves exactly as
  // it did before they existed.
  bool isPristine() const;
  void clear();

  // Everything kept for a block that has left the rig.
  void removeBlock(const std::string& blockId);
  // What is kept for a block's kind parameters (everything but the three
  // common ones), for a block that keeps its id but changes kind.
  void removeKindParams(const std::string& blockId);

  // Scenes version=1 active=<0..7>
  //   Scene index=<0..7> [name="..."]            (always eight)
  //     Block id="<block id>" enabled=<bool>
  //     Value blockId="<block id>" param="<param id>" value=<double>
  //   Expression blockId="<block id>" param="<param id>"
  //
  // A parameter is scene-assigned when the scenes hold a Value for it. A
  // reader ignores children and properties it does not know.
  juce::ValueTree toValueTree() const;
  // An invalid tree (a rig saved before scenes) clears the store.
  void restoreFromValueTree(const juce::ValueTree& tree);
};

// One footswitch's stomp assignment: the blocks it flips, together.
struct StompAssignment {
  std::vector<std::string> blockIds;
  // Empty = named after the first block.
  juce::String label;
  // On while held: the press flips the blocks and the release flips them
  // back.
  bool momentary{false};

  bool isEmpty() const { return blockIds.empty(); }
};

/** The stomp assignments of a rig, one per footswitch A to H. Guarded by
    chainMutex like the scenes, and saved next to them. */
class StompStore {
public:
  std::array<StompAssignment, kNumFootswitches> stomps;

  bool isEmpty() const;
  void clear();
  void removeBlock(const std::string& blockId);

  // Stomps version=1
  //   Stomp switch=<0..7> [label="..."] momentary=<bool>
  //     Block id="<block id>"
  juce::ValueTree toValueTree() const;
  void restoreFromValueTree(const juce::ValueTree& tree);
};

// What the eight switches do. Global, not part of a rig: it describes how
// the player works the board, and stays put across presets.
enum class FootswitchMode { Preset = 0, Scene = 1, Stomp = 2, Hybrid = 3 };
// What one row of four does in Hybrid mode.
enum class FootswitchRowRole { Scene = 0, Stomp = 1, Preset = 2 };

juce::String footswitchModeToString(FootswitchMode mode);
// Unknown strings give `fallback`.
FootswitchMode footswitchModeFromString(const juce::String& text,
                                        FootswitchMode fallback = FootswitchMode::Stomp);
juce::String footswitchRowRoleToString(FootswitchRowRole role);
FootswitchRowRole footswitchRowRoleFromString(const juce::String& text,
                                              FootswitchRowRole fallback);

/**
 * Tap tempo: turns the times of successive taps into a tempo.
 *
 * The tempo is the average of the last few intervals (up to four). A gap
 * longer than the slowest tempo starts a new count, a tap closer to the last
 * one than the fastest tempo is a switch bounce and is dropped, and an
 * interval that is far off the running average is an outlier: it is left out
 * and the count carries on, unless the next interval agrees with it, in
 * which case the player has changed tempo and the count restarts from those
 * two.
 *
 * Plain arithmetic on the times it is handed, so it is as testable as it is
 * deterministic. Not thread-safe: one thread taps.
 */
class TapTempo {
public:
  static constexpr double kMinBpm = 20.0;
  static constexpr double kMaxBpm = 300.0;
  static constexpr int kMaxIntervals = 4;
  // How far an interval may be from the running average, as a fraction of
  // it, before it counts as an outlier.
  static constexpr double kOutlierTolerance = 0.3;

  // One tap at `timeSeconds` (any clock that only moves forward). True when
  // the taps so far give a tempo, which `bpm` then receives, within
  // kMinBpm..kMaxBpm.
  bool tap(double timeSeconds, double& bpm);
  void reset();

private:
  bool hasLast{false};
  double lastTime{0.0};
  std::array<double, kMaxIntervals> intervals{};
  int numIntervals{0};
  // An interval set aside as an outlier, 0 when there is none.
  double pendingOutlier{0.0};
};
