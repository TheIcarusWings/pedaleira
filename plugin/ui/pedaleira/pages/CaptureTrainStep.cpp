#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

#include <algorithm>
#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kRightWidth = 380;
constexpr int kColumnGap = 24;

juce::Colour bandColour(const juce::String& band) { return band == "great" || band == "notBad" ? colour::ok : colour::warn; }

juce::String minutesLeft(double seconds) {
  if (seconds < 60.0)
    return "under a minute left";
  return "about " + juce::String(juce::roundToInt(seconds / 60.0)) + " min left";
}

}  // namespace

struct CaptureTrainStep::Layout {
  juce::Rectangle<int> left, trainer, input, message, settings, keep, output;
  bool showTrainer = false, showInput = false;
};

CaptureTrainStep::CaptureTrainStep(CaptureHost& hostPage) : CaptureStep(hostPage), trainerPanel(hostPage), inputFile(hostPage) {
  setTitle("Train");
  minimise.setEnabled(false);
  minimise.setTooltip(parts::kLater);
  addAndMakeVisible(minimise);
  addChildComponent(trainerPanel);
  addChildComponent(inputFile);
  inputFile.onChanged = [this] {
    resized();
    repaint();
    host.footerChanged();
  };
}

CaptureTrainStep::~CaptureTrainStep() = default;

// ####################
// STATE
// ####################
void CaptureTrainStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty())
    return;
  if (historyFor != id()) {
    history.clear();
    historyFor = id();
  }
  const juce::var training = session["training"];
  state = training["state"].toString().isEmpty() ? "none" : training["state"].toString();
  epochs = juce::jmax(1, static_cast<int>(training["epochs"]));
  epoch = static_cast<int>(training["epoch"]);
  error = training["error"].toString();
  hasEsr = training["esr"].isDouble();
  esr = hasEsr ? static_cast<double>(training["esr"]) : 0.0;
  if (session["method"].toString() != "dryWet") {
    const EngineReply settings = host.run("capture.getSettings", {}, {});
    if (settings.ok)
      inputFile.setSettings(settings.value);
  }
  trainerPanel.update();
  if (running() || state == "failed" || state == "cancelled")
    poll();
  resized();
  repaint();
}

void CaptureTrainStep::trainerChanged() {
  trainerPanel.update();
  resized();
  host.footerChanged();
}

void CaptureTrainStep::poll() {
  if (id().isEmpty())
    return;
  const EngineReply reply = host.run("capture.trainingStatus", object({{"id", id()}}), {});
  if (reply.ok)
    apply(reply.value);
}

void CaptureTrainStep::apply(const juce::var& status) {
  const bool was = running();
  state = status["state"].toString().isEmpty() ? juce::String("none") : status["state"].toString();
  epoch = static_cast<int>(status["epoch"]);
  if (static_cast<int>(status["epochs"]) > 0)
    epochs = static_cast<int>(status["epochs"]);
  progress = static_cast<double>(status["progress"]);
  secondsLeft = status["secondsLeft"].isVoid() ? -1.0 : static_cast<double>(status["secondsLeft"]);
  elapsed = static_cast<double>(status["elapsedSeconds"]);
  hasEsr = status["esr"].isDouble();
  esr = hasEsr ? static_cast<double>(status["esr"]) : 0.0;
  esrBand = status["esrBand"].toString();
  esrBandLabel = status["esrBandLabel"].toString();
  error = status["error"].toString();
  message = status["message"].toString();
  log.clear();
  if (const auto* lines = status["log"].getArray())
    for (const auto& line : *lines)
      log.add(line.toString());
  if (hasEsr && epoch > 0 && (history.empty() || history.back().first != epoch))
    history.emplace_back(epoch, esr);
  repaint();
  if (was && !running()) {
    // Done moves the capture to the review; a failure or a stop leaves it here.
    host.reload();
  }
}

void CaptureTrainStep::tick(int count) {
  if (running() && count % 5 == 0)
    poll();
}

// ####################
// ACTIONS
// ####################
CaptureStep::Footer CaptureTrainStep::footer() const {
  Footer footer;
  if (running()) {
    footer.showBack = false;
    footer.secondary = "Stop training";
    footer.primary = "Review";
    footer.primaryEnabled = false;
  } else {
    footer.primary = "Start training";
    footer.primaryEnabled = host.trainer().usable();
  }
  return footer;
}

void CaptureTrainStep::primary() {
  if (!running())
    start();
}

void CaptureTrainStep::start() {
  if (!host.trainer().usable()) {
    host.notice("The trainer is not ready", "Install it as the Trainer card says, then check again.");
    return;
  }
  history.clear();
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("id", id());
  if (static_cast<bool>(session["checksOverridden"]))
    args->setProperty("overrideChecks", true);
  const EngineReply reply = host.run("capture.startTraining", juce::var(args.get()), "Could not start training");
  if (reply.ok)
    apply(reply.value);
  host.footerChanged();
  resized();
}

