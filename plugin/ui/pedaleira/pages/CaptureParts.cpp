#include "CaptureParts.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <cmath>

namespace pedaleira::ui {

namespace parts {

int textWidth(const juce::Font& font, const juce::String& text) {
  return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text));
}

namespace {
juce::AttributedString attributed(const juce::String& text, const juce::Font& font, juce::Colour colour) {
  juce::AttributedString string;
  string.append(text, font, colour);
  string.setLineSpacing(2.0f);
  return string;
}
}  // namespace

int wrappedHeight(const juce::String& text, const juce::Font& font, int width) {
  if (text.isEmpty())
    return 0;
  juce::TextLayout layout;
  layout.createLayout(attributed(text, font, colour::text), static_cast<float>(juce::jmax(40, width)));
  return juce::roundToInt(std::ceil(layout.getHeight())) + 2;
}

void drawWrapped(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour textColour,
                 juce::Rectangle<int> area) {
  attributed(text, font, textColour).draw(g, area.toFloat());
}

void paintHeading(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle) {
  g.setFont(type::title());
  g.setColour(colour::text);
  g.drawText(title, area.getX(), area.getY(), area.getWidth(), 34, juce::Justification::centredLeft, true);
  g.setFont(type::body());
  g.setColour(colour::muted);
  g.drawText(subtitle, area.getX(), area.getY() + 38, area.getWidth(), 20, juce::Justification::centredLeft, true);
}

void paintCard(juce::Graphics& g, juce::Rectangle<int> area) { paint::panel(g, area.toFloat(), colour::surface, kCardRadius); }

void paintCardHeader(juce::Graphics& g, juce::Rectangle<int> card, const juce::String& label, const juce::String& value,
                     int inset) {
  const auto row = juce::Rectangle<int>(card.getX() + inset, card.getY() + 16, card.getWidth() - 2 * inset, 16);
  paint::label(g, label, row);
  if (value.isNotEmpty()) {
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(value, row, juce::Justification::centredRight, true);
  }
}

void drawCheckbox(juce::Graphics& g, juce::Rectangle<float> box, bool on, bool enabled) {
  if (on) {
    g.setColour(colour::iris);
    g.fillRoundedRectangle(box, 4.0f);
    icons::draw(g, icons::path(icons::Icon::check), box.reduced(2.0f), colour::ground, 2.5f);
  } else if (enabled) {
    g.setColour(colour::faint);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.5f);
  } else {
    paint::dashedOutline(g, box, 4.0f, colour::line);
  }
}

int pillWidth(const juce::String& text, int height) {
  return textWidth(type::sans(height >= 28 ? 13.0f : 12.0f, type::Weight::semibold), text) + (height >= 28 ? 24 : 20);
}

void paintPill(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Colour fill,
               juce::Colour textColour) {
  g.setColour(fill);
  g.fillRoundedRectangle(area.toFloat(), area.getHeight() * 0.5f);
  g.setFont(type::sans(area.getHeight() >= 28 ? 13.0f : 12.0f, type::Weight::bold));
  g.setColour(textColour);
  g.drawText(text, area, juce::Justification::centred, false);
}

void paintDot(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour dot) {
  g.setColour(dot);
  g.fillEllipse(area.toFloat().withSizeKeepingCentre(10.0f, 10.0f));
}

void styleEditor(juce::TextEditor& editor, bool multiline) {
  editor.setMultiLine(multiline, multiline);
  editor.setReturnKeyStartsNewLine(multiline);
  editor.setFont(type::sans(14.0f, type::Weight::medium));
  editor.setColour(juce::TextEditor::backgroundColourId, colour::ground);
  editor.setColour(juce::TextEditor::outlineColourId, colour::line);
  editor.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  editor.setColour(juce::TextEditor::textColourId, colour::text);
  editor.setColour(juce::TextEditor::highlightColourId, colour::iris.withAlpha(0.35f));
  editor.setColour(juce::CaretComponent::caretColourId, colour::irisHover);
  editor.setIndents(12, multiline ? 8 : 10);
  editor.setScrollbarsShown(multiline);
}

