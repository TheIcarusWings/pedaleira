#include "IrEditor.h"

#include "../Paint.h"
#include "../Type.h"

#include <algorithm>
#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kColumnGap = 20;
constexpr int kFileWidth = 280;
constexpr int kLevelsWidth = 340;
constexpr int kKnobDiameter = 72;
constexpr int kListRows = 6;
constexpr int kFolderRowHeight = 40;

juce::String thousands(juce::int64 value) {
  juce::String digits(value);
  juce::String out;
  for (int i = 0; i < digits.length(); ++i) {
    if (i > 0 && (digits.length() - i) % 3 == 0)
      out << ",";
    out << digits.substring(i, i + 1);
  }
  return out;
}

juce::String rateText(double hz) {
  juce::String khz(hz / 1000.0, 1);
  if (khz.endsWith(".0"))
    khz = khz.dropLastCharacters(2);
  return khz + " kHz";
}

juce::String lengthText(double seconds) { return juce::String(juce::roundToInt(seconds * 1000.0)) + " ms"; }

}  // namespace

// ####################
// FOLDER BUTTON
// ####################
// One IR of the folder: its name and length, the current one outlined.
class IrEditor::FolderButton : public juce::Button {
public:
  FolderButton(const juce::String& fileName, const juce::String& length, bool isCurrent)
      : juce::Button(fileName), name(fileName), duration(length), current(isCurrent) {
    setTitle(fileName);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus(true);
  }

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    const auto area = getLocalBounds().toFloat();
    g.setColour(current ? colour::control : (highlighted ? colour::control : colour::raised));
    g.fillRoundedRectangle(area, size::buttonRadius);
    g.setColour(current ? colour::iris : colour::line);
    g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
    auto inner = getLocalBounds().reduced(10, 0);
    g.setFont(type::mono(11.0f, type::Weight::regular));
    g.setColour(colour::muted);
    const int lengthWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(11.0f, type::Weight::regular), duration)) + 1;
    g.drawText(duration, inner.removeFromRight(lengthWidth), juce::Justification::centredRight, false);
    inner.removeFromRight(8);
    g.setFont(type::mono(12.0f));
    g.setColour(colour::text);
    g.drawText(name, inner, juce::Justification::centredLeft, true);
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
    }
  }

private:
  juce::String name, duration;
  bool current;
};

// ####################
// IR EDITOR
// ####################
IrEditor::IrEditor(EngineClient& client) : BlockEditorBody(client) {
  setTitle("IR editor");
  loadFile.onClick = [this] { chooseFile(); };
  addAndMakeVisible(loadFile);
}

IrEditor::~IrEditor() = default;

void IrEditor::showBlock(const BlockContext& block) {
  context = block;
  const juce::String key = block.blockId + "|" + block.block["activeModelId"].toString();
  if (key != assetKey) {
    assetKey = key;
    readAsset();
  }
  updateKnobs();
  repaint();
}