void CaptureTrainStep::secondary() {
  host.confirm("Stop training?", "The trainer is stopped and the work so far is thrown away.", "Stop training", [this] {
    const EngineReply reply = host.run("capture.cancelTraining", object({{"id", id()}}), "Could not stop training");
    if (reply.ok)
      apply(reply.value);
    host.footerChanged();
    resized();
  });
}

// ####################
// LAYOUT
// ####################
CaptureTrainStep::Layout CaptureTrainStep::layout() const {
  Layout l;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  const int leftWidth = getWidth() - kRightWidth - kColumnGap;
  l.left = {0, top, leftWidth, getHeight() - top};
  l.showTrainer = !running() && state != "done";
  l.showInput = l.showTrainer && !inputFile.exists() && session["method"].toString() != "dryWet";
  int y = top;
  if (l.showTrainer) {
    l.trainer = {0, y, leftWidth, trainerPanel.heightFor(leftWidth)};
    y = l.trainer.getBottom() + parts::kBlockGap;
  }
  if (l.showInput) {
    l.input = {0, y, leftWidth, inputFile.heightFor(leftWidth)};
    y = l.input.getBottom() + parts::kBlockGap;
  }
  if (l.showTrainer && (state == "failed" || state == "cancelled"))
    l.message = {0, y, leftWidth, getHeight() - y};

  const int rightX = getWidth() - kRightWidth;
  l.settings = {rightX, top, kRightWidth, 20 + 16 + 10 + 4 * 38 + 10};
  l.keep = {rightX, l.settings.getBottom() + parts::kBlockGap, kRightWidth, 196};
  l.output = {rightX, l.keep.getBottom() + parts::kBlockGap, kRightWidth, getHeight() - l.keep.getBottom() - parts::kBlockGap};
  return l;
}

void CaptureTrainStep::resized() {
  const Layout l = layout();
  trainerPanel.setVisible(l.showTrainer);
  inputFile.setVisible(l.showInput);
  if (l.showTrainer)
    trainerPanel.setBounds(l.trainer);
  if (l.showInput)
    inputFile.setBounds(l.input);
  minimise.setBounds(l.keep.getX() + 24, l.keep.getBottom() - 20 - 36, minimise.idealWidth() + 8, 36);
}

