// How the built-in effects reach the kind registry (cmake/EffectSources.cmake,
// effects/EffectRegistration.h): every file under plugin/src/effects defines
// a hook, the build lists the hooks, the registry runs them. These tests are
// the "not silently" half of that scheme: a hook that registers nothing, a
// kind the registry refused and an effect without its conformance test all
// fail here.
#include "BlockKinds.h"
#include "effects/EffectRegistration.h"
#include "effects/effect_test_helpers.h"

#include <gtest/gtest.h>

namespace {

juce::File repoFile(const char* relative) {
  // T3K_TEST_FILES_DIR is <repo>/test/files.
  return juce::File(T3K_TEST_FILES_DIR).getParentDirectory().getParentDirectory().getChildFile(
      relative);
}

juce::var kindNamed(const juce::var& listed, const char* id) {
  if (const auto* kinds = listed["kinds"].getArray())
    for (const auto& kind : *kinds)
      if (kind["id"].toString() == id)
        return kind;
  return {};
}

}  // namespace

TEST(EffectRegistrationTest, EveryEffectFileRegistersItsKinds) {
  const auto& registry = BlockKindRegistry::instance();
  const auto& reports = registry.effectHookReports();
  ASSERT_EQ(reports.size(), builtinEffectHooks().size());
  ASSERT_FALSE(reports.empty()) << "no effect hooks were generated";

  for (const auto& report : reports) {
    SCOPED_TRACE(report.file.toStdString());
    EXPECT_FALSE(report.kindIds.isEmpty())
        << "the hook ran but registered no kind (a helper file belongs in a dsp/ or detail/ "
           "folder)";
    EXPECT_TRUE(repoFile("plugin/src/effects").getChildFile(report.file).existsAsFile());
    for (const auto& id : report.kindIds)
      EXPECT_NE(registry.find(id), nullptr);
  }
  EXPECT_TRUE(registry.refusedBuiltinIds().isEmpty())
      << "refused at registration (id taken, or a parameter id repeated, shadowing a common "
         "block parameter or misusing 'spillover'): "
      << registry.refusedBuiltinIds().joinIntoString(", ");
}

TEST(EffectRegistrationTest, EveryEffectKindHasItsConformanceTest) {
  // The sources of the effect tests, searched for the call. Crude on
  // purpose: it only has to notice an effect nobody wrote a test file for.
  juce::String sources;
  for (const auto& entry : juce::RangedDirectoryIterator(repoFile("test/src/effects"), true,
                                                         "*_tests.cpp"))
    sources += entry.getFile().loadFileAsString();
  ASSERT_TRUE(sources.isNotEmpty());

  for (const auto& report : BlockKindRegistry::instance().effectHookReports())
    for (const auto& id : report.kindIds)
      EXPECT_TRUE(sources.contains("expectKindConforms(\"" + id + "\""))
          << id << " (" << report.file << ") has no test calling expectKindConforms(\"" << id
          << "\", ...) under test/src/effects";
}

TEST(EffectRegistrationTest, TheKindsThatWereHereBeforeStillAre) {
  const auto& registry = BlockKindRegistry::instance();
  for (const char* id : {"nam", "ir", "utility.gain", "plugin"}) {
    const BlockKindInfo* info = registry.find(id);
    ASSERT_NE(info, nullptr) << id;
    EXPECT_FALSE(info->spillover) << id;
    EXPECT_FLOAT_EQ(info->defaultMix, 1.0f) << id;
    EXPECT_EQ(info->mixLaw, BlockMixLaw::Crossfade) << id;
  }
  EXPECT_TRUE(registry.find("utility.gain")->addable);
  EXPECT_FALSE(registry.find("nam")->addable);
}

TEST(EffectRegistrationTest, SpilloverIsDeclaredByTheTableAndTheIdIsReserved) {
  auto& registry = BlockKindRegistry::instance();
  const BlockKindInfo* delay = registry.find("delay.simple");
  ASSERT_NE(delay, nullptr);
  EXPECT_TRUE(delay->spillover);
  EXPECT_EQ(delay->category, BlockCategory::Delay);
  EXPECT_FLOAT_EQ(delay->defaultMix, 0.35f);
  EXPECT_EQ(delay->mixLaw, BlockMixLaw::DryPreserving);
  const int index = findBlockParam(delay->params, kSpilloverParamId);
  ASSERT_GE(index, 0);
  EXPECT_EQ(delay->params[static_cast<size_t>(index)].type, BlockParamType::Bool);
  EXPECT_FLOAT_EQ(delay->params[static_cast<size_t>(index)].defaultValue, 1.0f);

  // Anything but the switch under the reserved id is refused: the audio loop
  // would read it as one.
  const auto factory = []() -> std::unique_ptr<BlockProcessor> { return nullptr; };
  EXPECT_FALSE(registry.add(
      {"test.badspill", "Bad", BlockCategory::Utility,
       {BlockParamSpec::floatParam(kSpilloverParamId, "Spill", "%", 0.0f, 100.0f, 0.0f)},
       factory, true}));
  EXPECT_EQ(registry.find("test.badspill"), nullptr);
  // A refusal after startup is the caller's to handle; it is not recorded
  // as a broken built-in.
  EXPECT_TRUE(registry.refusedBuiltinIds().isEmpty());
}

TEST(EffectRegistrationTest, KindsListShowsSpilloverDefaultMixAndCategories) {
  fxtest::EffectRig rig{juce::String()};
  const juce::var listed = rig.must("kinds.list");

  const juce::var delay = kindNamed(listed, "delay.simple");
  ASSERT_TRUE(delay.isObject());
  EXPECT_EQ(delay["name"].toString(), juce::String("Simple Delay"));
  EXPECT_EQ(delay["category"].toString(), juce::String("delay"));
  EXPECT_TRUE(static_cast<bool>(delay["addable"]));
  EXPECT_TRUE(static_cast<bool>(delay["spillover"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(delay["defaultMix"]), 0.35);
  EXPECT_EQ(delay["mixLaw"].toString(), juce::String("dryPreserving"));

  const juce::var gain = kindNamed(listed, "utility.gain");
  ASSERT_TRUE(gain.isObject());
  EXPECT_FALSE(static_cast<bool>(gain["spillover"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(gain["defaultMix"]), 1.0);
  EXPECT_EQ(gain["mixLaw"].toString(), juce::String("crossfade"));
  EXPECT_EQ(kindNamed(listed, "nam")["mixLaw"].toString(), juce::String("crossfade"));
  EXPECT_EQ(kindNamed(listed, "plugin")["mixLaw"].toString(), juce::String("crossfade"));
  EXPECT_FALSE(static_cast<bool>(kindNamed(listed, "nam")["spillover"]));

  // Every category a kind can carry, by the name the kinds use.
  const auto* categories = listed["categories"].getArray();
  ASSERT_NE(categories, nullptr);
  EXPECT_EQ(categories->size(), static_cast<int>(allBlockCategories().size()));
  for (const char* name : {"amp", "drive", "cab", "dynamics", "eq", "delay", "reverb",
                           "modulation", "pitch", "filter", "utility", "plugin"})
    EXPECT_TRUE(categories->contains(juce::var(name))) << name;
  for (const auto& kind : *listed["kinds"].getArray())
    EXPECT_TRUE(categories->contains(kind["category"])) << kind["id"].toString();

  // A new block starts at its kind's mix.
  fxtest::EffectRig delayRig("delay.simple");
  EXPECT_NEAR(static_cast<double>(delayRig.blockState()["params"]["mix"]), 0.35, 1e-6);
}
