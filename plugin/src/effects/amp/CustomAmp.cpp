// amp.custom: an amp built from a description file (docs/amp-framework.md).
//
// The same engine as amp.highgain (dsp/AmpProcessor.h), but the amp is not
// fixed: block.loadDescription gives the block a description, a JSON file that
// lists an amp's parts as read off its schematic, and the block builds that
// amp. A new block starts as the shipped high-gain description, so it plays
// straight away.
//
// The file is not a parameter in the table (parameters are numbers): it is the
// block's state, the way a hosted plugin's chunk is. The block keeps the
// description's text, writes it into saved presets (so a preset that uses a
// description does not depend on the file staying where it was) and reads it
// back when a preset or an undo restores the block. A block that still has the
// shipped description saves nothing extra.
#include "dsp/AmpProcessor.h"
#include "effects/amp/BuiltinAmps.h"
#include "effects/EffectRegistration.h"

namespace {

constexpr const char* kDefaultFile = "high-gain.json";

class CustomAmpProcessor final : public ampfx::AmpProcessorBase {
public:
  static constexpr const char* kKindId = "amp.custom";

  static const BlockParamTable& parameterSpecs() {
    // The panel is the same ten parameters on every amp; the description says
    // which parts they move. Up to three voicings.
    static const BlockParamTable table = makeParameterTable({"Voicing 1", "Voicing 2", "Voicing 3"}, 0);
    return table;
  }

  CustomAmpProcessor() : AmpProcessorBase(kKindId, parameterSpecs(), ampfx::builtinAmp(kDefaultFile)) {
    usingDefault = true;
  }

  bool takesDescription() const noexcept override { return true; }

  bool checkDescription(const juce::String& text, juce::String& error) const override {
    amp::AmpDescription scratch;
    std::string message;
    if (amp::parseAmpDescription(text.toStdString(), scratch, message))
      return true;
    error = juce::String(message);
    return false;
  }

  bool loadDescription(const juce::String& text, juce::String& error) override {
    auto parsed = std::make_shared<amp::AmpDescription>();
    std::string message;
    const std::string utf8 = text.toStdString();
    if (!amp::parseAmpDescription(utf8, *parsed, message)) {
      error = juce::String(message);
      return false;
    }
    const int voicing = parsed->defaultVoicing;
    descriptionText = utf8;
    usingDefault = false;
    applyDescription(std::move(parsed));
    // A new amp starts on the voicing its description names.
    setParameter(kVoicing, static_cast<float>(voicing));
    return true;
  }

  juce::String displayName() const override { return juce::String(description().name); }

  void getStateChunk(juce::MemoryBlock& chunk) const override {
    if (!usingDefault)
      chunk.append(descriptionText.data(), descriptionText.size());
  }

  void setStateChunk(const juce::MemoryBlock& chunk, int) override {
    auto parsed = std::make_shared<amp::AmpDescription>();
    std::string message;
    const std::string utf8(static_cast<const char*>(chunk.getData()), chunk.getSize());
    if (amp::parseAmpDescription(utf8, *parsed, message)) {
      descriptionText = utf8;
      usingDefault = false;
      applyDescription(std::move(parsed));
    } else {
      juce::Logger::writeToLog("[amp.custom] saved description does not load, using the shipped one: " +
                               juce::String(message));
      useDefault();
    }
  }

  void setKindState(const juce::ValueTree& kindState) override {
    BlockProcessor::setKindState(kindState);
    // No chunk in the node: the block was saved with the shipped description.
    if (kindState.getProperty("chunk").getBinaryData() == nullptr)
      useDefault();
  }

private:
  void useDefault() {
    if (usingDefault)
      return;
    usingDefault = true;
    descriptionText.clear();
    applyDescription(ampfx::builtinAmp(kDefaultFile));
  }

  std::string descriptionText;
  bool usingDefault{true};
};

}  // namespace

PEDALEIRA_EFFECT_KINDS(CustomAmp) {
  addEffectKind<CustomAmpProcessor>(registry, "Custom Amp", BlockCategory::Amp);
}
