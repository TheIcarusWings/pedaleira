#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

#include <cmath>

namespace pedaleira::ui {

namespace {
constexpr int kStatsHeight = 78;
constexpr int kNoteHeight = 18;
constexpr int kLabelColumn = 168;
}  // namespace

struct CaptureRecordStep::Layout {
  juce::Rectangle<int> input, main, note;
  std::array<juce::Rectangle<int>, 3> stats;
  bool showInput = false;
};

CaptureRecordStep::CaptureRecordStep(CaptureHost& hostPage) : CaptureStep(hostPage), inputFile(hostPage) {
  setTitle("Record");
  inputFile.onChanged = [this] {
    resized();
    repaint();
    host.footerChanged();
  };
  addChildComponent(inputFile);
}

CaptureRecordStep::~CaptureRecordStep() = default;

// ####################
// STATE
// ####################
void CaptureRecordStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty())
    return;
  dryWet = session["method"].toString() == "dryWet";
  plannedSeconds = static_cast<double>(session["recording"]["plannedSeconds"]);
  state = session["recording"]["state"].toString();
  if (state.isEmpty())
    state = "none";
  error = session["recording"]["error"].toString();
  const auto& channels = session["channels"];
  channelText = dryWet ? "Dry: Input " + juce::String(static_cast<int>(channels["dry"]) + 1) + ", wet: Input " +
                             juce::String(static_cast<int>(channels["wet"]) + 1)
                       : "Input " + juce::String(static_cast<int>(channels["returnIn"]) + 1);
  if (!dryWet) {
    const EngineReply settings = host.run("capture.getSettings", {}, {});
    if (settings.ok)
      inputFile.setSettings(settings.value);
  }
  inputFile.setVisible(false);
  if (state == "complete") {
    progress = 1.0;
    recorded = total = static_cast<double>(session["recording"]["seconds"]);
    left = 0.0;
    if (session["checks"]["latencySamples"].isInt() || session["checks"]["latencySamples"].isDouble()) {
      hasLatency = true;
      latencySamples = static_cast<int>(session["checks"]["latencySamples"]);
      latencyMs = static_cast<double>(session["checks"]["latencyMs"]);
    }
  } else if (state == "none" || state == "failed") {
    progress = recorded = left = 0.0;
    total = dryWet ? plannedSeconds : inputFile.seconds();
    hasPeak = hasLatency = false;
    clipped = 0;
  }
  if (recording())
    poll();
  resized();
  repaint();
}

void CaptureRecordStep::poll() {
  if (id().isEmpty())
    return;
  const EngineReply reply = host.run("capture.recordingStatus", object({{"id", id()}}), {});
  if (reply.ok)
    apply(reply.value);
}

void CaptureRecordStep::apply(const juce::var& status) {
  const bool was = recording();
  state = status["state"].toString();
  progress = static_cast<double>(status["progress"]);
  recorded = static_cast<double>(status["recordedSeconds"]);
  total = static_cast<double>(status["totalSeconds"]);
  left = static_cast<double>(status["secondsLeft"]);
  error = status["error"].toString();
  const juce::var peak = status[dryWet ? "dryPeakDb" : "returnPeakDb"];
  if (!peak.isVoid()) {
    hasPeak = true;
    peakDb = static_cast<double>(peak);
    if (dryWet && !status["wetPeakDb"].isVoid())
      wetPeakDb = static_cast<double>(status["wetPeakDb"]);
    clipped = static_cast<int>(status["clippedSamples"]);
  }
  if (const juce::var latency = status["latency"]; latency.isObject()) {
    hasLatency = true;
    latencySamples = static_cast<int>(latency["samples"]);
    latencyMs = static_cast<double>(latency["ms"]);
  }
  repaint();
  // A recording that ended: the session moved on (complete or failed).
  if (was && !recording()) {
    if (state == "complete" || state == "failed")
      host.reload();
  }
}

void CaptureRecordStep::tick(int count) {
  if (recording() && count % 5 == 0)
    poll();
}

void CaptureRecordStep::leave() {}

// ####################
// ACTIONS
// ####################
CaptureStep::Footer CaptureRecordStep::footer() const {
  Footer footer;
  if (recording()) {
    footer.showBack = false;
    footer.secondary = "Stop and retry";
    footer.primary = "Continue";
    footer.primaryEnabled = false;
  } else if (state == "complete") {
    footer.secondary = "Record again";
    footer.primary = checking ? "Checking..." : "Check the recording";
    footer.primaryEnabled = !checking;
    footer.secondaryEnabled = !checking;
    footer.backEnabled = !checking;
  } else {
    footer.primary = "Start recording";
    footer.primaryEnabled = dryWet || inputFile.ready();
  }
  return footer;
}

