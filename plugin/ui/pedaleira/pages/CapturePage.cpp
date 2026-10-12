#include "CapturePage.h"

#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {
constexpr int kStepperHeight = 72;
constexpr int kFooterHeight = 72;
constexpr int kMarginX = 40;
constexpr int kMarginY = 32;
constexpr int kButtonHeight = 40;
}  // namespace

// ####################
// SETUP
// ####################
CapturePage::CapturePage(EngineClient& engineClient) : client(engineClient) {
  setTitle("Capture");
  setWantsKeyboardFocus(true);

  for (const capture::Step step : {capture::Step::source, capture::Step::wiring, capture::Step::levels, capture::Step::record,
                                   capture::Step::check, capture::Step::train, capture::Step::review})
    addChildComponent(view(step));
  for (juce::Component* c : std::initializer_list<juce::Component*>{&cancel, &backButton, &secondaryButton, &primaryButton})
    addChildComponent(c);
  addChildComponent(prompt);

  cancel.onClick = [this] { cancelCapture(); };
  backButton.onClick = [this] { view(shown).back(); };
  secondaryButton.onClick = [this] { view(shown).secondary(); };
  primaryButton.onClick = [this] { view(shown).primary(); };

  view(shown).setVisible(true);
  view(shown).show({});
  layoutFooter();
}

CapturePage::~CapturePage() {
  stopTimer();
  // A tone left playing would keep the rig muted.
  view(shown).leave();
}

CaptureStep& CapturePage::view(capture::Step step) {
  switch (step) {
    case capture::Step::source: return sourceStep;
    case capture::Step::wiring: return wiringStep;
    case capture::Step::levels: return levelsStep;
    case capture::Step::record: return recordStep;
    case capture::Step::check: return checkStep;
    case capture::Step::train: return trainStep;
    case capture::Step::review: return reviewStep;
  }
  return sourceStep;
}

// ####################
// THE SESSION
// ####################
void CapturePage::refresh() {
  if (currentId.isNotEmpty()) {
    reload();
    return;
  }
  // The capture in progress, newest first; a saved one is done.
  const juce::var list = client.read("capture.list");
  if (const auto* captures = list["captures"].getArray())
    for (const auto& entry : *captures)
      if (entry["savedFile"].toString().isEmpty()) {
        open(entry["id"].toString());
        return;
      }
}

void CapturePage::open(const juce::String& id) {
  currentId = id;
  reload();
}

void CapturePage::reload() {
  if (currentId.isEmpty())
    return;
  const EngineReply reply = client.run("capture.get", object({{"id", currentId}}));
  if (!reply.ok) {
    reset();
    return;
  }
  sessionChanged(reply.value);
}

void CapturePage::adopt(const juce::var& newSession) {
  session = newSession;
  currentId = newSession["id"].toString();
}

void CapturePage::sessionChanged(const juce::var& newSession) {
  if (!newSession.isObject()) {
    reset();
    return;
  }
  session = newSession;
  currentId = newSession["id"].toString();
  present();
}

bool CapturePage::moveTo(capture::Step step, const juce::var& extra) {
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("id", currentId);
  args->setProperty("step", capture::stepId(step));
  if (const auto* properties = extra.getDynamicObject())
    for (const auto& property : properties->getProperties())
      args->setProperty(property.name, property.value);
  const EngineReply reply = run("capture.setStep", juce::var(args.get()), "Cannot go on yet");
  if (!reply.ok)
    return false;
  sessionChanged(reply.value);
  return true;
}

void CapturePage::goBack() {
  const int index = static_cast<int>(shown);
  if (index > 0)
    moveTo(static_cast<capture::Step>(index - 1), {});
}

void CapturePage::reset() {
  currentId.clear();
  session = juce::var();
  present();
}

void CapturePage::present() {
  const capture::Step step = currentId.isEmpty() ? capture::Step::source : capture::stepFromId(session["step"].toString());
  if (step != shown)
    view(shown).leave();
  shown = step;
  for (int i = 0; i < capture::kStepCount; ++i)
    view(static_cast<capture::Step>(i)).setVisible(static_cast<capture::Step>(i) == shown);
  view(shown).show(session);
  if ((shown == capture::Step::check || shown == capture::Step::train) && !trainerInfo.checked && !trainerInfo.busy)
    checkTrainer();
  layoutFooter();
  repaint();
}

