#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

namespace pedaleira::ui {

namespace {

constexpr int kVerdictWidth = 440;
constexpr int kColumnGap = 24;
constexpr int kRowPad = 12;
constexpr int kIconSize = 28;
constexpr int kRowGap = 2;

juce::Colour statusColour(const juce::String& status) {
  if (status == "fail")
    return colour::clip;
  if (status == "warn")
    return colour::warn;
  return colour::ok;
}

const juce::Path& statusIcon(const juce::String& status) {
  if (status == "fail")
    return icons::path(icons::Icon::close);
  if (status == "warn")
    return icons::path(icons::Icon::warning);
  return icons::path(icons::Icon::check);
}

}  // namespace

struct CaptureCheckStep::Layout {
  juce::Rectangle<int> results, list, verdict, trainer, paragraph, buttons, overrideRow;
  std::vector<int> rowHeights;
  int contentHeight = 0;
};

namespace {

// What the verdict card says about a report.
struct Verdict {
  juce::String title, pill, text;
  juce::Colour pillColour = colour::ok;
};

Verdict describe(const juce::var& report) {
  Verdict v;
  const juce::String verdict = report["verdict"].toString();
  const int passed = static_cast<int>(report["passed"]);
  const int warnings = static_cast<int>(report["warnings"]);
  juce::StringArray warned, failed;
  juce::String firstFailure;
  if (const auto* list = report["checks"].getArray())
    for (const auto& item : *list) {
      if (item["status"].toString() == "warn")
        warned.add(item["message"].toString());
      if (item["status"].toString() == "fail") {
        failed.add(item["message"].toString());
        if (firstFailure.isEmpty())
          firstFailure = item["name"].toString().toLowerCase() + " " + item["value"].toString();
      }
    }
  if (verdict == "fail") {
    v.title = "Not ready to train";
    v.pill = "Failed: " + firstFailure;
    v.pillColour = colour::clip;
    v.text = (failed.isEmpty() ? juce::String() : failed[0]) + " Record again, or go back to the levels and adjust.";
  } else if (verdict == "warn") {
    v.title = "Good to train";
    v.pill = juce::String(passed) + " passed " + type::dot() + " " + juce::String(warnings) + (warnings == 1 ? " warning" : " warnings");
    v.text = warned.joinIntoString(" ");
  } else {
    v.title = "Good to train";
    v.pill = juce::String(passed) + " passed";
    v.text = "The signal is clean, lined up and well inside the level range.";
  }
  return v;
}

}  // namespace

CaptureCheckStep::CaptureCheckStep(CaptureHost& hostPage) : CaptureStep(hostPage), trainerPanel(hostPage) {
  setTitle("Check");
  again.onClick = [this] { recordAgain(); };
  listen.setEnabled(false);
  listen.setTooltip(parts::kLater);
  trainAnyway.onClick = [this] { host.footerChanged(); };
  addAndMakeVisible(again);
  addAndMakeVisible(listen);
  addChildComponent(trainAnyway);
  addAndMakeVisible(trainerPanel);
}

void CaptureCheckStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty())
    return;
  scroll = 0;
  trainAnyway.setToggleState(static_cast<bool>(session["checksOverridden"]), juce::dontSendNotification);
  trainerPanel.update();
  resized();
  repaint();
}

void CaptureCheckStep::trainerChanged() {
  trainerPanel.update();
  resized();
  host.footerChanged();
}

bool CaptureCheckStep::canTrain() const {
  if (!hasReport() || !host.trainer().usable())
    return false;
  return verdict() != "fail" || trainAnyway.getToggleState();
}

CaptureStep::Footer CaptureCheckStep::footer() const {
  Footer footer;
  footer.backEnabled = !checking;
  if (!hasReport()) {
    footer.primary = checking ? "Checking..." : "Run the checks";
    footer.primaryEnabled = !checking && session["recording"]["state"].toString() == "complete";
    return footer;
  }
  footer.primary = "Start training";
  footer.primaryEnabled = canTrain();
  return footer;
}

