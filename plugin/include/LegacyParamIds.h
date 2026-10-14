// Parameter ids that older builds wrote under other names. Plugin state,
// presets and MIDI maps are rewritten to the current ids as they load
// (ProcessorState.cpp, ProcessorPresets.cpp) and go back to disk under the
// current ids on their next save, so a stored file migrates the first time
// it is used. Depends on juce_core + juce_data_structures only, so
// tools/preset_tool.cpp can run the same rewrite over the factory presets.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace t3k::legacy_ids {

/** The current id for `id`: the renamed one when an older build wrote it,
    otherwise `id` itself. */
juce::String currentParamId(const juce::String& id);

/** Rewrites `property` of every child of `tree` through currentParamId.
    When a child already carries the current id, the renamed child replaces
    it: the only state that holds both is one an older build saved last
    (APVTS keeps ids it doesn't know and writes them back), so the legacy
    entry is the fresher one. Returns how many children were renamed. */
int migrateParamIds(juce::ValueTree tree, const juce::Identifier& property);

/** The current block kind id for `id`: the renamed one when an older build
    saved the kind under another id (Freeze, Bit Crusher, Stutter and Ring
    Modulator were "filter.*" before the Creative category), otherwise `id`. */
juce::String currentKindId(const juce::String& id);

/** Rewrites the "kind" of every KindState node under `tree` (at any depth:
    chains, scenes, grid rows) through currentKindId. Returns how many were
    renamed. */
int migrateKindIds(juce::ValueTree tree);

}  // namespace t3k::legacy_ids