void paintLater(juce::Graphics& g, juce::Rectangle<int> area) {
  paint::chip(g, kLater, area.toFloat(), paint::ChipStyle::dashed);
}

void chooseFrom(SelectBox& box, const juce::StringArray& choices, int selected, std::function<void(int)> chosen) {
  juce::PopupMenu menu;
  juce::Component::SafePointer<SelectBox> where(&box);
  for (int i = 0; i < choices.size(); ++i)
    menu.addItem(choices[i], true, i == selected, [where, chosen, i] {
      if (where != nullptr)
        chosen(i);
    });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&box));
}

}  // namespace parts

// ####################
// CHOICE CARD
// ####################
ChoiceCard::ChoiceCard(const juce::String& accessibleName, const Spec& cardSpec) : juce::Button(accessibleName), spec(cardSpec) {
  setTitle(accessibleName);
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void ChoiceCard::setChosen(bool on) {
  if (chosen == on)
    return;
  chosen = on;
  repaint();
}

void ChoiceCard::paintButton(juce::Graphics& g, bool highlighted, bool) {
  const auto area = getLocalBounds().withTrimmedBottom(static_cast<int>(size::dropDepth));
  paint::raised(g, area.toFloat(), parts::kCardRadius, colour::surface);
  g.setColour(chosen ? colour::iris : highlighted ? colour::faint : colour::line);
  g.drawRoundedRectangle(area.toFloat().reduced(chosen ? 1.0f : 0.5f), parts::kCardRadius, chosen ? 2.0f : 1.0f);
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::irisHover);
    g.drawRoundedRectangle(area.toFloat().reduced(3.0f), parts::kCardRadius - 2.0f, 1.5f);
  }

  auto inner = area.reduced(20);
  if (spec.icon != nullptr)
    icons::draw(g, *spec.icon, inner.removeFromTop(48).removeFromLeft(48).toFloat(), chosen ? colour::iris : colour::text);
  inner.removeFromTop(14);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(spec.name, inner.removeFromTop(26), juce::Justification::centredLeft, true);
  inner.removeFromTop(4);
  const int descriptionHeight = parts::wrappedHeight(spec.description, type::body(), inner.getWidth());
  parts::drawWrapped(g, spec.description, type::body(), colour::muted, inner.removeFromTop(descriptionHeight));
  if (spec.note.isNotEmpty()) {
    inner.removeFromTop(4);
    const int noteHeight = parts::wrappedHeight(spec.note, type::sans(12.0f), inner.getWidth());
    parts::drawWrapped(g, spec.note, type::sans(12.0f), colour::muted, inner.removeFromTop(noteHeight));
  }

  const int count = spec.needs.size();
  const int needsHeight = 14 + 6 + count * 18 + juce::jmax(0, count - 1) * 2;
  auto needs = inner.removeFromBottom(needsHeight);
  paint::label(g, "You need:", needs.removeFromTop(14));
  needs.removeFromTop(6);
  g.setFont(type::sans(13.0f));
  for (const auto& need : spec.needs) {
    auto row = needs.removeFromTop(18);
    g.setColour(colour::faint);
    g.fillEllipse(row.removeFromLeft(4).toFloat().withSizeKeepingCentre(4.0f, 4.0f));
    row.removeFromLeft(8);
    g.setColour(colour::muted);
    g.drawText(need, row, juce::Justification::centredLeft, true);
    needs.removeFromTop(2);
  }

  if (chosen) {
    const auto badge = juce::Rectangle<float>(static_cast<float>(area.getRight() - 14 - 24), static_cast<float>(area.getY() + 14), 24.0f, 24.0f);
    g.setColour(colour::iris);
    g.fillEllipse(badge);
    icons::draw(g, icons::path(icons::Icon::check), badge.reduced(5.0f), colour::ground, 2.5f);
  }
}

// ####################
// CHECKLIST
// ####################
namespace {
constexpr int kCheckRowGap = 6;
constexpr int kCheckTextLeft = 18 + 10;

int checkTextWidth(int width) { return width - kCheckTextLeft; }
}  // namespace

