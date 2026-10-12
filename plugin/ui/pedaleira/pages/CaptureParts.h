// The parts the capture wizard's seven steps share: what a step asks of the
// page (CaptureHost), the base of a step (CaptureStep), the drawing rules of
// the boards "Capture 1" to "Capture 7" and the few widgets they have that the
// other pages do not: the choice cards, the numbered checklist, the trainer
// card and the standard input file card.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "CaptureModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace pedaleira::ui {

// What capture.checkTrainer said, kept by the page for the steps that show it.
struct TrainerInfo {
  bool checked = false;
  bool busy = false;
  bool pythonFound = false;
  bool packageFound = false;
  bool versionOk = false;
  juce::String python, version, minimumVersion, message;

  bool usable() const { return checked && versionOk; }
};

// What a step asks of the page. The page owns the session and the footer.
class CaptureHost {
public:
  virtual ~CaptureHost() = default;

  virtual EngineClient& engine() = 0;
  // Runs a command; when it fails the player gets a notice titled
  // `failureTitle` (none when that is empty) and the reply says ok = false.
  virtual EngineReply run(const juce::String& command, const juce::var& args, const juce::String& failureTitle) = 0;
  // A session just made by capture.create: the page keeps its id.
  virtual void adopt(const juce::var& session) = 0;
  // capture.setStep; false (after a notice) when the engine refused.
  virtual bool moveTo(capture::Step step, const juce::var& extra = {}) = 0;
  // A command answered with the session: the page shows it.
  virtual void sessionChanged(const juce::var& session) = 0;
  // Reads the session again (a run finished).
  virtual void reload() = 0;
  // A step's footer text or state changed.
  virtual void footerChanged() = 0;
  // A capture was added to the rig or saved: the header and the grid read it again.
  virtual void rigChanged() = 0;
  virtual void notice(const juce::String& title, const juce::String& message) = 0;
  virtual void confirm(const juce::String& title, const juce::String& message, const juce::String& button,
                       std::function<void()> yes) = 0;
  // Runs `work` a moment later, after the screen drew (for the slow commands).
  virtual void later(std::function<void()> work) = 0;
  // Back one step.
  virtual void goBack() = 0;
  // Throws the session away and starts at the first step (after asking).
  virtual void cancelCapture() = 0;
  // The trainer's state, checked the first time a step wants it.
  virtual const TrainerInfo& trainer() = 0;
  virtual void checkTrainer() = 0;
};

// One step of the wizard, laid over the page between the stepper and the
// footer. It paints its own heading and cards; the page draws the footer
// buttons from footer().
class CaptureStep : public juce::Component {
public:
  struct Footer {
    bool showBack = true;
    bool backEnabled = true;
    juce::String primary;
    bool primaryEnabled = true;
    // A second button before the primary one ("Stop and retry").
    juce::String secondary;
    bool secondaryEnabled = true;
    bool showCancel = true;
  };

  explicit CaptureStep(CaptureHost& hostPage) : host(hostPage) {}

  // The session (capture.get) just changed; read it and lay out.
  virtual void show(const juce::var& newSession) = 0;
  // 10 times a second while the page is showing and this step is up.
  virtual void tick(int count) { juce::ignoreUnused(count); }
  // The step stops being the one in front, or the page is hidden: end
  // anything that plays audio.
  virtual void leave() {}
  // The page read the trainer's state again.
  virtual void trainerChanged() {}
  virtual Footer footer() const = 0;
  virtual void primary() = 0;
  virtual void secondary() {}
  virtual void back() { host.goBack(); }

protected:
  juce::String id() const { return session["id"].toString(); }

  CaptureHost& host;
  juce::var session;
};