// ####################
// PAINT
// ####################
void CaptureTrainStep::paint(juce::Graphics& g) {
  parts::paintHeading(g, getLocalBounds(), "Training", "Runs in the background on this computer. You can keep playing.");
  if (id().isEmpty())
    return;
  const Layout l = layout();

  if (!l.showTrainer) {
    // ---- Progress and the ESR curve ----
    parts::paintCard(g, l.left);
    auto inner = l.left.reduced(24);
    {
      auto row = inner.removeFromTop(34);
      const juce::String text = state == "checking" ? juce::String("Checking the data...")
                                : state == "done"   ? "Done " + type::dot() + " " + juce::String(epochs) + " epochs"
                                                    : "Epoch " + juce::String(epoch) + " of " + juce::String(epochs) +
                                                          (secondsLeft >= 0.0 ? " " + type::dot() + " " + minutesLeft(secondsLeft) : juce::String());
      g.setFont(type::sans(18.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(text, row.removeFromLeft(row.getWidth() - 100), juce::Justification::centredLeft, true);
      g.setFont(type::mono(28.0f));
      g.drawText(juce::String(juce::roundToInt(progress * 100.0)) + "%", row, juce::Justification::centredRight, false);
    }
    inner.removeFromTop(10);
    {
      const auto bar = inner.removeFromTop(10);
      g.setColour(colour::raised);
      g.fillRoundedRectangle(bar.toFloat(), 5.0f);
      g.setColour(colour::iris);
      g.fillRoundedRectangle(bar.toFloat().withWidth(static_cast<float>(juce::jlimit(0.0, 1.0, progress)) * bar.getWidth()), 5.0f);
    }
    inner.removeFromTop(24);
    {
      auto head = inner.removeFromTop(16);
      const auto legend = head.removeFromRight(100);
      paint::label(g, "Error so far " + type::dot() + " ESR, lower is better", head);
      g.setColour(colour::iris);
      g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({static_cast<float>(legend.getX() + 4), static_cast<float>(legend.getCentreY())}));
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("ESR by epoch", legend.withTrimmedLeft(14), juce::Justification::centredLeft, false);
    }
    inner.removeFromTop(10);
    const auto footerRow = inner.removeFromBottom(28);
    inner.removeFromBottom(12);
    // The chart: x is the epoch, y the ESR; what is still to come is shaded.
    {
      const auto plot = juce::Rectangle<float>(static_cast<float>(inner.getX() + 56), static_cast<float>(inner.getY() + 12),
                                               static_cast<float>(inner.getWidth() - 56 - 8), static_cast<float>(inner.getHeight() - 12 - 30));
      double top = 0.12;
      for (const auto& point : history)
        top = std::max(top, std::ceil(point.second / 0.03) * 0.03);
      const auto px = [&](double e) { return plot.getX() + static_cast<float>(e / epochs) * plot.getWidth(); };
      const auto py = [&](double v) { return plot.getBottom() - static_cast<float>(v / top) * plot.getHeight(); };
      if (!history.empty()) {
        g.setColour(colour::raised.withAlpha(0.45f));
        g.fillRect(juce::Rectangle<float>(px(history.back().first), plot.getY(), plot.getRight() - px(history.back().first), plot.getHeight()));
      }
      g.setColour(colour::line);
      for (int i = 0; i <= 4; ++i) {
        const float y = plot.getY() + plot.getHeight() * static_cast<float>(i) / 4.0f;
        g.fillRect(plot.getX(), y - (i == 4 ? 0.0f : 0.0f), plot.getWidth(), 1.0f);
        g.fillRect(plot.getX() + plot.getWidth() * static_cast<float>(i) / 4.0f, plot.getY(), 1.0f, plot.getHeight());
      }
      g.setColour(colour::faint);
      g.fillRect(plot.getX(), plot.getBottom(), plot.getWidth(), 1.0f);
      g.setFont(type::mono(11.0f, type::Weight::regular));
      g.setColour(colour::muted);
      for (int i = 0; i <= 4; ++i) {
        const float y = plot.getY() + plot.getHeight() * static_cast<float>(i) / 4.0f;
        g.drawText(juce::String(top * (4 - i) / 4.0, 2), juce::Rectangle<int>(inner.getX(), juce::roundToInt(y) - 7, 48, 14),
                   juce::Justification::centredRight, false);
        g.drawText(juce::String(juce::roundToInt(epochs * static_cast<double>(i) / 4.0)),
                   juce::Rectangle<int>(juce::roundToInt(plot.getX() + plot.getWidth() * static_cast<float>(i) / 4.0f) - 25, juce::roundToInt(plot.getBottom()) + 6, 50, 14),
                   juce::Justification::centred, false);
      }
      if (history.empty()) {
        g.setFont(type::body());
        g.setColour(colour::muted);
        g.drawText(state == "checking" ? "The trainer is checking the data." : "Waiting for the first epoch.", plot.toNearestInt(),
                   juce::Justification::centred, false);
      } else {
        juce::Path curve;
        for (size_t i = 0; i < history.size(); ++i) {
          const juce::Point<float> p(px(history[i].first), py(history[i].second));
          if (i == 0)
            curve.startNewSubPath(p);
          else
            curve.lineTo(p);
        }
        g.setColour(colour::iris);
        g.strokePath(curve, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const juce::Point<float> last(px(history.back().first), py(history.back().second));
        g.setColour(colour::iris.withAlpha(0.7f));
        g.drawLine(last.x, plot.getY(), last.x, plot.getBottom(), 1.0f);
        g.setColour(colour::ground);
        g.fillEllipse(juce::Rectangle<float>(14.0f, 14.0f).withCentre(last));
        g.setColour(colour::iris);
        g.fillEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre(last));
        const juce::String label = "ESR " + capture::esrText(history.back().second);
        const float boxWidth = static_cast<float>(parts::textWidth(type::mono(13.0f), label) + 20);
        const bool flip = last.x + 22.0f + boxWidth > plot.getRight();
        const auto box = juce::Rectangle<float>(flip ? last.x - 22.0f - boxWidth : last.x + 22.0f, juce::jmax(plot.getY(), last.y - 62.0f), boxWidth, 26.0f);
        g.setColour(colour::muted);
        g.drawLine(last.x, last.y, flip ? box.getRight() : box.getX(), box.getCentreY(), 1.0f);
        g.setColour(colour::control);
        g.fillRoundedRectangle(box, 6.0f);
        g.setColour(colour::line);
        g.drawRoundedRectangle(box.reduced(0.5f), 6.0f, 1.0f);
        g.setFont(type::mono(13.0f));
        g.setColour(colour::text);
        g.drawText(label, box.toNearestInt(), juce::Justification::centred, false);
      }
    }
    {
      auto row = footerRow;
      if (hasEsr && esrBandLabel.isNotEmpty()) {
        const juce::String text = "Accuracy so far: " + esrBandLabel;
        const int width = parts::pillWidth(text, 28);
        parts::paintPill(g, text, row.removeFromLeft(width), bandColour(esrBand));
        row.removeFromLeft(12);
        g.setFont(type::body());
        g.setColour(colour::muted);
        g.drawText("Estimated from the latest epoch. It keeps improving until the curve flattens.", row, juce::Justification::centredLeft, true);
      }
    }
  } else if (l.message.getHeight() > 60) {
    // The trainer stopped before the end.
    parts::paintCard(g, l.message);
    auto inner = l.message.reduced(24);
    paint::label(g, state == "cancelled" ? "Training was stopped" : "Training failed", inner.removeFromTop(16));
    inner.removeFromTop(10);
    const juce::String text = error.isNotEmpty() ? error : state == "cancelled" ? juce::String("You stopped the trainer. Start it again when you are ready.") : message;
    if (text.isNotEmpty()) {
      const int height = juce::jmin(inner.getHeight(), parts::wrappedHeight(text, type::body(), inner.getWidth()));
      parts::drawWrapped(g, text, type::body(), state == "failed" ? colour::warn : colour::muted, inner.removeFromTop(height));
      inner.removeFromTop(10);
    }
    g.setFont(type::mono(11.0f, type::Weight::regular));
    g.setColour(colour::muted);
    for (const auto& line : log) {
      if (inner.getHeight() < 16)
        break;
      g.drawText(line, inner.removeFromTop(16), juce::Justification::centredLeft, true);
    }
  }

  // ---- Settings ----
  parts::paintCard(g, l.settings);
  paint::label(g, "Settings", {l.settings.getX() + 24, l.settings.getY() + 20, 200, 16});
  {
    juce::String python = host.trainer().python;
    if (python.length() > 26)
      python = "..." + python.substring(python.length() - 24);
    const struct {
      const char* label;
      juce::String value;
      bool dot;
    } rows[] = {{"Model", "A2 (Full + Lite)", false},
                {"Epochs", juce::String(epochs), false},
                {"Python", python.isEmpty() ? juce::String("python3") : python, false},
                {"Helper process", running() ? "running" : state == "done" ? "finished" : "idle", true}};
    auto area = juce::Rectangle<int>(l.settings.getX() + 24, l.settings.getY() + 20 + 16 + 10, l.settings.getWidth() - 48, 28);
    for (const auto& row : rows) {
      auto line = area;
      g.setFont(type::body());
      g.setColour(colour::muted);
      g.drawText(row.label, line.removeFromLeft(line.getWidth() / 2 - 20), juce::Justification::centredLeft, true);
      g.setFont(type::mono(13.0f));
      g.setColour(colour::text);
      g.drawText(row.value, line, juce::Justification::centredRight, true);
      if (row.dot) {
        const int textWidth = parts::textWidth(type::mono(13.0f), row.value);
        g.setColour(running() ? colour::ok : colour::faint);
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({static_cast<float>(line.getRight() - textWidth - 14), static_cast<float>(line.getCentreY())}));
      }
      area.translate(0, 38);
    }
  }

  // ---- Keep playing ----
  parts::paintCard(g, l.keep);
  paint::label(g, "Keep playing", {l.keep.getX() + 24, l.keep.getY() + 20, 200, 16});
  parts::drawWrapped(g,
                     "Training does not need this window. Leave this page and play as normal: the trainer keeps going, and Capture shows where it "
                     "is when you come back.",
                     type::body(), colour::muted, juce::Rectangle<int>(l.keep.getX() + 24, l.keep.getY() + 20 + 16 + 12, l.keep.getWidth() - 48, 80));
  parts::paintLater(g, juce::Rectangle<int>(minimise.getRight() + 10, minimise.getY() + 7, paint::chipWidth(parts::kLater), 22));

  // ---- The trainer's last lines ----
  parts::paintCard(g, l.output);
  paint::label(g, "Trainer output", {l.output.getX() + 24, l.output.getY() + 16, 200, 16});
  {
    auto area = juce::Rectangle<int>(l.output.getX() + 24, l.output.getY() + 16 + 16 + 8, l.output.getWidth() - 48, l.output.getHeight() - 16 - 16 - 8 - 12);
    g.setFont(type::mono(11.0f, type::Weight::regular));
    if (log.isEmpty()) {
      g.setColour(colour::faint);
      g.drawText("Nothing yet.", area.removeFromTop(16), juce::Justification::centredLeft, true);
    } else {
      const int fit = juce::jmax(1, area.getHeight() / 15);
      g.setColour(colour::muted);
      for (int i = juce::jmax(0, log.size() - fit); i < log.size(); ++i)
        g.drawText(log[i], area.removeFromTop(15), juce::Justification::centredLeft, true);
    }
  }
}

}  // namespace pedaleira::ui
