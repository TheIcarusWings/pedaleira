#include "SettingsParts.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <cmath>

namespace pedaleira::ui {

bool SettingsSection::run(const juce::String& failTitle, const juce::String& command, const juce::var& args) {
  const EngineReply reply = engine.run(command, args);
  if (!reply.ok)
    notice(failTitle, reply.errorMessage);
  return reply.ok;
}

// ####################
// HELPERS
// ####################
namespace settings {

juce::Rectangle<int> cardContent(juce::Rectangle<int> box) {
  return box.reduced(kCardPadding).withTrimmedTop(22 + 14);
}

juce::Rectangle<int> paintCard(juce::Graphics& g, juce::Rectangle<int> box, const juce::String& heading) {
  paint::panel(g, box.toFloat(), colour::surface, 12.0f);
  g.setFont(type::sans(16.0f, type::Weight::semibold));
  g.setColour(colour::text);
  g.drawText(heading, box.reduced(kCardPadding).removeFromTop(22), juce::Justification::centredLeft, true);
  return cardContent(box);
}

void paintRowLine(juce::Graphics& g, juce::Rectangle<int> row) {
  g.setColour(colour::line);
  g.fillRect(row.getX(), row.getBottom() - 1, row.getWidth(), 1);
}

void styleEditor(juce::TextEditor& editor, bool mono) {
  editor.setFont(mono ? type::mono(14.0f) : type::body());
  editor.setColour(juce::TextEditor::backgroundColourId, colour::raised);
  editor.setColour(juce::TextEditor::outlineColourId, colour::line);
  editor.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  editor.setColour(juce::TextEditor::textColourId, colour::text);
  editor.setColour(juce::CaretComponent::caretColourId, colour::text);
  editor.setColour(juce::TextEditor::highlightColourId, colour::iris.withAlpha(0.35f));
  editor.setIndents(12, 9);
}

void chooseFromMenu(SelectBox& box, const juce::StringArray& items, int selected, std::function<void(int)> chosen) {
  juce::PopupMenu menu;
  juce::Component::SafePointer<SelectBox> self(&box);
  for (int i = 0; i < items.size(); ++i)
    menu.addItem(items[i], true, i == selected, [self, chosen, i] {
      // The menu is open after the page went; do nothing then.
      if (self != nullptr)
        chosen(i);
    });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&box).withMinimumWidth(box.getWidth()));
}

void afterClick(juce::Component& owner, std::function<void()> action) {
  juce::Component::SafePointer<juce::Component> self(&owner);
  juce::MessageManager::callAsync([self, action = std::move(action)] {
    if (self != nullptr)
      action();
  });
}

juce::String agoText(const juce::String& iso) {
  if (iso.isEmpty())
    return {};
  const juce::Time then = juce::Time::fromISO8601(iso);
  if (then.toMilliseconds() == 0)
    return {};
  const juce::Time now = juce::Time::getCurrentTime();
  const juce::int64 seconds = (now.toMilliseconds() - then.toMilliseconds()) / 1000;
  if (seconds < 10)
    return "Just now";
  if (seconds < 60 * 60)
    return juce::String(juce::jmax<juce::int64>(1, seconds / 60)) + " min ago";
  const bool today = then.getDayOfMonth() == now.getDayOfMonth() && then.getMonth() == now.getMonth() &&
                     then.getYear() == now.getYear();
  return then.formatted(today ? "%H:%M" : "%d %b, %H:%M");
}

void paintBar(juce::Graphics& g, juce::Rectangle<int> track, double fraction, juce::Colour fill) {
  const auto bar = track.toFloat();
  g.setColour(colour::line);
  g.fillRoundedRectangle(bar, bar.getHeight() * 0.5f);
  const float width = bar.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, fraction));
  if (width > 0.5f) {
    g.setColour(fill);
    g.fillRoundedRectangle(bar.withWidth(juce::jmax(width, bar.getHeight())), bar.getHeight() * 0.5f);
  }
}

namespace {
juce::AttributedString noteText(const juce::String& text, float size) {
  juce::AttributedString out;
  out.setLineSpacing(2.0f);
  out.append(text, type::sans(size), colour::muted);
  return out;
}
}  // namespace

int noteHeight(int width, const juce::String& text, float size) {
  if (text.isEmpty() || width <= 0)
    return 0;
  juce::TextLayout layout;
  layout.createLayout(noteText(text, size), static_cast<float>(width));
  return juce::roundToInt(std::ceil(layout.getHeight())) + 2;
}

int paintNote(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, float size) {
  const int height = noteHeight(area.getWidth(), text, size);
  noteText(text, size).draw(g, area.withHeight(height).toFloat());
  return height;
}

