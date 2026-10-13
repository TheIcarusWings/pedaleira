// The plugin reference mode of PedaleiraMatch (tools/MatchPlugin.cpp): a
// plugin is hosted offline, parameters are set by name, by text or by row, and
// the test signal comes back with the plugin's latency taken off. The
// reference is PedaleiraTestFx (test/plugins), whose audio is exact:
// out = gain * in delayed by `delaySamples`, which it also reports as its
// latency, so every expectation below is a statement about samples.
#include "MatchPlugin.h"
#include "hosted_plugin_test_helpers.h"

#include <gtest/gtest.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <memory>
#include <random>

namespace {

using MatchPlugin::Setting;

juce::AudioBuffer<float> noise(int frames, unsigned seed = 7) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
  juce::AudioBuffer<float> buffer(1, frames);
  for (int i = 0; i < frames; ++i)
    buffer.setSample(0, i, dist(rng));
  return buffer;
}

juce::File scratchFolder(const juce::String& name) {
  const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("t3k-match-plugin-" + name + "-" + juce::Uuid().toString());
  dir.createDirectory();
  return dir;
}

juce::AudioBuffer<float> readWav(const juce::File& file, double* rate = nullptr) {
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  EXPECT_NE(reader, nullptr);
  juce::AudioBuffer<float> out;
  if (reader == nullptr)
    return out;
  if (rate != nullptr)
    *rate = reader->sampleRate;
  out.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
  reader->read(&out, 0, out.getNumSamples(), 0, true, true);
  return out;
}

const int kDelayRow = 1;

}  // namespace

TEST(MatchPlugin, ParsesSettings) {
  const Setting byName = MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::value, "Gain=0.25");
  EXPECT_EQ(byName.name, "Gain");
  EXPECT_DOUBLE_EQ(byName.value, 0.25);

  const Setting byText = MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::text, "Low Cut=80 Hz");
  EXPECT_EQ(byText.name, "Low Cut");
  EXPECT_EQ(byText.text, "80 Hz");

  const Setting byIndex = MatchPlugin::parseSetting(Setting::Target::index, Setting::Kind::value, "3=1");
  EXPECT_EQ(byIndex.index, 3);

  EXPECT_THROW(MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::value, "Gain"),
               MatchPlugin::PluginError);
  EXPECT_THROW(MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::value, "Gain=1.5"),
               MatchPlugin::PluginError);
  EXPECT_THROW(MatchPlugin::parseSetting(Setting::Target::index, Setting::Kind::value, "x=0.5"),
               MatchPlugin::PluginError);
}

TEST(MatchPlugin, ListsParameters) {
  ScannerSandbox sandbox;
  MatchPlugin::Loaded loaded = MatchPlugin::load(testPluginFile().getFullPathName(), {});
  ASSERT_NE(loaded.instance, nullptr);
  EXPECT_EQ(loaded.description.name, "PedaleiraTestFx");

  const auto params = MatchPlugin::listParameters(*loaded.instance);
  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params[0].index, 0);
  EXPECT_EQ(params[0].name, "Gain");
  EXPECT_EQ(params[0].text, "1.000");
  EXPECT_EQ(params[0].steps, 0) << "a float is continuous";
  EXPECT_EQ(params[1].name, "Delay Samples");
  // The VST3 wrapper of the test plugin reports its integer parameter as continuous too; a plugin
  // with a real switch reports its step count here (0 means continuous).
  EXPECT_EQ(params[1].steps, 0);
}

TEST(MatchPlugin, SetsParametersByNameTextAndIndex) {
  ScannerSandbox sandbox;
  MatchPlugin::Loaded loaded = MatchPlugin::load(testPluginFile().getFullPathName(), {});
  HostedPluginInstance& fx = *loaded.instance;

  // The name matches without regard to case; the value is normalised (0..2 is the gain's range).
  auto applied = MatchPlugin::apply(
      fx, MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::value, "gAiN=0.25"));
  EXPECT_EQ(applied.index, 0);
  EXPECT_EQ(applied.name, "Gain");
  EXPECT_FLOAT_EQ(applied.value, 0.25f);
  EXPECT_EQ(applied.text, "0.500");

  // Text goes through the plugin's own conversion.
  applied = MatchPlugin::apply(
      fx, MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::text, "Gain=1.5"));
  EXPECT_FLOAT_EQ(applied.value, 0.75f);
  EXPECT_EQ(applied.text, "1.500");

  // By row.
  applied = MatchPlugin::apply(
      fx, MatchPlugin::parseSetting(Setting::Target::index, Setting::Kind::value, "1=0.015625"));
  EXPECT_EQ(applied.name, "Delay Samples");
  EXPECT_EQ(applied.text, "64");

  EXPECT_THROW(MatchPlugin::apply(fx, MatchPlugin::parseSetting(Setting::Target::name,
                                                                Setting::Kind::value, "Nope=0.5")),
               MatchPlugin::PluginError);
  EXPECT_THROW(MatchPlugin::apply(fx, MatchPlugin::parseSetting(Setting::Target::index,
                                                                Setting::Kind::value, "9=0.5")),
               MatchPlugin::PluginError);
}