void CaptureCheckStep::primary() {
  if (!hasReport()) {
    runChecks();
    return;
  }
  if (!canTrain())
    return;
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("id", id());
  if (verdict() == "fail" && trainAnyway.getToggleState())
    args->setProperty("overrideChecks", true);
  const EngineReply reply = host.run("capture.startTraining", juce::var(args.get()), "Could not start training");
  if (reply.ok)
    host.reload();
}

void CaptureCheckStep::secondary() {}

void CaptureCheckStep::runChecks() {
  checking = true;
  host.footerChanged();
  host.later([this] {
    const EngineReply reply = host.run("capture.runChecks", object({{"id", id()}}), "The checks could not run");
    checking = false;
    if (reply.ok)
      host.sessionChanged(reply.value);
    else
      host.footerChanged();
  });
}

void CaptureCheckStep::recordAgain() {
  host.confirm("Record again?", "The recording and its checks are thrown away and you can record it again.", "Record again", [this] {
    const EngineReply reply = host.run("capture.cancelRecording", object({{"id", id()}}), "Could not discard the recording");
    if (reply.ok)
      host.reload();
  });
}

void CaptureCheckStep::back() {
  host.confirm("Go back to the recording?", "The recording and its checks are thrown away. You can record it again.", "Go back",
               [this] { host.goBack(); });
}

// ####################
// LAYOUT
// ####################
CaptureCheckStep::Layout CaptureCheckStep::layout() const {
  Layout l;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  const int resultsWidth = getWidth() - kVerdictWidth - kColumnGap;
  l.results = {0, top, resultsWidth, getHeight() - top};
  l.list = l.results.withTrimmedTop(28);
  if (const auto* list = session["checks"]["checks"].getArray()) {
    const int textWidth = resultsWidth - 2 * kRowPad - kIconSize - 14;
    for (const auto& item : *list) {
      const int note = parts::wrappedHeight(item["message"].toString(), type::body(), textWidth);
      const int height = 2 * kRowPad + 22 + note;
      l.rowHeights.push_back(height);
      l.contentHeight += height + kRowGap;
    }
  }

  // The verdict card, and the trainer card under it.
  const int x = getWidth() - kVerdictWidth;
  const int inner = kVerdictWidth - 48;
  int y = top + 20 + 16 + 14;
  y += 34 + 14;
  const int paragraphHeight = hasReport() ? parts::wrappedHeight(describe(session["checks"]).text, type::body(), inner) : 0;
  l.paragraph = {x + 24, y, inner, paragraphHeight};
  y += paragraphHeight + 14;
  l.buttons = {x + 24, y, inner, 36};
  y += 36;
  if (hasReport() && verdict() == "fail") {
    y += 14;
    l.overrideRow = {x + 24, y, inner, 24};
    y += 24;
  }
  y += 20;
  l.verdict = {x, top, kVerdictWidth, y - top};
  l.trainer = {x, l.verdict.getBottom() + parts::kBlockGap, kVerdictWidth, trainerPanel.heightFor(kVerdictWidth)};
  return l;
}

void CaptureCheckStep::resized() {
  const Layout l = layout();
  again.setBounds(l.buttons.getX(), l.buttons.getY(), again.idealWidth() + 8, 36);
  listen.setBounds(again.getRight() + 12, l.buttons.getY(), listen.idealWidth() + 8, 36);
  again.setVisible(hasReport());
  listen.setVisible(hasReport());
  trainAnyway.setVisible(hasReport() && verdict() == "fail");
  trainAnyway.setBounds(l.overrideRow);
  trainerPanel.setBounds(l.trainer);
}

void CaptureCheckStep::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  const Layout l = layout();
  if (!l.list.contains(event.getPosition()))
    return;
  const int most = juce::jmax(0, l.contentHeight - l.list.getHeight());
  scroll = juce::jlimit(0, most, scroll - juce::roundToInt(wheel.deltaY * 200.0f));
  repaint();
}

