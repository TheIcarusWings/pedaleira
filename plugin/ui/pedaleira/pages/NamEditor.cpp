#include "NamEditor.h"

#include "../Paint.h"
#include "../Type.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kColumnGap = 20;
constexpr int kCaptureWidth = 280;
constexpr int kLevelsWidth = 340;
constexpr int kMeterBarHeight = 64;
constexpr int kBarGap = 16;
constexpr int kKnobDiameter = 88;
constexpr int kKnobCellWidth = 96;
// Rows of the capture panel: the label and its value, then the gap.
constexpr int kFieldPitch = 50;
constexpr int kModelPitch = 66;

juce::String gearTypeName(const juce::String& gear) {
  const juce::String key = gear.toLowerCase().trim();
  if (key == "amp")
    return "Amp head";
  if (key == "amp_cab" || key == "amp+cab")
    return "Amp and cab";
  if (key == "preamp")
    return "Preamp";
  if (key == "pedal")
    return "Pedal";
  if (key == "pedal_amp")
    return "Pedal and amp";
  if (key == "full_rig")
    return "Full rig";
  if (key == "outboard")
    return "Outboard";
  if (key.isEmpty())
    return "Not in the file";
  return key.replaceCharacter('_', ' ').substring(0, 1).toUpperCase() + key.replaceCharacter('_', ' ').substring(1);
}

juce::String dbuText(const juce::var& value) {
  const double dbu = value;
  return (dbu >= 0.0 ? "+" : "") + juce::String(dbu, 1) + " dBu";
}

double levelOf(const juce::var& value) {
  if (value.isVoid() || value.isUndefined())
    return -100.0;
  return juce::jmax(-100.0, static_cast<double>(value));
}

juce::String levelText(double db) { return db <= -59.9 ? juce::String("-inf dB") : juce::String(db, 1) + " dB"; }

}  // namespace

// ####################
// METER ROW
// ####################
class NamEditor::MeterRow : public juce::Component {
public:
  explicit MeterRow(const juce::String& rowName) : name(rowName) {
    setTitle(rowName + " level");
    addAndMakeVisible(bar);
  }

  void setLevel(double db) {
    level = db;
    peak = juce::jmax(peak, db);
    bar.setLevel(static_cast<float>(db), static_cast<float>(peak));
    repaint();
  }

  void resetPeak() {
    peak = -100.0;
    level = -100.0;
    bar.setLevel(-100.0f, -100.0f);
    repaint();
  }

  void paint(juce::Graphics& g) override {
    auto area = getLocalBounds();
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(colour::text);
    g.drawText(name, area.removeFromLeft(44), juce::Justification::centredLeft, false);
    area.removeFromRight(0);
    auto peakArea = area.removeFromRight(76);
    g.setFont(type::mono(11.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(peak <= -59.9 ? juce::String("pk -inf") : "pk " + juce::String(peak, 1) + " dB", peakArea,
               juce::Justification::centredRight, false);
    auto levelArea = area.removeFromRight(64);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::text);
    g.drawText(levelText(level), levelArea, juce::Justification::centredRight, false);
  }

  void resized() override {
    auto area = getLocalBounds();
    area.removeFromLeft(44 + 12);
    area.removeFromRight(76 + 64 + 12);
    bar.setBounds(area.withSizeKeepingCentre(area.getWidth(), 8));
  }

private:
  juce::String name;
  LevelMeter bar;
  double level = -100.0, peak = -100.0;
};

// ####################
// NAM EDITOR
// ####################
NamEditor::NamEditor(EngineClient& client) : BlockEditorBody(client), eq(client) {
  setTitle("Capture editor");
  inputMeter = std::make_unique<MeterRow>("Input");
  outputMeter = std::make_unique<MeterRow>("Output");
  modelSize.setTitle("Model size");
  modelSize.setTooltip("Only A2 captures change with the model size");
  modelSize.onChange = [this](int index) {
    const double size = index == 0 ? 0.0 : index == 1 ? 0.5 : 1.0;
    engine.run("block.setSlimSize", object({{"blockId", context.blockId}, {"slimSize", size}}));
  };
  normalize.onToggle = [this](bool on) {
    engine.run("block.setParam", object({{"blockId", context.blockId}, {"param", "normalize"}, {"value", on ? 1.0 : 0.0}}));
  };
  eq.onChanged = [this] {
    if (onChanged)
      onChanged();
  };
  swap.onClick = [this] {
    if (onReplace)
      onReplace(2);
  };
  swap.setTooltip("Pick another capture for this block. Its settings stay.");
  loadFile.onClick = [this] { chooseFile(); };
  loadFile.setTooltip("Load a .nam file for this block. Its settings stay.");
  for (juce::Component* c : std::initializer_list<juce::Component*>{&modelSize, &normalize, &eq, inputMeter.get(),
                                                                    outputMeter.get(), &swap, &loadFile})
    addAndMakeVisible(c);
}

NamEditor::~NamEditor() = default;

void NamEditor::swapTo(const juce::File& file) {
  const auto reply = engine.run("chain.addToneFromFiles",
                                object({{"paths", juce::Array<juce::var>{file.getFullPathName()}}, {"target", context.blockId}}));
  if (!reply.ok) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load " + file.getFileName(),
                                           reply.errorMessage);
    return;
  }
  if (onChanged)
    onChanged();
}