void CaptureRecordStep::primary() {
  if (state == "complete") {
    checking = true;
    host.footerChanged();
    repaint();
    // The checks read the recording and take a moment: after the screen drew.
    host.later([this] {
      const EngineReply reply = host.run("capture.runChecks", object({{"id", id()}}), "The checks could not run");
      checking = false;
      if (reply.ok)
        host.sessionChanged(reply.value);
      else
        host.footerChanged();
    });
    return;
  }
  start();
}

void CaptureRecordStep::start() {
  if (id().isEmpty())
    return;
  juce::DynamicObject::Ptr args = new juce::DynamicObject();
  args->setProperty("id", id());
  if (!dryWet) {
    if (!inputFile.ready()) {
      host.notice("The standard input file is missing",
                  "Choose the NAM standard input file (v3_0_0.wav) below, then start the recording.");
      return;
    }
    if (inputFile.allowUnrecognised())
      args->setProperty("allowUnrecognisedInput", true);
  }
  const EngineReply reply = host.run("capture.startRecording", juce::var(args.get()), "Could not start the recording");
  if (reply.ok) {
    hasPeak = hasLatency = false;
    clipped = 0;
    apply(reply.value);
  }
  host.footerChanged();
  resized();
}

void CaptureRecordStep::secondary() {
  if (recording())
    host.confirm("Stop and retry?", "The recording so far is thrown away and you can start it again.", "Stop and retry",
                 [this] { discard(); });
  else
    host.confirm("Record again?", "The recording is thrown away and you can record it again.", "Record again", [this] { discard(); });
}

void CaptureRecordStep::discard() {
  const EngineReply reply = host.run("capture.cancelRecording", object({{"id", id()}}), "Could not stop the recording");
  if (reply.ok)
    host.reload();
}

void CaptureRecordStep::back() {
  if (state == "complete")
    host.confirm("Go back to the levels?", "The recording is thrown away. You can record it again after the levels.", "Go back",
                 [this] { host.goBack(); });
  else
    host.goBack();
}

// ####################
// LAYOUT AND PAINT
// ####################
CaptureRecordStep::Layout CaptureRecordStep::layout() const {
  Layout l;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  l.note = {0, getHeight() - kNoteHeight, getWidth(), kNoteHeight};
  const int statsTop = l.note.getY() - parts::kBlockGap - kStatsHeight;
  const int width = (getWidth() - 2 * parts::kBlockGap) / 3;
  for (int i = 0; i < 3; ++i)
    l.stats[static_cast<size_t>(i)] = {i * (width + parts::kBlockGap), statsTop, width, kStatsHeight};
  l.showInput = needsInputFile() && !id().isEmpty();
  int mainTop = top;
  if (l.showInput) {
    l.input = {0, top, getWidth(), inputFile.heightFor(getWidth())};
    mainTop = l.input.getBottom() + parts::kBlockGap;
  }
  l.main = {0, mainTop, getWidth(), statsTop - parts::kBlockGap - mainTop};
  return l;
}

void CaptureRecordStep::resized() {
  const Layout l = layout();
  inputFile.setVisible(l.showInput);
  if (l.showInput)
    inputFile.setBounds(l.input);
}