TEST(MatchPlugin, RenderIsExactAndLatencyIsCompensated) {
  ScannerSandbox sandbox;
  const juce::AudioBuffer<float> input = noise(5000);
  // Gain 0.5 by text, a delay of 64 samples by name; blocks that do not divide the signal.
  for (const int block : {100, 512}) {
    MatchPlugin::Loaded loaded = MatchPlugin::load(testPluginFile().getFullPathName(), {});
    MatchPlugin::apply(*loaded.instance, MatchPlugin::parseSetting(Setting::Target::name,
                                                                   Setting::Kind::text, "gain=0.5"));
    MatchPlugin::apply(*loaded.instance,
                       MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::value,
                                                 "Delay Samples=0.015625"));
    const MatchPlugin::Rendered out = MatchPlugin::render(*loaded.instance, input, 480, block, 0);

    EXPECT_EQ(out.latencySamples, 64) << "block " << block;
    EXPECT_EQ(out.laneChannels, 2);
    ASSERT_EQ(out.audio.getNumSamples(), 5000 + 480);
    int wrong = 0;
    for (int channel = 0; channel < 2; ++channel)
      for (int i = 0; i < 5000; ++i)
        if (out.audio.getSample(channel, i) != 0.5f * input.getSample(0, i))
          ++wrong;
    EXPECT_EQ(wrong, 0) << "sample n of the output answers sample n of the input, block " << block;
    // Past the signal there is only the tail, which is silent.
    for (int i = 5000; i < 5480; ++i)
      ASSERT_EQ(out.audio.getSample(0, i), 0.0f);
  }
}

TEST(MatchPlugin, MonoLaneFoldsAndStaysAligned) {
  ScannerSandbox sandbox;
  MatchPlugin::Loaded loaded = MatchPlugin::load(testPluginFile().getFullPathName(), {});
  MatchPlugin::apply(*loaded.instance, MatchPlugin::parseSetting(Setting::Target::index, Setting::Kind::value,
                                                                 "1=0.00390625"));  // 16 samples
  const juce::AudioBuffer<float> input = noise(2000, 11);
  const MatchPlugin::Rendered out = MatchPlugin::render(*loaded.instance, input, 0, 256, 1);
  EXPECT_EQ(out.latencySamples, 16);
  ASSERT_EQ(out.audio.getNumChannels(), 1);
  ASSERT_EQ(out.audio.getNumSamples(), 2000);
  for (int i = 0; i < 2000; ++i)
    ASSERT_EQ(out.audio.getSample(0, i), input.getSample(0, i)) << i;
}

TEST(MatchPlugin, RestoresAStateChunkBeforeSettingParameters) {
  ScannerSandbox sandbox;
  const juce::File dir = scratchFolder("state");
  juce::File stateFile = dir.getChildFile("fx.state");
  {
    MatchPlugin::Loaded loaded = MatchPlugin::load(testPluginFile().getFullPathName(), {});
    MatchPlugin::apply(*loaded.instance, MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::text,
                                                                   "Gain=1.5"));
    loaded.instance->serviceOnMessageThread(true);
    const juce::MemoryBlock chunk = loaded.instance->cachedState();
    ASSERT_GT(chunk.getSize(), 0u);
    ASSERT_TRUE(stateFile.replaceWithData(chunk.getData(), chunk.getSize()));
  }
  MatchPlugin::Loaded restored = MatchPlugin::load(testPluginFile().getFullPathName(), stateFile);
  EXPECT_EQ(restored.stateKind, "chunk");
  EXPECT_EQ(MatchPlugin::listParameters(*restored.instance)[0].text, "1.500");
  // A setting made after the state wins over it.
  MatchPlugin::apply(*restored.instance, MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::text,
                                                                   "Gain=0.5"));
  EXPECT_EQ(MatchPlugin::listParameters(*restored.instance)[0].text, "0.500");
  dir.deleteRecursively();
}