void NamEditor::chooseFile() {
  chooser = std::make_unique<juce::FileChooser>("Load a NAM capture", juce::File(), "*.nam");
  juce::Component::SafePointer<NamEditor> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [self](const juce::FileChooser& picked) {
                         if (self != nullptr && picked.getResult().existsAsFile())
                           self->swapTo(picked.getResult());
                       });
}

void NamEditor::showBlock(const BlockContext& block) {
  const bool other = block.blockId != context.blockId;
  context = block;
  if (other) {
    inputMeter->resetPeak();
    outputMeter->resetPeak();
  }
  const juce::var tone = block.block["tone"];
  const juce::String key = block.blockId + "|" + block.block["activeModelId"].toString();
  if (key != assetKey) {
    assetKey = key;
    readAsset();
  }
  const double slim = block.block["params"]["slimSize"];
  modelSize.setSelected(slim < 0.25 ? 0 : slim < 0.75 ? 1 : 2);
  updateKnobs();
  eq.showBlock(block.blockId, block.block["params"]["eq"], "capture");
  tick();
  repaint();
}

void NamEditor::updateKnobs() {
  const juce::var listed = engine.read("block.listParams", object({{"blockId", context.blockId}}));
  std::vector<KnobParam> specs;
  if (const auto* list = listed["params"].getArray())
    for (const auto& item : *list) {
      KnobParam p = KnobParam::fromVar(item);
      p.sceneAssigned = context.sceneParams.contains(p.id);
      p.expressionAssigned = context.expressionParams.contains(p.id);
      if (p.id == "normalize")
        normalize.setState(p.value >= 0.5);
      if (p.id == "inputGain" || p.id == "outputGain" || p.id == "mix")
        specs.push_back(p);
    }
  const juce::String key = context.blockId;
  if (knobKey != key || knobs.size() != specs.size()) {
    knobKey = key;
    knobs.clear();
    for (const auto& spec : specs) {
      auto knob = std::make_unique<ParamKnob>(engine, context.blockId, spec);
      knob->setDiameter(kKnobDiameter);
      knob->setSize(kKnobCellWidth, ParamKnob::cellSize(kKnobDiameter).y);
      knob->onAssignmentChanged = [this] {
        if (onChanged)
          onChanged();
      };
      addAndMakeVisible(*knob);
      knobs.push_back(std::move(knob));
    }
    resized();
  }
  for (size_t i = 0; i < knobs.size() && i < specs.size(); ++i) {
    knobs[i]->update(specs[i]);
    knobs[i]->setBypassed(!context.enabled);
  }
}

