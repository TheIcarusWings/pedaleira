// What the Settings sub pages share (the boards "Settings, audio" and the
// others of that family): the section base, the card, a switch with its label,
// a number with its unit and the small helpers around menus and text fields.
#pragma once

#include "../EngineClient.h"
#include "../Tokens.h"
#include "../widgets/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

// One sub page of Settings: a title and a line under it (painted by the
// page), and the cards below. The page gives it a width and asks for the
// height it needs at that width, so a section is a column that scrolls.
class SettingsSection : public juce::Component {
public:
  SettingsSection(EngineClient& client, const juce::String& heading, const juce::String& blurb)
      : engine(client), title(heading), note(blurb) {
    setTitle(heading);
  }

  const juce::String& heading() const { return title; }
  const juce::String& blurb() const { return note; }

  // Reads what it shows again (the page was opened, or something changed).
  virtual void refresh() {}
  // A few times a second while the page is showing: levels, scan progress.
  virtual void tick() {}
  // The height the section needs at this width.
  virtual int heightFor(int width) const = 0;

  // A sentence the page shows in a dialog: a command that was refused.
  std::function<void(const juce::String& title, const juce::String& message)> onNotice;
  // The section's content changed height; the page measures it again.
  std::function<void()> onHeightChanged;

protected:
  void heightChanged() {
    if (onHeightChanged)
      onHeightChanged();
  }
  // Runs a command; on a refusal tells the page and returns false.
  bool run(const juce::String& failTitle, const juce::String& command, const juce::var& args = {});
  void notice(const juce::String& noticeTitle, const juce::String& message) {
    if (onNotice)
      onNotice(noticeTitle, message);
  }

  EngineClient& engine;

private:
  juce::String title, note;
};

namespace settings {

constexpr int kCardPadding = 20;
constexpr int kCardGap = 16;
// A card's heading and the space under it, and the space under its content.
constexpr int kCardTop = kCardPadding + 22 + 14;
constexpr int kCardBottom = kCardPadding;
constexpr int kFieldHeight = 36;
// A label in caps over a field: 16 + 6 + 36.
constexpr int kLabelledField = 16 + 6 + kFieldHeight;

// The card of the boards: the surface with its line border and a 16/600
// heading. Returns the area under the heading for the content.
juce::Rectangle<int> paintCard(juce::Graphics& g, juce::Rectangle<int> box, const juce::String& heading);
// The area a card's content gets inside the box (paintCard's return value).
juce::Rectangle<int> cardContent(juce::Rectangle<int> box);

// A thin line under a row of a list.
void paintRowLine(juce::Graphics& g, juce::Rectangle<int> row);

// A text field in the boards' look (the search boxes).
void styleEditor(juce::TextEditor& editor, bool mono = false);

// Opens a menu of choices under a select box; `chosen` runs with the index.
// Safe against the box going away while the menu is open.
void chooseFromMenu(SelectBox& box, const juce::StringArray& items, int selected, std::function<void(int)> chosen);

// "Just now", "4 min ago", "21:15" or "10 Oct, 21:15" for an ISO 8601 time;
// empty for a time that does not parse.
juce::String agoText(const juce::String& iso);

// A level or progress bar: the track and the part of it that is filled.
void paintBar(juce::Graphics& g, juce::Rectangle<int> track, double fraction, juce::Colour fill);

// Runs `action` after the click that asked for it has returned, for an action
// that rebuilds the button that was clicked. Skipped if `owner` has gone.
void afterClick(juce::Component& owner, std::function<void()> action);

// A line of explanation, muted, wrapped inside `area`; returns the height used.
int paintNote(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, float size = 13.0f);
// The height paintNote needs for this text at this width.
int noteHeight(int width, const juce::String& text, float size = 13.0f);

// A dashed box saying a part is not built yet.
void paintLater(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text);

}  // namespace settings

// A switch with its label on the left (and a line under it, optionally); the
// switch is on the right, as on the boards.
class SettingToggle : public juce::Component {
public:
  explicit SettingToggle(const juce::String& label, const juce::String& detail = {});

  void setState(bool on);
  bool getState() const { return toggle.getToggleState(); }
  void setDetail(const juce::String& text);
  void setSwitchEnabled(bool enabled);
  // The player turned it; the new state.
  std::function<void(bool)> onToggle;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  Toggle toggle;
  juce::String label, detail;
};

// A small button with a line icon and no text (remove a row, open a menu);
// its name is what a screen reader says.
class IconButton : public juce::Button {
public:
  IconButton(const juce::String& accessibleName, const juce::Path* iconPath);

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  const juce::Path* icon;
};

// A number with its unit after it (the boards' "12.5 dBu"): a field that
// takes a typed value on Return or when it loses focus, clamped to its range.
class UnitField : public juce::Component, private juce::TextEditor::Listener {
public:
  UnitField(const juce::String& name, const juce::String& unit, int decimals = 1);

  void setRange(double lowest, double highest);
  // Shows a value without sending it; ignored while the player is typing.
  void setValue(double newValue);
  double value() const { return current; }
  std::function<void(double)> onCommit;

  void paint(juce::Graphics& g) override;
  void resized() override;
  void focusOfChildComponentChanged(FocusChangeType) override { repaint(); }

private:
  void textEditorReturnKeyPressed(juce::TextEditor&) override { commit(); }
  void textEditorFocusLost(juce::TextEditor&) override { commit(); }
  void textEditorEscapeKeyPressed(juce::TextEditor&) override;
  void commit();
  juce::String formatted(double v) const;

  juce::TextEditor input;
  juce::String unit;
  int decimals;
  double lowest = 0.0, highest = 1.0, current = 0.0;
};

}  // namespace pedaleira::ui
