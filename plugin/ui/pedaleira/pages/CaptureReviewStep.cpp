#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

namespace pedaleira::ui {

namespace {
constexpr int kSaveWidth = 480;
constexpr int kColumnGap = 24;
constexpr int kAbHeight = 196;
constexpr const char* const kGainIds[] = {"clean", "crunch", "highGain"};

int gainIndex(const juce::String& id) {
  for (int i = 0; i < 3; ++i)
    if (id == kGainIds[i])
      return i;
  return 1;
}

juce::Colour bandColour(const juce::String& band) { return band == "great" || band == "notBad" ? colour::ok : colour::warn; }
}  // namespace

// One of the two big buttons of the A/B group: a name and a line under it.
class CaptureReviewStep::AbButton : public juce::Button {
public:
  AbButton(const juce::String& title, const juce::String& line) : juce::Button(title), name(title), sub(line) {
    setTitle(title);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void setSub(const juce::String& line) {
    sub = line;
    repaint();
  }
  void setOn(bool on) {
    chosen = on;
    repaint();
  }

protected:
  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    const auto area = getLocalBounds().toFloat();
    const float alpha = isEnabled() ? 1.0f : 0.45f;
    if (chosen) {
      g.setColour(colour::drop);
      g.fillRoundedRectangle(area.translated(0.0f, 2.0f).withTrimmedBottom(2.0f), 9.0f);
      g.setColour(colour::iris.withMultipliedAlpha(alpha));
      g.fillRoundedRectangle(area.withTrimmedBottom(2.0f), 9.0f);
    } else if (highlighted && isEnabled()) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(area, 9.0f);
    }
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::irisHover);
      g.drawRoundedRectangle(area.reduced(1.0f), 9.0f, 1.5f);
    }
    auto inner = getLocalBounds().reduced(14, 8);
    g.setFont(type::sans(20.0f, type::Weight::bold));
    g.setColour((chosen ? colour::ground : colour::text).withMultipliedAlpha(alpha));
    g.drawText(name, inner.removeFromTop(26), juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f, type::Weight::medium));
    g.setColour((chosen ? colour::ground : colour::muted).withMultipliedAlpha(alpha));
    g.drawText(sub, inner.removeFromTop(16), juce::Justification::centredLeft, true);
  }

private:
  juce::String name, sub;
  bool chosen = false;
};

struct CaptureReviewStep::Layout {
  juce::Rectangle<int> ab, accuracy, legend, difference, save;
  juce::Rectangle<int> group;
  juce::Rectangle<int> nameLabel, nameField, typeLabel, typeField, gainLabel, gainField, notesLabel, notesField;
  juce::Rectangle<int> calibrationLabel, calibration, calibrationNote, footerRow;
};

CaptureReviewStep::CaptureReviewStep(CaptureHost& hostPage) : CaptureStep(hostPage) {
  setTitle("Review");
  parts::styleEditor(nameEdit);
  nameEdit.setTitle("Capture name");
  nameEdit.onReturnKey = [this] { applyMetadata(); };
  nameEdit.onFocusLost = [this] { applyMetadata(); };
  addAndMakeVisible(nameEdit);
  parts::styleEditor(notesEdit, true);
  notesEdit.setTitle("Notes");
  notesEdit.onFocusLost = [this] { applyMetadata(); };
  addAndMakeVisible(notesEdit);
  gainControl.setTitle("Gain tag");
  gainControl.onChange = [this](int) {
    if (!updating)
      applyMetadata();
  };
  addAndMakeVisible(gainControl);
  addAndMakeVisible(addOnSave);
  addOnSave.setToggleState(true, juce::dontSendNotification);

  realButton = std::make_unique<AbButton>("A " + type::dot() + " Real amp", "Comes in a later build");
  realButton->setEnabled(false);
  captureButton = std::make_unique<AbButton>("B " + type::dot() + " Capture", "");
  captureButton->onClick = [this] { addToRig(); };
  addAndMakeVisible(*realButton);
  addAndMakeVisible(*captureButton);
  blindSwitch.setEnabled(false);
  addAndMakeVisible(blindSwitch);
}

CaptureReviewStep::~CaptureReviewStep() = default;

void CaptureReviewStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty())
    return;
  review = host.run("capture.review", object({{"id", id()}}), {}).value;
  savedFile = session["files"]["saved"].toString();
  updating = true;
  if (nameEdit.getText() != session["name"].toString())
    nameEdit.setText(session["name"].toString(), false);
  if (notesEdit.getText() != session["notes"].toString())
    notesEdit.setText(session["notes"].toString(), false);
  gainControl.setSelected(gainIndex(session["gainTag"].toString()));
  rebuildTypes();
  updating = false;
  const bool done = !savedFile.isEmpty();
  nameEdit.setEnabled(!done);
  notesEdit.setEnabled(!done);
  gainControl.setEnabled(!done);
  addOnSave.setVisible(!done && !addedToRig);
  captureButton->setSub(addedToRig ? "In the rig, ready to play" : "Add it to the rig to play it");
  captureButton->setOn(addedToRig);
  resized();
  repaint();
}