void paintLater(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text) {
  paint::dashedOutline(g, area.toFloat(), size::panelRadius, colour::faint);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawFittedText(text, area.reduced(14, 6), juce::Justification::centred, 3);
}

}  // namespace settings

// ####################
// SETTING TOGGLE
// ####################
SettingToggle::SettingToggle(const juce::String& text, const juce::String& line)
    : toggle(text), label(text), detail(line) {
  toggle.onClick = [this] {
    if (onToggle)
      onToggle(toggle.getToggleState());
  };
  addAndMakeVisible(toggle);
}

void SettingToggle::setState(bool on) { toggle.setToggleState(on, juce::dontSendNotification); }

void SettingToggle::setDetail(const juce::String& text) {
  detail = text;
  repaint();
}

void SettingToggle::setSwitchEnabled(bool enabled) {
  toggle.setEnabled(enabled);
  setAlpha(enabled ? 1.0f : 0.5f);
}

void SettingToggle::paint(juce::Graphics& g) {
  auto area = getLocalBounds().withTrimmedRight(52);
  g.setFont(type::body());
  g.setColour(colour::text);
  if (detail.isEmpty()) {
    g.drawText(label, area, juce::Justification::centredLeft, true);
    return;
  }
  g.drawText(label, area.removeFromTop(20), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(detail, area.removeFromTop(16), juce::Justification::centredLeft, true);
}

void SettingToggle::resized() {
  toggle.setBounds(getWidth() - 40, (getHeight() - 28) / 2, 40, 28);
}

// ####################
// ICON BUTTON
// ####################
IconButton::IconButton(const juce::String& accessibleName, const juce::Path* iconPath)
    : juce::Button(accessibleName), icon(iconPath) {
  setTitle(accessibleName);
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool) {
  const auto area = getLocalBounds().toFloat();
  if (highlighted) {
    g.setColour(colour::control);
    g.fillRoundedRectangle(area, size::buttonRadius);
  }
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(area.reduced(1.0f), size::buttonRadius, 2.0f);
  }
  icons::draw(g, *icon, area.withSizeKeepingCentre(16.0f, 16.0f), highlighted ? colour::text : colour::muted, 2.0f);
}

// ####################
// UNIT FIELD
// ####################
UnitField::UnitField(const juce::String& name, const juce::String& unitText, int decimalPlaces)
    : unit(unitText), decimals(decimalPlaces) {
  setTitle(name);
  input.setTitle(name);
  input.setFont(type::mono(14.0f));
  input.setBorder(juce::BorderSize<int>(0));
  input.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
  input.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
  input.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
  input.setColour(juce::TextEditor::textColourId, colour::text);
  input.setColour(juce::CaretComponent::caretColourId, colour::text);
  input.setColour(juce::TextEditor::highlightColourId, colour::iris.withAlpha(0.35f));
  input.addListener(this);
  addAndMakeVisible(input);
  setValue(0.0);
}

void UnitField::setRange(double low, double high) {
  lowest = low;
  highest = high;
}

juce::String UnitField::formatted(double v) const { return juce::String(v, decimals); }

void UnitField::setValue(double newValue) {
  current = newValue;
  if (!input.hasKeyboardFocus(true))
    input.setText(formatted(current), false);
}

void UnitField::commit() {
  const juce::String text = input.getText().retainCharacters("0123456789.-+");
  if (text.containsAnyOf("0123456789")) {
    const double entered = juce::jlimit(lowest, highest, text.getDoubleValue());
    const bool changed = std::abs(entered - current) > 1e-9;
    current = entered;
    if (changed && onCommit)
      onCommit(current);
  }
  input.setText(formatted(current), false);
}

void UnitField::textEditorEscapeKeyPressed(juce::TextEditor&) {
  input.setText(formatted(current), false);
  input.giveAwayKeyboardFocus();
}

void UnitField::paint(juce::Graphics& g) {
  const auto area = getLocalBounds().toFloat();
  g.setColour(colour::raised);
  g.fillRoundedRectangle(area, size::buttonRadius);
  const bool focused = input.hasKeyboardFocus(true);
  g.setColour(focused ? colour::iris : colour::line);
  g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, focused ? 2.0f : 1.0f);
  if (unit.isNotEmpty()) {
    g.setFont(type::mono(13.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(unit, getLocalBounds().reduced(12, 0), juce::Justification::centredRight, false);
  }
}

void UnitField::resized() {
  const int unitWidth =
      unit.isEmpty() ? 0 : juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::mono(13.0f, type::Weight::regular), unit)) + 8;
  input.setBounds(getLocalBounds().withTrimmedLeft(12).withTrimmedRight(12 + unitWidth));
  input.setIndents(0, juce::jmax(0, (getHeight() - juce::roundToInt(type::mono(14.0f).getHeight())) / 2));
}

}  // namespace pedaleira::ui