TEST(MatchPlugin, RenderCommandWritesTheReferenceAndItsJson) {
  ScannerSandbox sandbox;
  const juce::File dir = scratchFolder("render");

  // The input signal: a short noise burst at 48 kHz.
  const juce::AudioBuffer<float> input = noise(4800, 3);
  const juce::File signal = dir.getChildFile("signal.wav");
  {
    std::unique_ptr<juce::OutputStream> stream = signal.createOutputStream();
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}
                    .withSampleRate(48000.0)
                    .withNumChannels(1)
                    .withBitsPerSample(32)
                    .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    ASSERT_NE(writer, nullptr);
    ASSERT_TRUE(writer->writeFromAudioSampleBuffer(input, 0, input.getNumSamples()));
  }

  MatchPlugin::Options options;
  options.plugin = testPluginFile().getFullPathName();
  options.input = signal;
  options.output = dir.getChildFile("ref").getChildFile("take.wav");
  options.tailSeconds = 0.1;
  options.block = 256;
  options.settings.push_back(
      MatchPlugin::parseSetting(Setting::Target::name, Setting::Kind::text, "Gain=0.5"));
  options.settings.push_back(
      MatchPlugin::parseSetting(Setting::Target::index, Setting::Kind::value, "1=0.03125"));  // 128
  EXPECT_EQ(MatchPlugin::runRender(options), 0);

  double rate = 0.0;
  const juce::AudioBuffer<float> wav = readWav(options.output, &rate);
  EXPECT_DOUBLE_EQ(rate, 48000.0);
  ASSERT_EQ(wav.getNumSamples(), 4800 + 4800);
  for (int i = 0; i < 4800; ++i)
    ASSERT_EQ(wav.getSample(0, i), 0.5f * input.getSample(0, i)) << i;

  const juce::var json = juce::JSON::parse(options.output.withFileExtension("json"));
  ASSERT_TRUE(json.isObject());
  EXPECT_EQ(json["format"].toString(), "pedaleira-plugin-reference");
  EXPECT_EQ(json["plugin"]["name"].toString(), "PedaleiraTestFx");
  EXPECT_EQ(static_cast<int>(json["latencySamples"]), 128);
  EXPECT_TRUE(static_cast<bool>(json["latencyCompensated"]));
  ASSERT_EQ(json["parameters"].size(), 2);
  EXPECT_EQ(json["parameters"][0]["name"].toString(), "Gain");
  EXPECT_EQ(json["parameters"][0]["requested"]["text"].toString(), "0.5");
  EXPECT_EQ(json["parameters"][0]["text"].toString(), "0.500");
  EXPECT_EQ(json["parameters"][kDelayRow]["text"].toString(), "128");
  dir.deleteRecursively();
}

TEST(MatchPlugin, RefusesToWriteInsideTheRepository) {
  const juce::File fakeRepo = scratchFolder("repo");
  fakeRepo.getChildFile(".git").createDirectory();
  fakeRepo.getChildFile("plugin/src").createDirectory();
  ASSERT_TRUE(fakeRepo.getChildFile("plugin/src/EffectMatch.cpp").replaceWithText("//"));
  EXPECT_EQ(MatchPlugin::repositoryOf(fakeRepo.getChildFile("match/ref/take.wav")), fakeRepo);

  MatchPlugin::Options options;
  options.plugin = testPluginFile().getFullPathName();
  options.output = fakeRepo.getChildFile("match/ref/take.wav");
  EXPECT_THROW(MatchPlugin::runRender(options), MatchPlugin::PluginError);
  EXPECT_FALSE(options.output.exists());

  EXPECT_EQ(MatchPlugin::repositoryOf(scratchFolder("elsewhere").getChildFile("a.wav")), juce::File());
  fakeRepo.deleteRecursively();
}

TEST(MatchPlugin, ReportsAPluginThatIsNotThere) {
  ScannerSandbox sandbox;
  EXPECT_THROW(MatchPlugin::load("/no/such/Plugin.vst3", {}), MatchPlugin::PluginError);
}