void CaptureReviewStep::rebuildTypes() {
  const juce::String gearType = session["gear"]["type"].toString();
  const juce::StringArray choices = capture::gearTypeChoices(gearType);
  typeControl = std::make_unique<SegmentedControl>(choices);
  typeControl->setTitle("Type");
  typeControl->setSelected(juce::jmax(0, choices.indexOf(capture::gearTypeName(gearType))));
  typeControl->setEnabled(savedFile.isEmpty());
  typeControl->onChange = [this, choices](int index) {
    if (updating)
      return;
    const EngineReply reply = host.run("capture.setSource", object({{"id", id()}, {"gearType", capture::gearTypeId(choices[index])}}),
                                       "Could not change the type");
    if (reply.ok)
      session = reply.value;
  };
  addAndMakeVisible(*typeControl);
}

// ####################
// ACTIONS
// ####################
CaptureStep::Footer CaptureReviewStep::footer() const {
  Footer footer;
  footer.showBack = false;
  if (!savedFile.isEmpty()) {
    footer.showCancel = false;
    footer.primary = "Start another capture";
    return footer;
  }
  footer.secondary = "Train again with more epochs";
  footer.primary = "Save capture";
  footer.primaryEnabled = nameEdit.getText().trim().isNotEmpty();
  return footer;
}

void CaptureReviewStep::primary() {
  if (!savedFile.isEmpty()) {
    // The working folder of a saved capture is not needed any more.
    host.run("capture.delete", object({{"id", id()}}), {});
    savedFile.clear();
    addedToRig = false;
    rigNote.clear();
    host.sessionChanged({});
    return;
  }
  save();
}

void CaptureReviewStep::secondary() { trainAgain(); }

void CaptureReviewStep::applyMetadata() {
  if (updating || id().isEmpty() || !savedFile.isEmpty())
    return;
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("id", id());
  bool changed = false;
  const juce::String name = nameEdit.getText().trim();
  if (name.isNotEmpty() && name != session["name"].toString()) {
    args->setProperty("name", name);
    changed = true;
  }
  if (notesEdit.getText() != session["notes"].toString()) {
    args->setProperty("notes", notesEdit.getText());
    changed = true;
  }
  const juce::String gain = kGainIds[juce::jlimit(0, 2, gainControl.getSelected())];
  if (gain != session["gainTag"].toString()) {
    args->setProperty("gainTag", gain);
    changed = true;
  }
  if (!changed) {
    host.footerChanged();
    return;
  }
  const EngineReply reply = host.run("capture.setSource", juce::var(args.get()), "Could not change the capture");
  if (reply.ok) {
    session = reply.value;
    review = host.run("capture.review", object({{"id", id()}}), {}).value;
  }
  host.footerChanged();
  repaint();
}

void CaptureReviewStep::addToRig() {
  if (id().isEmpty() || addedToRig)
    return;
  const EngineReply reply = host.run("capture.addToRig", object({{"id", id()}}), "Could not add the capture to the rig");
  if (!reply.ok)
    return;
  addedToRig = true;
  rigNote = "Added to the rig as a NAM block.";
  captureButton->setSub("In the rig, ready to play");
  captureButton->setOn(true);
  addOnSave.setVisible(false);
  host.rigChanged();
  repaint();
}

void CaptureReviewStep::save() {
  applyMetadata();
  const bool add = addOnSave.getToggleState() && !addedToRig;
  const EngineReply reply = host.run("capture.save", object({{"id", id()}}), "Could not save the capture");
  if (!reply.ok)
    return;
  savedFile = reply.value["file"].toString();
  if (add) {
    const EngineReply added = host.run("capture.addToRig", object({{"id", id()}}), "The capture is saved, but could not be added to the rig");
    if (added.ok) {
      addedToRig = true;
      rigNote = "Added to the rig as a NAM block.";
    }
  }
  host.rigChanged();
  host.reload();
}

void CaptureReviewStep::trainAgain() {
  const int epochs = juce::jlimit(1, 2000, static_cast<int>(session["training"]["epochs"]) * 2);
  host.confirm("Train again?", "The trainer starts over with " + juce::String(epochs) + " epochs. This model is thrown away when it begins.",
               "Train again", [this, epochs] {
                 const EngineReply reply = host.run("capture.startTraining", object({{"id", id()}, {"epochs", epochs}}), "Could not start training");
                 if (reply.ok) {
                   addedToRig = false;
                   host.reload();
                 }
               });
}

