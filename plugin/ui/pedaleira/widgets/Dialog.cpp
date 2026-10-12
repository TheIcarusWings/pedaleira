#include "Dialog.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {
constexpr int kCardWidth = 440;
constexpr int kPadding = 24;
}  // namespace

Dialog::Dialog() {
  setVisible(false);
  setWantsKeyboardFocus(true);
  setInterceptsMouseClicks(true, true);
  input.setFont(type::sans(14.0f));
  input.setColour(juce::TextEditor::backgroundColourId, colour::ground);
  input.setColour(juce::TextEditor::outlineColourId, colour::line);
  input.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  input.setColour(juce::TextEditor::textColourId, colour::text);
  input.setIndents(12, 9);
  input.onReturnKey = [this] { finish(true); };
  input.onEscapeKey = [this] { finish(false); };
  cancel.onClick = [this] { finish(false); };
  confirm.onClick = [this] { finish(true); };
  addChildComponent(input);
  addAndMakeVisible(cancel);
  addAndMakeVisible(confirm);
}

void Dialog::show(const Options& options, std::function<void(bool, const juce::String&)> done) {
  current = options;
  callback = std::move(done);
  setTitle(options.title);
  confirm.setButtonText(options.confirm);
  cancel.setVisible(options.cancelable);
  input.setVisible(options.withText);
  input.setTextToShowWhenEmpty(options.placeholder, colour::faint);
  input.setText(options.text, false);
  if (auto* parent = getParentComponent()) {
    setBounds(parent->getLocalBounds());
    toFront(false);
  }
  setVisible(true);
  resized();
  repaint();
  if (options.withText) {
    input.grabKeyboardFocus();
    input.selectAll();
  } else {
    grabKeyboardFocus();
  }
}

void Dialog::hide() {
  setVisible(false);
  callback = nullptr;
}

void Dialog::finish(bool confirmed) {
  if (!isVisible())
    return;
  const juce::String text = input.getText().trim();
  // A name field cannot be confirmed empty.
  if (confirmed && current.withText && text.isEmpty())
    return;
  auto done = std::move(callback);
  hide();
  if (done)
    done(confirmed, text);
}

int Dialog::messageHeight() const {
  if (current.message.isEmpty())
    return 0;
  juce::AttributedString text;
  text.append(current.message, type::body(), colour::muted);
  juce::TextLayout layout;
  layout.createLayout(text, static_cast<float>(kCardWidth - 2 * kPadding));
  return juce::roundToInt(std::ceil(layout.getHeight())) + 4;
}

juce::Rectangle<int> Dialog::cardBounds() const {
  const int height = kPadding + 28 + (current.message.isEmpty() ? 0 : 8 + messageHeight()) +
                     (current.withText ? 16 + 38 : 0) + 24 + 36 + kPadding;
  return juce::Rectangle<int>(kCardWidth, height).withCentre(getLocalBounds().getCentre());
}

void Dialog::resized() {
  const auto card = cardBounds();
  auto inner = card.reduced(kPadding);
  auto buttons = inner.removeFromBottom(36);
  confirm.setBounds(buttons.removeFromRight(juce::jmax(88, confirm.idealWidth())));
  buttons.removeFromRight(10);
  cancel.setBounds(buttons.removeFromRight(juce::jmax(88, cancel.idealWidth())));
  if (current.withText)
    input.setBounds(inner.withTrimmedBottom(24).removeFromBottom(38));
}

void Dialog::paint(juce::Graphics& g) {
  g.fillAll(colour::ground.withAlpha(0.72f));
  const auto card = cardBounds();
  paint::raised(g, card.toFloat(), size::panelRadius, colour::surface);
  g.setColour(colour::line);
  g.drawRoundedRectangle(card.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
  auto inner = card.reduced(kPadding);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(current.title, inner.removeFromTop(28), juce::Justification::centredLeft, true);
  if (current.message.isNotEmpty()) {
    inner.removeFromTop(8);
    juce::AttributedString text;
    text.append(current.message, type::body(), colour::muted);
    text.draw(g, inner.removeFromTop(messageHeight()).toFloat());
  }
}

bool Dialog::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey && current.cancelable) {
    finish(false);
    return true;
  }
  if (key == juce::KeyPress::returnKey) {
    finish(true);
    return true;
  }
  return false;
}

}  // namespace pedaleira::ui