EngineReply CapturePage::run(const juce::String& command, const juce::var& args, const juce::String& failureTitle) {
  const EngineReply reply = client.run(command, args);
  if (!reply.ok && failureTitle.isNotEmpty())
    notice(failureTitle, reply.errorMessage.isNotEmpty() ? reply.errorMessage : reply.errorCode);
  return reply;
}

void CapturePage::notice(const juce::String& title, const juce::String& message) {
  Dialog::Options options;
  options.title = title;
  options.message = message;
  options.cancelable = false;
  prompt.show(options, [](bool, const juce::String&) {});
}

void CapturePage::confirm(const juce::String& title, const juce::String& message, const juce::String& button,
                          std::function<void()> yes) {
  Dialog::Options options;
  options.title = title;
  options.message = message;
  options.confirm = button;
  juce::Component::SafePointer<CapturePage> self(this);
  prompt.show(options, [self, yes](bool confirmed, const juce::String&) {
    if (self != nullptr && confirmed)
      yes();
  });
}

void CapturePage::later(std::function<void()> work) {
  juce::Component::SafePointer<CapturePage> self(this);
  juce::Timer::callAfterDelay(40, [self, work] {
    if (self != nullptr)
      work();
  });
}

void CapturePage::cancelCapture() {
  if (currentId.isEmpty()) {
    view(shown).show({});
    return;
  }
  const juce::String name = session["name"].toString();
  confirm("Cancel this capture?",
          "The recordings and the trained model of \"" + name +
              "\" are deleted. A capture you already saved to the library stays.",
          "Delete capture", [this] {
            view(shown).leave();
            if (run("capture.delete", object({{"id", currentId}}), "Could not delete the capture").ok)
              reset();
          });
}

// ####################
// THE TRAINER
// ####################
void CapturePage::readTrainer() {
  const EngineReply reply = client.run("capture.checkTrainer", {});
  trainerInfo.checked = true;
  trainerInfo.busy = false;
  if (!reply.ok) {
    trainerInfo.pythonFound = trainerInfo.packageFound = trainerInfo.versionOk = false;
    trainerInfo.message = reply.errorMessage;
    return;
  }
  trainerInfo.pythonFound = static_cast<bool>(reply.value["pythonFound"]);
  trainerInfo.packageFound = static_cast<bool>(reply.value["packageFound"]);
  trainerInfo.versionOk = static_cast<bool>(reply.value["versionOk"]);
  trainerInfo.python = reply.value["python"].toString();
  trainerInfo.version = reply.value["version"].toString();
  trainerInfo.minimumVersion = reply.value["minimumVersion"].toString();
  trainerInfo.message = reply.value["message"].toString();
}

void CapturePage::checkTrainer() {
  if (trainerInfo.busy)
    return;
  trainerInfo.busy = true;
  for (int i = 0; i < capture::kStepCount; ++i)
    view(static_cast<capture::Step>(i)).trainerChanged();
  repaint();
  // The check starts Python, which takes a few seconds: after the screen drew.
  later([this] { checkTrainerNow(); });
}

void CapturePage::checkTrainerNow() {
  readTrainer();
  for (int i = 0; i < capture::kStepCount; ++i)
    view(static_cast<capture::Step>(i)).trainerChanged();
  layoutFooter();
  repaint();
}

// ####################
// FOOTER AND TIMER
// ####################
void CapturePage::pressPrimary() { view(shown).primary(); }
void CapturePage::pressSecondary() { view(shown).secondary(); }
void CapturePage::pressBack() { view(shown).back(); }