// ####################
// LAYOUT
// ####################
CaptureReviewStep::Layout CaptureReviewStep::layout() const {
  Layout l;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  const int leftWidth = getWidth() - kSaveWidth - kColumnGap;
  l.ab = {0, top, leftWidth, kAbHeight};
  l.accuracy = {0, l.ab.getBottom() + parts::kBlockGap, leftWidth, 48};
  l.legend = {0, l.accuracy.getBottom() + 10, leftWidth, 16};
  l.difference = {0, l.legend.getBottom() + parts::kBlockGap, leftWidth, getHeight() - l.legend.getBottom() - parts::kBlockGap};
  l.group = {24, top + 20 + 16 + 14, 2 * 200 + 4 + 8, 62 + 8};

  l.save = {getWidth() - kSaveWidth, top, kSaveWidth, getHeight() - top};
  const int x = l.save.getX() + 24;
  const int w = l.save.getWidth() - 48;
  int y = l.save.getY() + 20 + 16 + 12;
  l.nameLabel = {x, y, w, 16};
  l.nameField = {x, y + 22, w, 36};
  y += 58 + 12;
  l.typeLabel = {x, y, w, 16};
  l.typeField = {x, y + 22, w, 38};
  y += 60 + 12;
  l.gainLabel = {x, y, w, 16};
  l.gainField = {x, y + 22, w, 38};
  y += 60 + 12;
  l.notesLabel = {x, y, w, 16};
  l.notesField = {x, y + 22, w, 52};
  y += 74 + 12;
  l.calibrationLabel = {x, y, w, 16};
  l.calibration = {x, y + 22, w, 86};
  l.calibrationNote = {x, l.calibration.getBottom() + 4, w, 16};
  l.footerRow = {x, l.calibrationNote.getBottom() + 12, w, 24};
  return l;
}

void CaptureReviewStep::resized() {
  const Layout l = layout();
  realButton->setBounds(l.group.getX() + 4, l.group.getY() + 4, 200, 62);
  captureButton->setBounds(l.group.getX() + 4 + 200 + 4, l.group.getY() + 4, 200, 62);
  blindSwitch.setBounds(l.group.getRight() + 24, l.group.getY() + 4, 40, 22);
  nameEdit.setBounds(l.nameField);
  if (typeControl != nullptr)
    typeControl->setBounds(l.typeField.withWidth(juce::jmin(l.typeField.getWidth(), typeControl->idealWidth() + 40)));
  gainControl.setBounds(l.gainField.withWidth(juce::jmin(l.gainField.getWidth(), gainControl.idealWidth() + 40)));
  notesEdit.setBounds(l.notesField);
  addOnSave.setBounds(l.footerRow);
}

