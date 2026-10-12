#include "effects/dsp/TempoSync.h"

namespace fx {

const juce::StringArray& noteDivisionNames() {
  static const juce::StringArray names = [] {
    juce::StringArray out;
    for (int base = 0; base < kNumNoteDivisions / 3; ++base) {
      const juce::String straight = "1/" + juce::String(1 << base);
      out.add(straight);
      out.add(straight + "D");
      out.add(straight + "T");
    }
    return out;
  }();
  return names;
}

namespace {

// "sync" or, with a prefix, "leftSync".
juce::String prefixedId(const juce::String& prefix, const char* id) {
  if (prefix.isEmpty())
    return id;
  const juce::String tail(id);
  return prefix + tail.substring(0, 1).toUpperCase() + tail.substring(1);
}

BlockParamSpec flagged(BlockParamSpec spec, bool affectsTail) {
  return affectsTail ? spec.withAffectsTail() : spec;
}

}  // namespace

TimeParamIndices appendTimeParams(BlockParamTable& table, const TimeParamOptions& options) {
  TimeParamIndices indices;
  indices.sync = static_cast<int>(table.size());
  indices.division = indices.sync + 1;
  indices.timeMs = indices.sync + 2;

  // The switch is not automatable: it swaps which of the other two is the
  // time, which is a jump an effect glides through but a host lane should
  // not sweep. The other two are.
  table.push_back(flagged(BlockParamSpec::boolParam(prefixedId(options.idPrefix, "sync"),
                                                    options.name + " Sync", options.defaultSync)
                              .notAutomatable(),
                          options.affectsTail));
  table.push_back(flagged(BlockParamSpec::choiceParam(prefixedId(options.idPrefix, "division"),
                                                      options.name + " Division",
                                                      noteDivisionNames(), options.defaultDivision),
                          options.affectsTail));
  table.push_back(flagged(BlockParamSpec::floatParam(prefixedId(options.idPrefix, "timeMs"),
                                                     options.name, "ms", options.minMs,
                                                     options.maxMs, options.defaultMs)
                              .withSkew(0.4f)
                              .withTempoSync(),
                          options.affectsTail));
  return indices;
}

RateParamIndices appendRateParams(BlockParamTable& table, const RateParamOptions& options) {
  RateParamIndices indices;
  indices.sync = static_cast<int>(table.size());
  indices.division = indices.sync + 1;
  indices.rateHz = indices.sync + 2;

  table.push_back(BlockParamSpec::boolParam(prefixedId(options.idPrefix, "sync"),
                                            options.name + " Sync", options.defaultSync)
                      .notAutomatable());
  table.push_back(BlockParamSpec::choiceParam(prefixedId(options.idPrefix, "division"),
                                              options.name + " Division", noteDivisionNames(),
                                              options.defaultDivision));
  table.push_back(BlockParamSpec::floatParam(prefixedId(options.idPrefix, "rateHz"), options.name,
                                             "Hz", options.minHz, options.maxHz,
                                             options.defaultHz)
                      .withSkew(0.4f)
                      .withTempoSync());
  return indices;
}

}  // namespace fx
