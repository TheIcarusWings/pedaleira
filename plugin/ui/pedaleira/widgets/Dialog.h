// A question over a page: a title, a sentence, an optional text field and
// two buttons, on a dimmed ground. Used for destructive actions (delete a
// preset or a setlist asks first) and for names (rename, save as, new
// setlist). It belongs to the page that shows it, so a callback never
// outlives the page it will touch.
#pragma once

#include "Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

class Dialog : public juce::Component {
public:
  struct Options {
    juce::String title;
    juce::String message;
    juce::String confirm = "OK";
    // False for a notice with a single button.
    bool cancelable = true;
    // A name field, filled with `text`.
    bool withText = false;
    juce::String text;
    juce::String placeholder;
  };

  Dialog();

  // Shows the dialog over its parent; `done` gets whether it was confirmed
  // and the field's text. The dialog hides before the callback runs.
  void show(const Options& options, std::function<void(bool confirmed, const juce::String& text)> done);
  void hide();

  // The text field and the confirm action, for tests.
  juce::TextEditor& field() { return input; }
  void confirmNow() { finish(true); }

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent&) override {}
  bool keyPressed(const juce::KeyPress& key) override;

private:
  void finish(bool confirmed);
  juce::Rectangle<int> cardBounds() const;
  int messageHeight() const;

  Options current;
  std::function<void(bool, const juce::String&)> callback;
  juce::TextEditor input;
  PushButton cancel{"Cancel"};
  PushButton confirm{"OK", PushButton::Kind::primary};
};

}  // namespace pedaleira::ui