namespace {
constexpr int kCheckNumberWidth = 16;

// The rows of a checklist: each as tall as its wrapped text, at least a checkbox.
std::vector<juce::Rectangle<int>> checklistRows(const juce::StringArray& items, int width) {
  std::vector<juce::Rectangle<int>> rows;
  int y = 0;
  for (const auto& item : items) {
    const int height = juce::jmax(18, parts::wrappedHeight(item, type::sans(13.0f), checkTextWidth(width) - kCheckNumberWidth));
    rows.emplace_back(0, y, width, height);
    y += height + kCheckRowGap;
  }
  return rows;
}
}  // namespace

void Checklist::setItems(const juce::StringArray& newItems) {
  items = newItems;
  done.assign(static_cast<size_t>(items.size()), false);
  repaint();
}

int Checklist::heightFor(int width) const {
  const auto rows = checklistRows(items, width);
  return rows.empty() ? 0 : rows.back().getBottom();
}

int Checklist::doneCount() const {
  int n = 0;
  for (const bool tick : done)
    if (tick)
      ++n;
  return n;
}

void Checklist::paint(juce::Graphics& g) {
  const auto rows = checklistRows(items, getWidth());
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto row = rows[i];
    parts::drawCheckbox(g, juce::Rectangle<float>(static_cast<float>(row.getX()), static_cast<float>(row.getY()), 18.0f, 18.0f),
                        done[i], true);
    const auto text = row.withTrimmedLeft(kCheckTextLeft);
    g.setFont(type::mono(13.0f));
    g.setColour(colour::iris);
    g.drawText(juce::String(static_cast<int>(i) + 1), text.getX(), text.getY(), kCheckNumberWidth, 18, juce::Justification::centredLeft,
               false);
    parts::drawWrapped(g, items[static_cast<int>(i)], type::sans(13.0f), done[i] ? colour::muted : colour::text,
                       text.withTrimmedLeft(kCheckNumberWidth));
  }
}

void Checklist::mouseUp(const juce::MouseEvent& event) {
  const auto rows = checklistRows(items, getWidth());
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].contains(event.getPosition())) {
      done[i] = !done[i];
      repaint();
      if (onChange)
        onChange();
      return;
    }
}

// ####################
// CHECK BOX
// ####################
CheckBox::CheckBox(const juce::String& text) : juce::Button(text) {
  setTitle(text);
  setClickingTogglesState(true);
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void CheckBox::paintButton(juce::Graphics& g, bool highlighted, bool) {
  const float alpha = isEnabled() ? 1.0f : 0.45f;
  auto area = getLocalBounds();
  const auto box = area.removeFromLeft(18).withSizeKeepingCentre(18, 18).toFloat();
  parts::drawCheckbox(g, box, getToggleState(), isEnabled());
  area.removeFromLeft(10);
  g.setFont(type::sans(13.0f));
  g.setColour((highlighted ? colour::text : colour::muted).withMultipliedAlpha(alpha));
  g.drawText(getButtonText(), area, juce::Justification::centredLeft, true);
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::irisHover);
    g.drawRoundedRectangle(box.expanded(2.0f), 5.0f, 1.5f);
  }
}

// ####################
// TRAINER PANEL
// ####################
namespace {
constexpr const char* kInstallCommand = "python3 -m pip install --upgrade \"neural-amp-modeler>=0.13.0\"";
}

TrainerPanel::TrainerPanel(CaptureHost& hostPage) : host(hostPage) {
  setTitle("Trainer");
  again.onClick = [this] { host.checkTrainer(); };
  pickPython.onClick = [this] { choosePython(); };
  addAndMakeVisible(again);
  addAndMakeVisible(pickPython);
}

void TrainerPanel::update() {
  const TrainerInfo& info = host.trainer();
  again.setEnabled(!info.busy);
  pickPython.setVisible(info.checked && !info.usable());
  resized();
  repaint();
}