// ####################
// PAINT
// ####################
void CaptureReviewStep::paint(juce::Graphics& g) {
  parts::paintHeading(g, getLocalBounds(), "Review and save", "Play it and listen. The capture should feel like the device.");
  if (id().isEmpty())
    return;
  const Layout l = layout();
  const bool done = !savedFile.isEmpty();

  // ---- A/B ----
  parts::paintCard(g, l.ab);
  paint::label(g, "A/B", {l.ab.getX() + 24, l.ab.getY() + 20, 100, 16});
  g.setColour(colour::ground);
  g.fillRoundedRectangle(l.group.toFloat(), 12.0f);
  g.setColour(colour::line);
  g.drawRoundedRectangle(l.group.toFloat().reduced(0.5f), 12.0f, 1.0f);
  parts::paintLater(g, {l.ab.getRight() - 24 - paint::chipWidth("A/B against the device: " + juce::String(parts::kLater)), l.ab.getY() + 18,
                        paint::chipWidth("A/B against the device: " + juce::String(parts::kLater)), 22});
  {
    // The blind test, the level match and the input meter need the real device in the loop.
    const int x = l.group.getRight() + 24;
    const int y = l.group.getY() + 4;
    g.setFont(type::body());
    g.setColour(colour::muted.withMultipliedAlpha(0.45f));
    g.drawText("Blind test", x + 50, y - 2, 200, 22, juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.drawText("Hides which side is playing", x + 50, y + 18, 220, 16, juce::Justification::centredLeft, true);
    g.drawText("Levels matched to the device", x, y + 48, 260, 18, juce::Justification::centredLeft, true);
  }
  {
    const juce::String text = addedToRig ? rigNote : "B is the capture running in the rig you are playing with.";
    g.setFont(type::compact());
    g.setColour(addedToRig ? colour::ok : colour::muted);
    g.drawText(text, l.ab.getX() + 24, l.group.getBottom() + 10, l.ab.getWidth() - 48, 18, juce::Justification::centredLeft, true);
  }

  // ---- Accuracy ----
  g.setColour(colour::ground);
  g.fillRoundedRectangle(l.accuracy.toFloat(), 8.0f);
  g.setColour(colour::line);
  g.drawRoundedRectangle(l.accuracy.toFloat().reduced(0.5f), 8.0f, 1.0f);
  {
    auto row = l.accuracy.reduced(16, 0);
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("Accuracy", row.removeFromLeft(80), juce::Justification::centredLeft, false);
    if (review["esr"].isDouble()) {
      g.setFont(type::mono(16.0f));
      g.setColour(colour::text);
      const juce::String esr = "ESR " + capture::esrText(static_cast<double>(review["esr"]));
      const int width = parts::textWidth(type::mono(16.0f), esr) + 12;
      g.drawText(esr, row.removeFromLeft(width), juce::Justification::centredLeft, false);
      const juce::String label = review["bandLabel"].toString();
      parts::paintPill(g, label, row.removeFromLeft(parts::pillWidth(label, 24)).withSizeKeepingCentre(parts::pillWidth(label, 24), 24),
                       bandColour(review["band"].toString()));
    } else {
      g.setFont(type::body());
      g.setColour(colour::faint);
      g.drawText("The trainer did not report an ESR.", row, juce::Justification::centredLeft, false);
    }
  }
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText("ESR: under 0.01 great " + type::dot() + " under 0.035 not bad " + type::dot() + " under 0.1 might be okay " + type::dot() +
                 " above that, recapture",
             l.legend, juce::Justification::centredLeft, true);

  // ---- The difference plot needs the real device's take ----
  parts::paintCard(g, l.difference);
  paint::label(g, "Difference " + type::dot() + " real amp and capture overlaid", {l.difference.getX() + 24, l.difference.getY() + 20, l.difference.getWidth() - 48, 16});
  {
    const auto area = l.difference.reduced(24).withTrimmedTop(32);
    paint::dashedOutline(g, area.toFloat(), 8.0f, colour::line);
    const int width = paint::chipWidth(parts::kLater);
    parts::paintLater(g, {area.getCentreX() - width / 2, area.getCentreY() - 11, width, 22});
  }

  // ---- Save ----
  parts::paintCard(g, l.save);
  paint::label(g, "Save capture", {l.save.getX() + 24, l.save.getY() + 20, 200, 16});
  paint::label(g, "Name", l.nameLabel);
  paint::label(g, "Type", l.typeLabel);
  paint::label(g, "Gain tag", l.gainLabel);
  paint::label(g, "Notes", l.notesLabel);
  paint::label(g, "Calibration", l.calibrationLabel);
  {
    g.setColour(colour::ground);
    g.fillRoundedRectangle(l.calibration.toFloat(), 8.0f);
    g.setColour(colour::line);
    g.drawRoundedRectangle(l.calibration.toFloat().reduced(0.5f), 8.0f, 1.0f);
    const auto dbu = [](const juce::var& value) {
      const double v = static_cast<double>(value);
      return juce::String(v >= 0.0 ? "+" : "") + juce::String(v, 1) + " dBu";
    };
    const juce::var calibration = session["calibration"];
    const juce::String latency = review["latencySamples"].isInt() || review["latencySamples"].isDouble()
                                     ? juce::String(static_cast<int>(review["latencySamples"])) + " samples"
                                     : juce::String("--");
    const std::pair<juce::String, juce::String> rows[] = {
        {"Input level", calibration["sendDbu"].isVoid() ? juce::String("Not set") : dbu(calibration["sendDbu"])},
        {"Output level", dbu(calibration["returnDbu"])},
        {"Latency", latency},
        {"Sample rate", "48 kHz"}};
    auto area = l.calibration.reduced(12, 8);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    for (const auto& row : rows) {
      auto line = area.removeFromTop(16);
      g.setColour(colour::muted);
      g.drawText(row.first, line, juce::Justification::centredLeft, false);
      g.setColour(colour::text);
      g.drawText(row.second, line, juce::Justification::centredRight, false);
      area.removeFromTop(2);
    }
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("Saved in the .nam file so levels are right when you load it.", l.calibrationNote, juce::Justification::centredLeft, true);
  }
  if (done) {
    icons::draw(g, icons::path(icons::Icon::check), juce::Rectangle<float>(static_cast<float>(l.footerRow.getX()), static_cast<float>(l.footerRow.getY() + 3), 18.0f, 18.0f), colour::ok, 2.25f);
    g.setFont(type::compact());
    g.setColour(colour::ok);
    g.drawText("Saved: " + juce::File(savedFile).getFileName() + (addedToRig ? ", and in the rig" : juce::String()),
               l.footerRow.withTrimmedLeft(28), juce::Justification::centredLeft, true);
  }
}

}  // namespace pedaleira::ui
