// Shipped factory preset content tests.
//
// The installers ship resources/factory-presets/*.t3kpreset verbatim, so the
// files themselves are release artifacts. These pin what shipping requires:
//
//   - every file is in the current preset framing (PresetFile.h), named
//     after its preset, with the stable factory id inside,
//   - every tone block's active model bytes are embedded, so a fresh install
//     loads the preset offline (no network, no auth),
//   - no block embeds bytes its tone no longer references: auditioned
//     leftovers baked in at authoring time bloated every user project that
//     loaded the preset until the persistence prune landed (issue #127; see
//     state_cache_tests.cpp for the runtime invariant),
//   - the chain actually restores and loads through the real processor.
#include "PresetManager.h"
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

namespace {

juce::File factoryPresetsDir() {
  // T3K_TEST_FILES_DIR is <repo>/test/files; the shipped presets live at
  // <repo>/resources/factory-presets.
  return juce::File(T3K_TEST_FILES_DIR)
      .getParentDirectory()
      .getParentDirectory()
      .getChildFile("resources")
      .getChildFile("factory-presets");
}

// The model ids a block's stored tone can still name: its active model plus
// the toneJson models array (the same rule as ChainBlock::referencesModel,
// applied to the file's shape before any restore).
std::set<int> referencedModelIds(const juce::ValueTree& block) {
  std::set<int> ids;
  ids.insert(static_cast<int>(block.getProperty("activeModelId", 0)));
  const juce::var tone = juce::JSON::parse(block.getProperty("toneJson").toString());
  if (const auto* models = tone["models"].getArray())
    for (const auto& model : *models)
      ids.insert(static_cast<int>(model["id"]));
  return ids;
}

TEST(FactoryPresetTest, ShippedPresetsAreSlimAndLoadOffline) {
  const juce::File dir = factoryPresetsDir();
  if (!dir.isDirectory())
    GTEST_SKIP() << "no resources/factory-presets directory in this checkout";

  const auto files = dir.findChildFiles(juce::File::findFiles, false,
                                        "*" + juce::String(PresetManager::kFileExtension));
  ASSERT_FALSE(files.isEmpty()) << "factory preset folder exists but holds no presets";

  for (const auto& file : files) {
    SCOPED_TRACE(file.getFileName().toStdString());

    // Real preset framing (see PresetFile.h): the current v2 header form,
    // since shipped files are regenerated rather than migrated lazily. The
    // header alone must identify the preset (that's what a cold list() reads)
    // and agree with the body, and the id must be the stable factory uuid,
    // not something derived from the readable filename.
    const auto header = t3k::presetfile::readHeader(file);
    ASSERT_TRUE(header.valid);
    EXPECT_FALSE(header.legacy) << "shipped preset is in the v1 framing; run PresetTool migrate";
    ASSERT_TRUE(header.id.isNotEmpty());
    ASSERT_TRUE(header.name.isNotEmpty());
    EXPECT_NE(header.id, file.getFileNameWithoutExtension());
    EXPECT_EQ(file.getFileNameWithoutExtension(), t3k::presetfile::sanitizeStem(header.name))
        << "filename is not the sanitized display name";

    const juce::ValueTree preset = t3k::presetfile::read(file);
    ASSERT_TRUE(preset.hasType(PresetManager::kPresetTag));
    EXPECT_EQ(preset.getProperty("id").toString(), header.id);
    EXPECT_EQ(preset.getProperty("name").toString(), header.name);

    const juce::ValueTree snapshot = preset.getChildWithName("ChainSnapshot");
    ASSERT_TRUE(snapshot.isValid());

    // File-level invariants, per tone block in both lanes.
    int toneBlocks = 0;
    for (const auto* laneName : {"ChainBlocks", "RightChainBlocks"}) {
      const juce::ValueTree lane = snapshot.getChildWithName(laneName);
      for (int i = 0; i < lane.getNumChildren(); ++i) {
        const juce::ValueTree block = lane.getChild(i);
        const juce::ValueTree cache = block.getChildWithName("ModelCache");
        if (!cache.isValid())
          continue;  // insert slot
        ++toneBlocks;

        const std::set<int> referenced = referencedModelIds(block);
        std::set<int> cached;
        for (int j = 0; j < cache.getNumChildren(); ++j) {
          const juce::ValueTree model = cache.getChild(j);
          const int modelId = model.getProperty("modelId");
          cached.insert(modelId);
          EXPECT_NE(referenced.count(modelId), 0u)
              << "embeds bytes for model " << modelId << ", which block "
              << block.getProperty("id").toString() << "'s tone no longer references";
          EXPECT_NE(model.getProperty("data").getBinaryData(), nullptr);
        }
        EXPECT_NE(cached.count(static_cast<int>(block.getProperty("activeModelId", 0))), 0u)
            << "active model bytes not embedded; a fresh install can't load this offline";
      }
    }
    ASSERT_GT(toneBlocks, 0) << "preset has no tone blocks";

    // And the chain restores + loads through the real processor, cache-first
    // (the assertions above guarantee no load ever needs the network).
    ChainTestProcessor proc;
    proc.restoreFromTree(snapshot);
    EXPECT_TRUE(waitForChainLoaded(proc)) << "preset chain did not finish loading offline";
  }
}


// Renders a factory preset through the real preset load (the way
// PedaleiraRender does: straight from its file, scratch stores, multicore off,
// prepared again once the rig is in place) and returns the raw stereo output.
juce::AudioBuffer<float> renderFactoryPresetStereo(const juce::String& presetFile,
                                                   std::int64_t wobbleSeed, double seconds) {
  constexpr double kRate = 48000.0;
  constexpr int kBlock = 128;

  const juce::File store = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("t3k-factory-render-" + juce::Uuid().toString());
  EXPECT_TRUE(store.createDirectory().wasOk());

  TONE3000Processor::setDeterministicRandomForOfflineRender(wobbleSeed);
  juce::AudioBuffer<float> out;
  {
    TONE3000Processor proc;
    proc.setMultiCoreEnabled(false, /*persist=*/false);
    proc.setPresetStoreForTesting(store);
    proc.setPlayConfigDetails(2, 2, kRate, kBlock);
    proc.prepareToPlay(kRate, kBlock);

    EXPECT_TRUE(proc.loadPresetFile(factoryPresetsDir().getChildFile(presetFile)));
    EXPECT_TRUE(waitForChainLoaded(proc));
    proc.prepareToPlay(kRate, kBlock);
    EXPECT_TRUE(waitForChainLoaded(proc));

    // Fixed-seed LCG noise plus a tone, right channel slightly different, so
    // the rig has something broadband to chew on.
    const int frames = static_cast<int>(seconds * kRate);
    out.setSize(2, frames);
    juce::AudioBuffer<float> block(2, kBlock);
    juce::MidiBuffer midi;
    std::uint32_t lcg = 0x2468ACE1u;
    for (int start = 0; start < frames; start += kBlock) {
      const int n = juce::jmin(kBlock, frames - start);
      for (int i = 0; i < n; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>(lcg >> 8) / 8388608.0f - 1.0f;
        const float tone = std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f *
                                    static_cast<float>(start + i) / static_cast<float>(kRate));
        block.setSample(0, i, 0.15f * tone + 0.05f * noise);
        block.setSample(1, i, 0.12f * tone - 0.05f * noise);
      }
      proc.processBlock(block, midi);
      for (int ch = 0; ch < 2; ++ch)
        out.copyFrom(ch, start, block, ch, 0, n);
    }
  }
  TONE3000Processor::clearDeterministicRandomForOfflineRender();
  store.deleteRecursively();
  return out;
}