void NamEditor::readAsset() {
  info = {};
  const juce::var tone = context.block["tone"];
  info.file = tone["title"].toString();
  info.source = static_cast<bool>(tone["local"]) ? "Local file" : "TONE3000";
  // A capture from TONE3000 shows what TONE3000's terms ask: a catalog tone's
  // summary, or what a downloaded file kept (chain.addToneFromFiles' tone3000).
  info.credit = static_cast<bool>(tone["local"]) ? Tone3000Credit::fromAttribution(tone["tone3000"])
                                                 : Tone3000Credit::fromSummary(tone);
  if (info.credit.known())
    info.source = "TONE3000";
  info.type = "Not in the file";
  info.calibration = "Not in the file";

  // The stash copy the block loaded, found among the captures by its path;
  // failing that, the library file of the same name.
  juce::File loaded;
  if (const auto* models = tone["models"].getArray())
    for (const auto& model : *models)
      if (model["id"].toString() == context.block["activeModelId"].toString())
        loaded = juce::URL(model["model_url"].toString()).getLocalFile();
  const juce::var assets = engine.read("assets.list", object({{"kind", "capture"}, {"limit", 1000}}));
  juce::var found, named;
  if (const auto* list = assets["assets"].getArray())
    for (const auto& asset : *list) {
      if (loaded != juce::File() && juce::File(asset["path"].toString()) == loaded)
        found = asset;
      if (asset["source"].toString() == "library" && asset["name"].toString() == info.file)
        named = asset;
    }
  if (found.isVoid())
    found = named;
  if (found.isVoid())
    return;
  info.type = gearTypeName(found["nam_gear_type"].toString());
  juce::StringArray tags;
  for (const auto& source : {found, named})
    if (const auto* list = source["tags"].getArray())
      for (const auto& tag : *list)
        tags.addIfNotAlreadyThere(tag.toString().toLowerCase());
  for (const char* tag : {"clean", "edge of breakup", "crunch", "high gain", "fuzz"})
    if (info.gainTag.isEmpty() && tags.contains(tag))
      info.gainTag = tag;
  if (info.gainTag.isEmpty() && !found["nam_gain"].isVoid())
    info.gainTag = "Gain " + juce::String(static_cast<double>(found["nam_gain"]), 1);
  if (info.gainTag.isNotEmpty())
    info.gainTag = info.gainTag.substring(0, 1).toUpperCase() + info.gainTag.substring(1);
  if (!found["nam_input_level_dbu"].isVoid() || !found["nam_output_level_dbu"].isVoid()) {
    juce::StringArray parts;
    if (!found["nam_input_level_dbu"].isVoid())
      parts.add("Input " + dbuText(found["nam_input_level_dbu"]));
    if (!found["nam_output_level_dbu"].isVoid())
      parts.add("Output " + dbuText(found["nam_output_level_dbu"]));
    info.calibration = parts.joinIntoString(" " + type::dot() + " ");
  }
}

void NamEditor::tick() {
  const juce::var levelsNow = engine.read("meters.get")["blocks"][juce::Identifier(context.blockId)];
  inputMeter->setLevel(levelOf(levelsNow["in"]));
  outputMeter->setLevel(levelOf(levelsNow["out"]));
}

// ####################
// LAYOUT
// ####################
void NamEditor::resized() {
  auto area = getLocalBounds();
  meters = area.removeFromBottom(kMeterBarHeight);
  area.removeFromBottom(kBarGap);
  capture = area.removeFromLeft(kCaptureWidth);
  area.removeFromLeft(kColumnGap);
  levels = area.removeFromLeft(kLevelsWidth);
  area.removeFromLeft(kColumnGap);
  eq.setBounds(area);

  // The capture panel: label and value pairs down the column.
  auto inner = capture.reduced(16);
  inner.removeFromTop(28);
  captureRows.clear();
  for (int i = 0; i < 6; ++i)
    captureRows.push_back(inner.removeFromTop(i == 4 ? kModelPitch : kFieldPitch));
  modelSize.setBounds(captureRows[4].withTrimmedTop(18).withHeight(36).withWidth(modelSize.idealWidth()));
  normalize.setBounds(inner.removeFromTop(32));

  // The levels: three knobs across, then the gate under a line.
  auto levelsInner = levels.reduced(16);
  levelsInner.removeFromTop(28);
  auto knobRow = levelsInner.removeFromTop(ParamKnob::cellSize(kKnobDiameter).y);
  const int spare = knobRow.getWidth() - kKnobCellWidth * static_cast<int>(knobs.size());
  const int gaps = juce::jmax(1, static_cast<int>(knobs.size()) - 1);
  for (size_t i = 0; i < knobs.size(); ++i)
    knobs[i]->setTopLeftPosition(knobRow.getX() + static_cast<int>(i) * (kKnobCellWidth + spare / gaps), knobRow.getY());
  levelsInner.removeFromTop(16);
  gate = levelsInner;

  auto bar = meters.reduced(16, 0);
  swap.setBounds(bar.removeFromRight(swap.idealWidth() + 4).withSizeKeepingCentre(swap.idealWidth() + 4, 36));
  bar.removeFromRight(12);
  loadFile.setBounds(bar.removeFromRight(loadFile.idealWidth() + 4).withSizeKeepingCentre(loadFile.idealWidth() + 4, 36));
  bar.removeFromRight(24);
  const int half = (bar.getWidth() - 24) / 2;
  inputMeter->setBounds(bar.removeFromLeft(half));
  bar.removeFromLeft(24);
  outputMeter->setBounds(bar);
}