int TrainerPanel::heightFor(int width) const {
  const TrainerInfo& info = host.trainer();
  const int inner = width - 40;
  int height = 16 + 16 + 12 + 22;
  if (!info.checked || info.busy) {
    height += 0;
  } else if (info.usable()) {
    height += 6 + 16;
  } else {
    height += 8 + parts::wrappedHeight(info.message, type::compact(), inner) + 12 + 18 + 6 + 34 + 6 +
              parts::wrappedHeight("Pedaleira never installs anything by itself. If the trainer is installed under another Python, "
                                   "choose that Python.",
                                   type::sans(12.0f), inner);
  }
  return height + 14 + 36 + 16;
}

void TrainerPanel::resized() {
  auto row = getLocalBounds().reduced(20, 0).removeFromBottom(16 + 36).removeFromTop(36);
  again.setBounds(row.removeFromLeft(again.idealWidth() + 4));
  row.removeFromLeft(8);
  pickPython.setBounds(row.removeFromLeft(pickPython.idealWidth() + 4));
}

void TrainerPanel::paint(juce::Graphics& g) {
  parts::paintCard(g, getLocalBounds());
  parts::paintCardHeader(g, getLocalBounds(), "Trainer");
  const TrainerInfo& info = host.trainer();
  auto inner = getLocalBounds().reduced(20, 0).withTrimmedTop(16 + 16 + 12);
  auto status = inner.removeFromTop(22);
  juce::Colour dot = colour::faint;
  juce::String text = "Checking the trainer...";
  if (info.checked && !info.busy) {
    dot = info.usable() ? colour::ok : colour::warn;
    text = info.usable() ? "Ready to train" : "Not ready yet";
  }
  parts::paintDot(g, status.removeFromLeft(10), dot);
  status.removeFromLeft(10);
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText(text, status, juce::Justification::centredLeft, true);
  if (!info.checked || info.busy)
    return;
  if (info.usable()) {
    inner.removeFromTop(6);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText("neural-amp-modeler " + info.version + " " + type::dot() + " " + info.python, inner.removeFromTop(16),
               juce::Justification::centredLeft, true);
    return;
  }
  inner.removeFromTop(8);
  parts::drawWrapped(g, info.message, type::compact(), colour::muted,
                     inner.removeFromTop(parts::wrappedHeight(info.message, type::compact(), inner.getWidth())));
  inner.removeFromTop(12);
  g.setFont(type::compact());
  g.setColour(colour::text);
  g.drawText("To install it, run this in a terminal:", inner.removeFromTop(18), juce::Justification::centredLeft, true);
  inner.removeFromTop(6);
  const auto box = inner.removeFromTop(34);
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
  g.setFont(type::mono(11.0f, type::Weight::regular));
  g.setColour(colour::text);
  g.drawText(kInstallCommand, box.reduced(10, 0), juce::Justification::centredLeft, true);
  inner.removeFromTop(6);
  const juce::String note =
      "Pedaleira never installs anything by itself. If the trainer is installed under another Python, choose that Python.";
  parts::drawWrapped(g, note, type::sans(12.0f), colour::muted, inner.removeFromTop(parts::wrappedHeight(note, type::sans(12.0f), inner.getWidth())));
}

void TrainerPanel::choosePython() {
  chooser = std::make_unique<juce::FileChooser>("Choose the Python that has neural-amp-modeler",
                                                juce::File::getSpecialLocation(juce::File::userHomeDirectory));
  juce::Component::SafePointer<TrainerPanel> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || !picked.getResult().existsAsFile())
                           return;
                         const EngineReply reply = self->host.run(
                             "capture.setSettings", object({{"pythonExecutable", picked.getResult().getFullPathName()}}),
                             "Could not use that Python");
                         if (reply.ok)
                           self->host.checkTrainer();
                       });
}

// ####################
// INPUT FILE PANEL
// ####################
InputFilePanel::InputFilePanel(CaptureHost& hostPage) : host(hostPage) {
  setTitle("Standard input file");
  choose.onClick = [this] { chooseFile(); };
  useAnyway.onClick = [this] {
    if (onChanged)
      onChanged();
  };
  addAndMakeVisible(choose);
  addChildComponent(useAnyway);
}