bool identical(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
  if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
    return false;
  for (int ch = 0; ch < a.getNumChannels(); ++ch)
    if (std::memcmp(a.getReadPointer(ch), b.getReadPointer(ch),
                    sizeof(float) * static_cast<size_t>(a.getNumSamples())) != 0)
      return false;
  return true;
}

// "Dumble Spread" runs the Spread/Align wobble, whose noise source is
// clock-seeded by default. With the offline seed set, two renders must match
// to the bit; a different seed must not (otherwise the wobble was never
// actually exercised and the equality proves nothing).
TEST(FactoryPresetTest, DumbleSpreadRendersBitExactWithFixedWobbleSeed) {
  if (!factoryPresetsDir().getChildFile("Dumble Spread.t3kpreset").existsAsFile())
    GTEST_SKIP() << "no Dumble Spread factory preset in this checkout";

  const auto first = renderFactoryPresetStereo("Dumble Spread.t3kpreset", 1234, 1.5);
  const auto second = renderFactoryPresetStereo("Dumble Spread.t3kpreset", 1234, 1.5);
  const auto other = renderFactoryPresetStereo("Dumble Spread.t3kpreset", 99, 1.5);

  EXPECT_GT(first.getMagnitude(0, first.getNumSamples()), 1e-3f) << "render is silent";
  EXPECT_TRUE(identical(first, second)) << "same seed, different render";
  EXPECT_FALSE(identical(first, other)) << "seed has no effect: the wobble is not in play";
}

}  // namespace