void NamEditor::paint(juce::Graphics& g) {
  // The capture panel.
  paintSection(g, capture, "Capture");
  const auto rows = captureRows;
  if (rows.size() == 6) {
    const auto label = [&g](juce::Rectangle<int> row, const juce::String& text) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText(text, row.withHeight(16), juce::Justification::centredLeft, true);
    };
    const auto value = [&g](juce::Rectangle<int> row, const juce::String& text, bool mono = false) {
      g.setFont(mono ? type::mono(12.0f, type::Weight::regular) : type::sans(13.0f));
      g.setColour(colour::text);
      g.drawText(text, row.withTrimmedTop(18).withHeight(20), juce::Justification::centredLeft, true);
    };
    if (info.credit.known()) {
      // From TONE3000: the tone's image, title, gear and format, then its
      // creator with their avatar and its licence, over the File and Source rows.
      const auto& credit = info.credit;
      auto card = rows[0].getUnion(rows[1]);
      label(card, "From TONE3000");
      card.removeFromTop(20);
      auto head = card.removeFromTop(40);
      tone3000::drawArt(g, head.removeFromLeft(40).toFloat(), images.get(credit.imageUrl, false),
                        tone3000::categoryOf(credit.gear, credit.format), 6.0f);
      head.removeFromLeft(10);
      g.setFont(type::sans(13.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(info.file, head.removeFromTop(20), juce::Justification::centredLeft, true);
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText(tone3000::gearLabel(credit.gear) + "  " + type::dot() + "  " + tone3000::formatLabel(credit.format), head,
                 juce::Justification::centredLeft, true);
      card.removeFromTop(6);
      tone3000::drawCreatorLine(g, card.removeFromTop(18), credit, images.get(credit.avatarUrl, true));
    } else {
      label(rows[0], "File");
      value(rows[0], info.file + ".nam", true);
      label(rows[1], "Source");
      value(rows[1], info.source);
    }
    label(rows[2], "Type");
    value(rows[2], info.type);
    label(rows[3], "Gain tag");
    if (info.gainTag.isNotEmpty()) {
      const juce::Colour tint = categoryColour(context.category);
      const int width = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f, type::Weight::semibold), info.gainTag)) + 20;
      const auto pill = juce::Rectangle<float>(static_cast<float>(rows[3].getX()), static_cast<float>(rows[3].getY() + 20),
                                               static_cast<float>(width), 22.0f);
      g.setColour(tint.withAlpha(0.16f));
      g.fillRoundedRectangle(pill, 11.0f);
      g.setFont(type::sans(12.0f, type::Weight::semibold));
      g.setColour(tint);
      g.drawText(info.gainTag, pill, juce::Justification::centred, false);
    } else {
      value(rows[3], "Not tagged");
    }
    label(rows[4], "Model size");
    label(rows[5], "Calibration");
    value(rows[5], info.calibration, true);
  }

  // The levels and the gate.
  paintSection(g, levels, "Levels");
  g.setColour(colour::line);
  g.fillRect(gate.getX(), gate.getY() - 8, gate.getWidth(), 1);
  auto gateArea = gate;
  auto heading = gateArea.removeFromTop(24);
  paint::label(g, "Noise gate", heading);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(kLaterBuild, heading, juce::Justification::centredRight, false);
  gateArea.removeFromTop(8);
  g.drawFittedText("The capture block has no gate of its own yet. Put a Noise Gate block before it for now.",
                   gateArea.withHeight(48), juce::Justification::topLeft, 3);

  // The meter bar.
  g.setColour(colour::ground);
  g.fillRoundedRectangle(meters.toFloat(), size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(meters.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
}

}  // namespace pedaleira::ui