namespace parts {

inline constexpr int kHeadingHeight = 58;
inline constexpr int kBlockGap = 24;
inline constexpr float kCardRadius = 12.0f;

int textWidth(const juce::Font& font, const juce::String& text);
// The height `text` takes wrapped at `width`.
int wrappedHeight(const juce::String& text, const juce::Font& font, int width);
// Wraps `text` in `area` and draws it.
void drawWrapped(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour colour,
                 juce::Rectangle<int> area);
// The step's title (28 bold) and the line under it.
void paintHeading(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle);
// A framed card of the boards: surface, a line, 12 px corners.
void paintCard(juce::Graphics& g, juce::Rectangle<int> area);
// A card label (12 caps) at the card's top left and, optionally, a mono value on its right.
void paintCardHeader(juce::Graphics& g, juce::Rectangle<int> card, const juce::String& label,
                     const juce::String& value = {}, int inset = 20);
void drawCheckbox(juce::Graphics& g, juce::Rectangle<float> box, bool on, bool enabled);
// A rounded pill with text (the boards' verdict and state pills).
void paintPill(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Colour fill,
               juce::Colour textColour = colour::ground);
int pillWidth(const juce::String& text, int height = 28);
// A small coloured dot, as the status lines use.
void paintDot(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour colour);
// Text fields: the ground fill, the line border, iris when focused.
void styleEditor(juce::TextEditor& editor, bool multiline = false);
// "Comes in a later build": the chip of a board element the engine cannot back yet.
void paintLater(juce::Graphics& g, juce::Rectangle<int> area);
inline const char* const kLater = "Comes in a later build";
// Opens a menu of choices under `box`; `chosen` gets the index.
void chooseFrom(SelectBox& box, const juce::StringArray& choices, int selected, std::function<void(int)> chosen);

}  // namespace parts

// A big card button of the first step: an icon, a name, what it is, a note
// and the list of what the player needs. Radio-like: the owner keeps one on.
class ChoiceCard : public juce::Button {
public:
  struct Spec {
    const juce::Path* icon = nullptr;
    juce::String name, description, note;
    juce::StringArray needs;
  };

  ChoiceCard(const juce::String& accessibleName, const Spec& cardSpec);

  void setChosen(bool on);
  bool isChosen() const { return chosen; }

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  Spec spec;
  bool chosen = false;
};

// Numbered steps with a checkbox each ("1 Volt Line out 3 to the re-amp box").
// The ticks are the player's own and live in the page only.
class Checklist : public juce::Component {
public:
  void setItems(const juce::StringArray& newItems);
  int doneCount() const;
  int count() const { return items.size(); }
  // The height the rows take at this width.
  int heightFor(int width) const;
  std::function<void()> onChange;

  void paint(juce::Graphics& g) override;
  void mouseUp(const juce::MouseEvent& event) override;

private:
  juce::StringArray items;
  std::vector<bool> done;
};

// A checkbox with its text.
class CheckBox : public juce::Button {
public:
  explicit CheckBox(const juce::String& text);

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
};

// The trainer card: whether Python and neural-amp-modeler 0.13.0 or newer
// are there, and, when they are not, what to install. Pedaleira never
// installs anything itself.
class TrainerPanel : public juce::Component {
public:
  explicit TrainerPanel(CaptureHost& hostPage);

  // Reads host.trainer() again.
  void update();
  // The height the card needs at this width.
  int heightFor(int width) const;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void choosePython();

  CaptureHost& host;
  PushButton again{"Check again"};
  PushButton pickPython{"Choose Python...", PushButton::Kind::ghost};
  std::unique_ptr<juce::FileChooser> chooser;
};

// The NAM standard input file: where it is, whether it is the version 3 file
// the trainer knows, and a way to choose it. A setting of the capture
// (capture.setSettings), never shipped and never downloaded.
class InputFilePanel : public juce::Component {
public:
  explicit InputFilePanel(CaptureHost& hostPage);

  // From capture.getSettings.
  void setSettings(const juce::var& settings);
  // Whether a recording can start with it: found and readable, and either the
  // known version or the player said to use it anyway.
  bool ready() const;
  bool exists() const { return found; }
  bool allowUnrecognised() const { return found && !recognised && useAnyway.getToggleState(); }
  // Seconds the file plays, 0 when unknown.
  double seconds() const { return length; }
  int heightFor(int width) const;
  std::function<void()> onChanged;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void chooseFile();

  CaptureHost& host;
  PushButton choose{"Choose file..."};
  CheckBox useAnyway{"Use it anyway"};
  std::unique_ptr<juce::FileChooser> chooser;
  juce::String path, message, version;
  bool found = false;
  bool readable = false;
  bool recognised = false;
  double length = 0.0;
};

}  // namespace pedaleira::ui