void CapturePage::layoutFooter() {
  const CaptureStep::Footer footer = view(shown).footer();
  cancel.setVisible(footer.showCancel);
  backButton.setVisible(footer.showBack);
  backButton.setEnabled(footer.backEnabled);
  secondaryButton.setVisible(footer.secondary.isNotEmpty());
  secondaryButton.setButtonText(footer.secondary);
  secondaryButton.setEnabled(footer.secondaryEnabled);
  primaryButton.setVisible(true);
  primaryButton.setButtonText(footer.primary);
  primaryButton.setEnabled(footer.primaryEnabled);

  const auto bar = footerBounds();
  const int top = bar.getY() + (bar.getHeight() - kButtonHeight) / 2;
  const auto width = [](const PushButton& button, int least) { return juce::jmax(least, button.idealWidth() + 12); };
  cancel.setBounds(bar.getX() + kMarginX - 12, top, width(cancel, 0), kButtonHeight);
  int right = bar.getRight() - kMarginX;
  const int primaryWidth = width(primaryButton, 120);
  primaryButton.setBounds(right - primaryWidth, top, primaryWidth, kButtonHeight);
  right -= primaryWidth + 12;
  if (footer.secondary.isNotEmpty()) {
    const int secondaryWidth = width(secondaryButton, 96);
    secondaryButton.setBounds(right - secondaryWidth, top, secondaryWidth, kButtonHeight);
    right -= secondaryWidth + 12;
  }
  backButton.setBounds(right - width(backButton, 96), top, width(backButton, 96), kButtonHeight);
}

void CapturePage::visibilityChanged() {
  if (isShowing()) {
    startTimerHz(10);
  } else {
    stopTimer();
    // The tone stops when the page is left; a recording and a training go on.
    view(shown).leave();
  }
}

void CapturePage::timerCallback() { view(shown).tick(++ticks); }

// ####################
// GEOMETRY AND PAINT
// ####################
juce::Rectangle<int> CapturePage::stepperBounds() const { return getLocalBounds().removeFromTop(kStepperHeight); }

juce::Rectangle<int> CapturePage::footerBounds() const { return getLocalBounds().removeFromBottom(kFooterHeight); }

juce::Rectangle<int> CapturePage::contentBounds() const {
  return getLocalBounds().withTrimmedTop(kStepperHeight).withTrimmedBottom(kFooterHeight).reduced(kMarginX, kMarginY);
}

void CapturePage::resized() {
  for (int i = 0; i < capture::kStepCount; ++i)
    view(static_cast<capture::Step>(i)).setBounds(contentBounds());
  prompt.setBounds(getLocalBounds());
  layoutFooter();
}

void CapturePage::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);

  // The stepper.
  const auto bar = stepperBounds();
  g.setColour(colour::line);
  g.fillRect(bar.getX(), bar.getBottom() - 1, bar.getWidth(), 1);
  int x = 24;
  const int middle = bar.getCentreY();
  const int current = static_cast<int>(shown) + 1;
  for (int i = 0; i < capture::kStepCount; ++i) {
    const int n = i + 1;
    if (i > 0) {
      g.setColour(colour::line);
      g.fillRect(x, middle - 1, 24, 2);
      x += 24 + 8;
    }
    const bool done = n < current;
    const bool isCurrent = n == current;
    const auto circle = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(middle - 14), 28.0f, 28.0f);
    g.setColour(done ? colour::ok : isCurrent ? colour::iris : juce::Colours::transparentBlack);
    g.fillEllipse(circle);
    g.setColour(done ? colour::ok : isCurrent ? colour::iris : colour::line);
    g.drawEllipse(circle.reduced(0.5f), 1.0f);
    g.setFont(type::mono(13.0f));
    g.setColour(done || isCurrent ? colour::ground : colour::muted);
    g.drawText(juce::String(n), circle.toNearestInt(), juce::Justification::centred, false);
    x += 28 + 8;
    const juce::String title = capture::stepTitle(static_cast<capture::Step>(i));
    const int labelWidth = parts::textWidth(type::button(), title) + 2;
    g.setFont(type::button());
    g.setColour(done || isCurrent ? colour::text : colour::muted);
    g.drawText(title, x, middle - 12, labelWidth, 24, juce::Justification::centredLeft, false);
    x += labelWidth + 8;
  }

  // The footer.
  const auto footer = footerBounds();
  g.setColour(colour::line);
  g.fillRect(footer.getX(), footer.getY(), footer.getWidth(), 1);
}

}  // namespace pedaleira::ui