void CaptureRecordStep::paint(juce::Graphics& g) {
  const juce::String title = recording() ? (dryWet ? "Recording your playing" : "Recording the test signal")
                             : state == "complete" ? "Recording done"
                                                   : (dryWet ? "Record your playing" : "Record the test signal");
  juce::String subtitle;
  if (dryWet)
    subtitle = recording() ? "Play as you will play it: notes, chords, hard and soft. Keep going until the clock runs out."
                           : "Play for about " + capture::clockText(plannedSeconds) + ". Both inputs are recorded at once.";
  else if (state == "complete")
    subtitle = "The take is saved. Next we check it before spending time on training.";
  else {
    const int minutes = juce::jmax(1, juce::roundToInt(total / 60.0));
    subtitle = "Stay quiet and do not touch the device. This takes " +
               (total > 0.0 ? "about " + juce::String(minutes) + (minutes == 1 ? " minute." : " minutes.") : juce::String("a few minutes."));
  }
  parts::paintHeading(g, getLocalBounds(), title, subtitle);
  if (id().isEmpty())
    return;
  const Layout l = layout();

  // ---- The main card ----
  parts::paintCard(g, l.main);
  auto inner = l.main.reduced(24);
  {
    auto row = inner.removeFromTop(64);
    int x = row.getX();
    if (state == "failed") {
      icons::draw(g, icons::path(icons::Icon::warning), juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(row.getY() + 20), 24.0f, 24.0f), colour::warn);
      parts::drawWrapped(g, error.isEmpty() ? juce::String("The recording failed.") : error, type::body(), colour::warn,
                         row.withTrimmedLeft(36).withTrimmedRight(160));
    } else {
      if (recording()) {
        const int width = parts::pillWidth("REC", 28) + 8;
        auto pill = juce::Rectangle<int>(x, row.getCentreY() - 14, width, 28);
        g.setColour(colour::clip);
        g.fillRoundedRectangle(pill.toFloat(), 14.0f);
        g.setColour(colour::ground);
        g.fillEllipse(pill.removeFromLeft(26).toFloat().withSizeKeepingCentre(8.0f, 8.0f).translated(2.0f, 0.0f));
        g.setFont(type::sans(12.0f, type::Weight::bold).withExtraKerningFactor(0.08f));
        g.drawText("REC", pill.withTrimmedLeft(-4), juce::Justification::centredLeft, false);
        x += width + 20;
      }
      const juce::String time = capture::clockText(recorded);
      g.setFont(type::mono(56.0f));
      g.setColour(colour::text);
      const int timeWidth = parts::textWidth(type::mono(56.0f), time);
      g.drawText(time, x, row.getY(), timeWidth + 4, 64, juce::Justification::centredLeft, false);
      g.setFont(type::mono(28.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText(total > 0.0 ? "of " + capture::clockText(total) : juce::String(), x + timeWidth + 14, row.getY() + 12, 200, 40,
                 juce::Justification::centredLeft, false);
    }
    const auto side = row.removeFromRight(160);
    g.setFont(type::mono(20.0f));
    g.setColour(colour::text);
    g.drawText(juce::String(juce::roundToInt(progress * 100.0)) + "%", side.withHeight(26), juce::Justification::centredRight, false);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(recording() ? capture::clockText(left) + " left" : juce::String(), side.withTrimmedTop(28).withHeight(16),
               juce::Justification::centredRight, false);
  }
  inner.removeFromTop(20);
  {
    // Progress, with its time axis.
    const auto label = inner.removeFromLeft(kLabelColumn);
    inner.removeFromLeft(10);
    paint::label(g, "Progress", label.withHeight(14).withY(inner.getY() - 1));
    const auto bar = juce::Rectangle<int>(inner.getX(), inner.getY(), inner.getWidth(), 10);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(bar.toFloat(), 5.0f);
    g.setColour(colour::iris);
    g.fillRoundedRectangle(bar.toFloat().withWidth(static_cast<float>(progress) * bar.getWidth()), 5.0f);
    if (total > 0.0) {
      g.setFont(type::mono(11.0f, type::Weight::regular));
      g.setColour(colour::muted);
      std::vector<double> marks{0.0};
      for (double mark = 60.0; mark < total - 12.0; mark += 60.0)
        marks.push_back(mark);
      marks.push_back(total);
      for (const double mark : marks) {
        const int markX = bar.getX() + juce::roundToInt(static_cast<float>(mark / total) * bar.getWidth());
        const bool first = mark == 0.0;
        const bool lastMark = mark == total;
        g.drawText(capture::clockText(mark), first ? markX : lastMark ? markX - 50 : markX - 25, bar.getBottom() + 6, 50, 14,
                   first ? juce::Justification::centredLeft : lastMark ? juce::Justification::centredRight : juce::Justification::centred,
                   false);
      }
    }
    inner.removeFromTop(10 + 6 + 14 + 14);
  }
  {
    // The two lanes: what is sent and what comes back.
    const int available = inner.getHeight();
    if (available >= 56) {
      const int laneHeight = juce::jlimit(26, 66, (available - 8) / 2);
      const auto labels = inner.removeFromLeft(kLabelColumn);
      inner.removeFromLeft(10);
      const juce::String names[2] = {dryWet ? "Dry: " + channelText.upToFirstOccurrenceOf(",", false, false).fromFirstOccurrenceOf(": ", false, false) : "Send: NAM test signal v3",
                                     dryWet ? "Wet: " + channelText.fromFirstOccurrenceOf("wet: ", false, false) : "Return: " + channelText};
      for (int i = 0; i < 2; ++i) {
        const auto lane = juce::Rectangle<int>(inner.getX(), inner.getY() + i * (laneHeight + 8), inner.getWidth(), laneHeight);
        g.setFont(type::sans(12.0f, type::Weight::semibold));
        g.setColour(i == 0 && !dryWet ? colour::muted : colour::text);
        g.drawText(names[i], labels.getX(), lane.getY(), labels.getWidth(), lane.getHeight(), juce::Justification::centredLeft, true);
        g.setColour(colour::ground);
        g.fillRoundedRectangle(lane.toFloat(), 8.0f);
        g.setColour(colour::line);
        g.fillRect(lane.getX(), lane.getCentreY(), lane.getWidth(), 1);
      }
      const int lanesBottom = inner.getY() + 2 * laneHeight + 8;
      // The playhead.
      const float headX = static_cast<float>(inner.getX()) + static_cast<float>(progress) * inner.getWidth();
      if (recording() || state == "complete") {
        g.setColour(colour::iris);
        g.fillRect(headX - 1.0f, static_cast<float>(inner.getY()), 2.0f, static_cast<float>(lanesBottom - inner.getY()));
        juce::Path flag;
        flag.addTriangle(headX - 6.0f, static_cast<float>(inner.getY()), headX + 6.0f, static_cast<float>(inner.getY()), headX,
                         static_cast<float>(inner.getY()) + 8.0f);
        g.fillPath(flag);
      }
      // The engine reports progress and peaks, not the waveform.
      const int chipWidth = paint::chipWidth("Waveform: " + juce::String(parts::kLater));
      paint::chip(g, "Waveform: " + juce::String(parts::kLater),
                  juce::Rectangle<int>(inner.getCentreX() - chipWidth / 2, inner.getY() + laneHeight + 4 - 11, chipWidth, 22).toFloat(),
                  paint::ChipStyle::dashed);
    }
  }

  // ---- The three readings ----
  const char* const labels[3] = {"Latency measured", dryWet ? "Peak, dry / wet" : "Return peak", "Clipping"};
  for (int i = 0; i < 3; ++i) {
    const auto card = l.stats[static_cast<size_t>(i)];
    parts::paintCard(g, card);
    paint::label(g, labels[i], {card.getX() + 20, card.getY() + 14, card.getWidth() - 40, 16});
    const auto row = juce::Rectangle<int>(card.getX() + 20, card.getY() + 14 + 16 + 6, card.getWidth() - 40, 28);
    g.setFont(type::mono(20.0f));
    g.setColour(colour::text);
    if (i == 0) {
      if (hasLatency) {
        const juce::String text = juce::String(latencySamples) + " samples " + type::dot() + " " + juce::String(latencyMs, 1) + " ms";
        g.drawText(text, row, juce::Justification::centredLeft, false);
        const int chipWidth = paint::chipWidth("Auto-aligned");
        paint::chip(g, "Auto-aligned", juce::Rectangle<int>(row.getRight() - chipWidth, row.getCentreY() - 11, chipWidth, 22).toFloat(),
                    paint::ChipStyle::ok);
      } else {
        g.setColour(colour::faint);
        g.drawText(dryWet ? "Measured in the check" : recording() ? "Measuring..." : "--", row, juce::Justification::centredLeft, false);
      }
    } else if (i == 1) {
      if (hasPeak) {
        g.drawText(dryWet ? capture::dbText(peakDb) + " / " + capture::dbText(wetPeakDb) + " dB" : capture::dbText(peakDb) + " dB", row,
                   juce::Justification::centredLeft, false);
      } else {
        g.setColour(colour::faint);
        g.drawText("--", row, juce::Justification::centredLeft, false);
      }
    } else {
      const bool any = hasPeak && clipped > 0;
      g.setColour(hasPeak ? colour::text : colour::faint);
      g.drawText(!hasPeak ? juce::String("--") : any ? juce::String(clipped) + (clipped == 1 ? " sample" : " samples") : juce::String("None"), row,
                 juce::Justification::centredLeft, false);
      if (hasPeak) {
        g.setColour(any ? colour::clip : colour::ok);
        g.fillEllipse(juce::Rectangle<float>(static_cast<float>(row.getRight() - 8), static_cast<float>(row.getCentreY() - 4), 8.0f, 8.0f));
      }
    }
  }

  // ---- The note ----
  auto note = l.note;
  icons::draw(g, icons::path(icons::Icon::info), note.removeFromLeft(18).toFloat(), colour::muted);
  g.setFont(type::compact());
  g.setColour(colour::muted);
  g.drawText("Audio from other rows is muted while recording.", note.withTrimmedLeft(8), juce::Justification::centredLeft, true);
}

}  // namespace pedaleira::ui
