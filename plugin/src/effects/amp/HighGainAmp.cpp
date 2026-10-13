// amp.highgain: a modern high gain tube amp model, composed from the
// components in plugin/include/amp (namespace amp).
//
//   in -> [4x: input coupling high-pass -> grid stopper low-pass -> cathode
//         shelf -> triode 1 -> gain pot with bright cap -> (coupling high-pass
//         -> triode) x 3 -> passive tone stack -> master -> power amp with
//         presence / depth feedback and supply sag] -> DC blocker -> level
//
// Every number that sets the sound is in resources/amps/high-gain.json, an
// amp description (docs/amp-framework.md) that is linked into the binary and
// read by the same loader a user's own description goes through. The circuit
// and the knobs are the shared amp engine (dsp/AmpProcessor.h); this file only
// says which description this kind is. The same amp can be loaded into an
// amp.custom block from the file, and sounds the same.
#include "dsp/AmpProcessor.h"
#include "effects/amp/BuiltinAmps.h"
#include "effects/EffectRegistration.h"

namespace {

constexpr const char* kDescriptionFile = "high-gain.json";

class HighGainAmpProcessor final : public ampfx::AmpProcessorBase {
public:
  static constexpr const char* kKindId = "amp.highgain";

  static const BlockParamTable& parameterSpecs() {
    static const BlockParamTable table = [] {
      const auto description = ampfx::builtinAmp(kDescriptionFile);
      juce::StringArray names;
      for (int v = 0; v < description->numVoicings; ++v)
        names.add(description->voicings[v].name);
      return makeParameterTable(names, description->defaultVoicing);
    }();
    return table;
  }

  HighGainAmpProcessor() : AmpProcessorBase(kKindId, parameterSpecs(), ampfx::builtinAmp(kDescriptionFile)) {}
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(HighGainAmp) {
  addEffectKind<HighGainAmpProcessor>(registry, "High Gain Amp", BlockCategory::Amp);
}