void InputFilePanel::setSettings(const juce::var& settings) {
  const juce::var input = settings["standardInput"];
  path = settings["standardInputFile"].toString();
  found = static_cast<bool>(input["exists"]);
  readable = static_cast<bool>(input["readable"]);
  recognised = static_cast<bool>(input["recognised"]);
  message = input["message"].toString();
  version = input["version"].toString();
  length = static_cast<double>(input["seconds"]);
  choose.setButtonText(found ? "Change file..." : "Choose file...");
  choose.setKind(found ? PushButton::Kind::ghost : PushButton::Kind::secondary);
  useAnyway.setVisible(found && readable && !recognised);
  resized();
  repaint();
}

bool InputFilePanel::ready() const { return found && readable && (recognised || useAnyway.getToggleState()); }

int InputFilePanel::heightFor(int width) const {
  const int inner = width - 40;
  int height = 16 + 16 + 12 + 22 + 8;
  if (!found)
    height += parts::wrappedHeight("The re-amp plays the NAM project's standard input file (v3_0_0.wav). It is published by the "
                                   "NAM project and not shipped with Pedaleira: download it, then choose it here.",
                                   type::compact(), inner);
  else
    height += 16;
  if (found && !recognised)
    height += 6 + parts::wrappedHeight(message, type::sans(12.0f), inner);
  return height + 12 + 36 + 16;
}

void InputFilePanel::resized() {
  auto row = getLocalBounds().reduced(20, 0).removeFromBottom(16 + 36).removeFromTop(36);
  choose.setBounds(row.removeFromLeft(choose.idealWidth() + 4));
  row.removeFromLeft(16);
  useAnyway.setBounds(row.removeFromLeft(140).withHeight(24).withY(row.getY() + 6));
}

void InputFilePanel::paint(juce::Graphics& g) {
  parts::paintCard(g, getLocalBounds());
  parts::paintCardHeader(g, getLocalBounds(), "Standard input file");
  auto inner = getLocalBounds().reduced(20, 0).withTrimmedTop(16 + 16 + 12);
  auto status = inner.removeFromTop(22);
  juce::Colour dot = colour::warn;
  juce::String text = path.isEmpty() ? "Not chosen yet" : "File not found";
  if (found && !readable) {
    text = "Cannot be read";
  } else if (found && recognised) {
    dot = colour::ok;
    text = "NAM standard input, version " + (version.isEmpty() ? juce::String("3") : version);
  } else if (found) {
    text = "Not a file the trainer knows";
  }
  parts::paintDot(g, status.removeFromLeft(10), dot);
  status.removeFromLeft(10);
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText(text, status, juce::Justification::centredLeft, true);
  inner.removeFromTop(8);
  if (!found) {
    const juce::String help =
        "The re-amp plays the NAM project's standard input file (v3_0_0.wav). It is published by the NAM project and not shipped "
        "with Pedaleira: download it, then choose it here.";
    parts::drawWrapped(g, help, type::compact(), colour::muted, inner.removeFromTop(parts::wrappedHeight(help, type::compact(), inner.getWidth())));
    return;
  }
  g.setFont(type::mono(11.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText(path + (length > 0.0 ? "  " + type::dot() + "  " + capture::clockText(length) : juce::String()), inner.removeFromTop(16),
             juce::Justification::centredLeft, true);
  if (!recognised) {
    inner.removeFromTop(6);
    parts::drawWrapped(g, message, type::sans(12.0f), colour::warn, inner.removeFromTop(parts::wrappedHeight(message, type::sans(12.0f), inner.getWidth())));
  }
}

void InputFilePanel::chooseFile() {
  chooser = std::make_unique<juce::FileChooser>("Choose the NAM standard input file (v3_0_0.wav)",
                                                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.wav");
  juce::Component::SafePointer<InputFilePanel> self(this);
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [self](const juce::FileChooser& picked) {
                         if (self == nullptr || !picked.getResult().existsAsFile())
                           return;
                         const EngineReply reply =
                             self->host.run("capture.setSettings", object({{"standardInputFile", picked.getResult().getFullPathName()}}),
                                            "Could not use that file");
                         if (!reply.ok)
                           return;
                         self->setSettings(reply.value);
                         if (self->onChanged)
                           self->onChanged();
                       });
}

}  // namespace pedaleira::ui