void IrEditor::updateKnobs() {
  const juce::var listed = engine.read("block.listParams", object({{"blockId", context.blockId}}));
  std::vector<KnobParam> specs;
  // Level, then mix, then the input.
  for (const char* wanted : {"outputGain", "mix", "inputGain"})
    if (const auto* list = listed["params"].getArray())
      for (const auto& item : *list)
        if (item["id"].toString() == wanted) {
          KnobParam p = KnobParam::fromVar(item);
          p.sceneAssigned = context.sceneParams.contains(p.id);
          p.expressionAssigned = context.expressionParams.contains(p.id);
          if (p.id == "outputGain")
            p.name = "Level";
          specs.push_back(p);
        }
  if (knobKey != context.blockId || knobs.size() != specs.size()) {
    knobKey = context.blockId;
    knobs.clear();
    for (const auto& spec : specs) {
      auto knob = std::make_unique<ParamKnob>(engine, context.blockId, spec);
      knob->setDiameter(kKnobDiameter);
      knob->setSize(ParamKnob::cellSize(kKnobDiameter).x, ParamKnob::cellSize(kKnobDiameter).y);
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

void IrEditor::readAsset() {
  info = {};
  alternates.clear();
  const juce::var tone = context.block["tone"];
  info.file = tone["title"].toString() + ".wav";
  info.length = info.sampleRate = info.channels = "Not known";
  info.folder = "Not in the library";
  info.listTitle = "In the library";

  juce::File loaded;
  if (const auto* models = tone["models"].getArray())
    for (const auto& model : *models)
      if (model["id"].toString() == context.block["activeModelId"].toString())
        loaded = juce::URL(model["model_url"].toString()).getLocalFile();
  const juce::var assets = engine.read("assets.list", object({{"kind", "ir"}, {"limit", 1000}}));
  juce::var found, named;
  std::vector<juce::var> library;
  if (const auto* list = assets["assets"].getArray())
    for (const auto& asset : *list) {
      if (loaded != juce::File() && juce::File(asset["path"].toString()) == loaded)
        found = asset;
      if (asset["source"].toString() == "library") {
        library.push_back(asset);
        if (asset["name"].toString() == tone["title"].toString())
          named = asset;
      }
    }
  const juce::var shown = found.isVoid() ? named : found;
  if (!shown.isVoid() && !shown["seconds"].isVoid()) {
    const double seconds = shown["seconds"], rate = shown["sampleRate"];
    info.length = lengthText(seconds);
    info.sampleRate = rateText(rate);
    info.samples = thousands(static_cast<juce::int64>(std::llround(seconds * rate))) + " samples";
    info.channels = static_cast<int>(shown["channels"]) == 1 ? juce::String("Mono")
                    : static_cast<int>(shown["channels"]) == 2 ? juce::String("Stereo")
                                                                : juce::String(static_cast<int>(shown["channels"])) + " channels";
  }

  // The other IRs of the folder the file came from, else the library's.
  std::vector<juce::var> siblings;
  if (!named.isVoid()) {
    const juce::File folder = juce::File(named["path"].toString()).getParentDirectory();
    info.folder = "From: " + folder.getFileName();
    info.listTitle = "In this folder";
    for (const auto& asset : library)
      if (juce::File(asset["path"].toString()).getParentDirectory() == folder)
        siblings.push_back(asset);
  } else {
    siblings = library;
  }
  std::sort(siblings.begin(), siblings.end(), [](const juce::var& a, const juce::var& b) {
    return a["name"].toString().compareNatural(b["name"].toString()) < 0;
  });
  // A window of the list that holds the current IR.
  int currentAt = 0;
  for (size_t i = 0; i < siblings.size(); ++i)
    if (siblings[i]["name"].toString() == tone["title"].toString())
      currentAt = static_cast<int>(i);
  const int first = juce::jlimit(0, juce::jmax(0, static_cast<int>(siblings.size()) - kListRows), currentAt - kListRows / 2);
  for (int i = first; i < static_cast<int>(siblings.size()) && i < first + kListRows; ++i) {
    Alternate alternate;
    alternate.name = siblings[static_cast<size_t>(i)]["name"].toString() + ".wav";
    alternate.path = siblings[static_cast<size_t>(i)]["path"].toString();
    alternate.length = siblings[static_cast<size_t>(i)]["seconds"].isVoid() ? juce::String()
                                                                            : lengthText(siblings[static_cast<size_t>(i)]["seconds"]);
    alternate.current = siblings[static_cast<size_t>(i)]["name"].toString() == tone["title"].toString();
    alternates.push_back(alternate);
  }

  folderButtons.clear();
  for (const auto& alternate : alternates) {
    auto button = std::make_unique<FolderButton>(alternate.name, alternate.length, alternate.current);
    const juce::File file(alternate.path);
    button->onClick = [this, file] { swapTo(file); };
    addAndMakeVisible(*button);
    folderButtons.push_back(std::move(button));
  }
  resized();
}

void IrEditor::swapTo(const juce::File& file) {
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

void IrEditor::chooseFile() {
  chooser = std::make_unique<juce::FileChooser>("Load an IR file", juce::File(), "*.wav");
  juce::Component::SafePointer<IrEditor> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [self](const juce::FileChooser& picked) {
                         if (self != nullptr && picked.getResult().existsAsFile())
                           self->swapTo(picked.getResult());
                       });
}

// ####################
// LAYOUT
// ####################
void IrEditor::resized() {
  auto area = getLocalBounds();
  filePanel = area.removeFromLeft(kFileWidth);
  area.removeFromLeft(kColumnGap);
  levelsPanel = area.removeFromRight(kLevelsWidth);
  area.removeFromRight(kColumnGap);
  previewPanel = area;

  // The file column: the card, the folder's IRs, the button at the bottom.
  auto inner = filePanel.reduced(16);
  inner.removeFromTop(28);
  fileCard = inner.removeFromTop(132);
  inner.removeFromTop(14);
  folderLabel = inner.removeFromTop(16);
  inner.removeFromTop(14);
  loadFile.setBounds(inner.removeFromBottom(36));
  for (auto& button : folderButtons) {
    button->setBounds(inner.removeFromTop(kFolderRowHeight));
    inner.removeFromTop(6);
  }

  // The preview column: two empty frames in the board's proportions.
  auto preview = previewPanel.reduced(16);
  preview.removeFromTop(28);
  waveFrame = preview.removeFromTop(juce::roundToInt(preview.getWidth() * 180.0f / 560.0f));
  preview.removeFromTop(16);
  responseLabel = preview.removeFromTop(16);
  preview.removeFromTop(10);
  responseFrame = preview.removeFromTop(juce::roundToInt(preview.getWidth() * 160.0f / 560.0f));

  // The levels column: the knobs three across, the notes under them.
  auto levels = levelsPanel.reduced(16);
  levels.removeFromTop(28);
  const int cell = levels.getWidth() / 3;
  int line = 0;
  for (size_t i = 0; i < knobs.size(); ++i) {
    const int column = static_cast<int>(i) % 3;
    if (i > 0 && column == 0)
      ++line;
    knobs[i]->setTopLeftPosition(levels.getX() + column * cell + (cell - knobs[i]->getWidth()) / 2,
                                 levels.getY() + line * (knobs[i]->getHeight() + 12));
  }
  levels.removeFromTop((line + 1) * (ParamKnob::cellSize(kKnobDiameter).y + 12));
  knobNote = levels.removeFromTop(44);
  levels.removeFromTop(14);
  dualDrop = levels.removeFromTop(juce::jmin(levels.getHeight(), 72));
}

void IrEditor::paint(juce::Graphics& g) {
  paintSection(g, filePanel, "IR file");
  paintSection(g, levelsPanel, "Shape and level");

  // The file card.
  g.setColour(colour::raised);
  g.fillRoundedRectangle(fileCard.toFloat(), size::panelRadius);
  auto card = fileCard.reduced(12);
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::text);
  g.drawFittedText(info.file, card.removeFromTop(32), juce::Justification::topLeft, 2);
  card.removeFromTop(10);
  const int half = card.getWidth() / 2;
  const auto cellOf = [&](int column, int row, const juce::String& name, const juce::String& value, bool mono) {
    const auto at = juce::Rectangle<int>(card.getX() + column * (half + 4), card.getY() + row * 34, half - 4, 32);
    g.setFont(type::sans(11.0f));
    g.setColour(colour::muted);
    g.drawText(name, at.withHeight(14), juce::Justification::centredLeft, true);
    g.setFont(mono ? type::mono(12.0f, type::Weight::regular) : type::sans(12.0f));
    g.setColour(colour::text);
    g.drawText(value, at.withTrimmedTop(14).withHeight(16), juce::Justification::centredLeft, true);
  };
  cellOf(0, 0, "Length", info.length, true);
  cellOf(1, 0, "Sample rate", info.sampleRate, true);
  cellOf(0, 1, "Channels", info.channels, true);
  cellOf(1, 1, "Folder", info.folder, false);
  paint::label(g, info.listTitle, folderLabel);

  // The preview column.
  {
    auto box = previewPanel;
    g.setColour(colour::ground);
    g.fillRoundedRectangle(box.toFloat(), size::panelRadius);
    g.setColour(colour::line);
    g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
    auto head = box.reduced(16).removeFromTop(16);
    paint::label(g, "Impulse response", head);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(info.samples, head, juce::Justification::centredRight, false);
    paint::label(g, "Frequency response", responseLabel);
    for (const auto& frame : {waveFrame, responseFrame}) {
      g.setColour(colour::surface);
      g.fillRoundedRectangle(frame.toFloat(), 8.0f);
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("Preview comes in a later build", frame, juce::Justification::centred, false);
    }
  }

  // The notes under the knobs.
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawFittedText("Low cut, high cut, start trim and length come in a later build.", knobNote,
                   juce::Justification::topLeft, 3);
  if (!dualDrop.isEmpty()) {
    paint::dashedOutline(g, dualDrop.toFloat(), size::panelRadius, colour::faint);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawFittedText("Dual IR, a second file for the right channel, and phase invert come in a later build.",
                     dualDrop.reduced(14, 8), juce::Justification::centred, 3);
  }
}

}  // namespace pedaleira::ui
