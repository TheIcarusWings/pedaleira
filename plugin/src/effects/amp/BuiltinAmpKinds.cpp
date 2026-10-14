// The amps that ship in the binary, one block kind each.
//
// Every description in resources/amps (docs/amp-framework.md) is linked into
// the binary (cmake/AmpFramework.cmake) and becomes a kind of its own here:
// its id is "amp." and the file name without ".json" in lower case, letters
// and digits only (builtinAmpKindId), its name the description's name, its
// family (High gain, Clean, Crunch) and character line the ones the
// description gives, its parameters the ten of every amp with the
// description's own voicings. So the block picker, its search and kinds.list
// all see "Lead 100" and "Red Forty" as kinds, and a new amp is a new file in
// resources/amps and nothing else; there is no list to keep in step.
//
// amp.highgain, the first amp, is high-gain.json and keeps its id, so saved
// presets that name it load as they did. amp.custom (CustomAmp.cpp) stays the
// block that loads any description from a file (block.loadDescription).
//
//   in -> [4x: input coupling high-pass -> grid stopper low-pass -> cathode
//         shelf -> triode 1 -> gain pot with bright cap -> (coupling high-pass
//         -> triode) x 3 -> passive tone stack -> master -> power amp with
//         presence / depth feedback and supply sag] -> DC blocker -> level
//
// The circuit and the knobs are the shared amp engine (dsp/AmpProcessor.h);
// this file only says which description each kind is. The same amp loaded
// into an amp.custom block from its file sounds the same.
#include <deque>
#include <memory>
#include <string>

#include "dsp/AmpProcessor.h"
#include "effects/amp/BuiltinAmps.h"
#include "effects/EffectRegistration.h"

namespace {

// What one shipped amp is, kept for the life of the process: a processor
// refers to its kind id and parameter table (ParameterisedBlockProcessor), so
// both live here, at an address that does not move.
struct BuiltinAmpKind {
  std::string file;
  std::string id;
  std::shared_ptr<const amp::AmpDescription> description;
  BlockParamTable table;
};

class BuiltinAmpProcessor final : public ampfx::AmpProcessorBase {
public:
  explicit BuiltinAmpProcessor(const BuiltinAmpKind& kind)
      : AmpProcessorBase(kind.id.c_str(), kind.table, kind.description) {}
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(BuiltinAmpKinds) {
  static std::deque<BuiltinAmpKind> kinds;
  for (const std::string& file : ampfx::builtinAmpFiles()) {
    const auto description = ampfx::builtinAmp(file.c_str());
    // A file that does not parse is a build error: the tests load every
    // shipped file and say which one. Here it is left out rather than a kind
    // that cannot be built.
    if (description == nullptr)
      continue;
    juce::StringArray voicings;
    for (int v = 0; v < description->numVoicings; ++v)
      voicings.add(description->voicings[v].name);
    // A choice parameter lists at least two: an amp with one voicing lists it
    // twice (the faceplate draws the description's own voicings, so it has no
    // switch to choose between them).
    while (voicings.size() < 2)
      voicings.add(voicings[voicings.size() - 1]);
    kinds.push_back({file, ampfx::builtinAmpKindId(file), description,
                     ampfx::AmpProcessorBase::makeParameterTable(voicings, description->defaultVoicing)});
    const BuiltinAmpKind& kind = kinds.back();

    BlockKindInfo info;
    info.id = kind.id;
    info.name = juce::String(description->name);
    info.category = BlockCategory::Amp;
    info.params = kind.table;
    info.readings = ampfx::AmpProcessorBase::readingSpecs();
    info.factory = [&kind]() -> std::unique_ptr<BlockProcessor> { return std::make_unique<BuiltinAmpProcessor>(kind); };
    info.addable = true;
    info.family = juce::String(amp::ampFamilyName(description->family));
    info.blurb = juce::String(description->faceplate.character);
    registry.add(std::move(info));
  }
}