// ####################
// PAINT
// ####################
void CaptureCheckStep::paint(juce::Graphics& g) {
  parts::paintHeading(g, getLocalBounds(), "Checking the recording", "We check the recording before spending time on training.");
  if (id().isEmpty())
    return;
  const Layout l = layout();

  // ---- The results ----
  const auto* list = session["checks"]["checks"].getArray();
  paint::label(g, "Results", {l.results.getX(), l.results.getY(), 200, 16});
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText(list != nullptr ? juce::String(list->size()) + " checks" : juce::String(), l.results.getX(), l.results.getY(), l.results.getWidth(), 16,
             juce::Justification::centredRight, false);
  if (list == nullptr) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText(checking ? "Running the checks..." : "The checks have not run on this recording yet.", l.list.withHeight(40),
               juce::Justification::centredLeft, true);
  } else {
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(l.list);
    int y = l.list.getY() - scroll;
    for (int i = 0; i < list->size(); ++i) {
      const auto& item = list->getReference(i);
      const juce::String status = item["status"].toString();
      const juce::Colour tint = statusColour(status);
      const auto row = juce::Rectangle<int>(l.list.getX(), y, l.list.getWidth(), l.rowHeights[static_cast<size_t>(i)]);
      y += row.getHeight() + kRowGap;
      if (row.getBottom() < l.list.getY() || row.getY() > l.list.getBottom())
        continue;
      if (status != "pass") {
        g.setColour(tint.withAlpha(0.08f));
        g.fillRoundedRectangle(row.toFloat(), 8.0f);
      }
      auto inner = row.reduced(kRowPad);
      const auto icon = inner.removeFromLeft(kIconSize).removeFromTop(kIconSize).toFloat();
      g.setColour(tint.withAlpha(0.16f));
      g.fillEllipse(icon);
      icons::draw(g, statusIcon(status), icon.reduced(5.0f), tint, 2.0f);
      inner.removeFromLeft(14);
      auto head = inner.removeFromTop(22);
      g.setFont(type::sans(15.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(item["name"].toString(), head.removeFromLeft(head.getWidth() / 2), juce::Justification::centredLeft, true);
      g.setFont(type::mono(13.0f));
      g.drawText(item["value"].toString(), head, juce::Justification::centredRight, true);
      parts::drawWrapped(g, item["message"].toString(), type::body(), colour::muted, inner);
    }
  }

  // ---- The verdict ----
  if (hasReport()) {
    const Verdict v = describe(session["checks"]);
    parts::paintCard(g, l.verdict);
    paint::label(g, "Verdict", {l.verdict.getX() + 24, l.verdict.getY() + 20, 200, 16});
    const int titleY = l.verdict.getY() + 20 + 16 + 14;
    g.setFont(type::title());
    g.setColour(colour::text);
    const int titleWidth = parts::textWidth(type::title(), v.title);
    g.drawText(v.title, l.verdict.getX() + 24, titleY, titleWidth + 4, 34, juce::Justification::centredLeft, false);
    int pillX = l.verdict.getX() + 24 + titleWidth + 12;
    int pillWidth = parts::pillWidth(v.pill, 24);
    if (pillX + pillWidth > l.verdict.getRight() - 24) {
      pillX = l.verdict.getX() + 24;
      pillWidth = juce::jmin(pillWidth, l.verdict.getWidth() - 48);
    }
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(v.pillColour);
    const auto pill = juce::Rectangle<int>(pillX, titleY + 5, pillWidth, 24);
    g.fillRoundedRectangle(pill.toFloat(), 12.0f);
    g.setColour(colour::ground);
    g.drawText(v.pill, pill, juce::Justification::centred, true);
    parts::drawWrapped(g, v.text, type::body(), colour::muted, l.paragraph);
    if (listen.isVisible()) {
      const auto later = juce::Rectangle<int>(listen.getRight() + 10, listen.getY() + 7, paint::chipWidth(parts::kLater), 22);
      if (later.getRight() <= l.verdict.getRight() - 12)
        parts::paintLater(g, later);
    }
  }
}

}  // namespace pedaleira::ui
