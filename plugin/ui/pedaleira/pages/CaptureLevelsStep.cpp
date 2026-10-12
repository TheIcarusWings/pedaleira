#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

#include <cmath>

namespace pedaleira::ui {

using capture::LevelState;

namespace {

constexpr int kRightWidth = 400;
constexpr int kColumnGap = 24;
constexpr int kMeterHeight = 88;
constexpr int kMeterHeightPair = 44;
constexpr double kTicks[] = {-60.0, -36.0, -24.0, -18.0, -12.0, -6.0, 0.0};

juce::String plainDb(double db) {
  return std::abs(db - std::round(db)) < 0.05 ? juce::String(juce::roundToInt(db)) : juce::String(db, 1);
}

juce::Colour stateColour(LevelState state) {
  switch (state) {
    case LevelState::inRange: return colour::ok;
    case LevelState::clipping: return colour::clip;
    case LevelState::low:
    case LevelState::high: return colour::warn;
    case LevelState::silent: break;
  }
  return colour::muted;
}

juce::String stateText(LevelState state) {
  switch (state) {
    case LevelState::inRange: return "In range";
    case LevelState::clipping: return "Clipping";
    case LevelState::low: return "Too low";
    case LevelState::high: return "Too high";
    case LevelState::silent: break;
  }
  return "No signal";
}

}  // namespace

struct CaptureLevelsStep::Layout {
  juce::Rectangle<int> returnCard, sendCard, noiseCard, tipsCard;
  juce::Rectangle<int> header, targetZone, tickZone, legend, readout, states;
  std::array<juce::Rectangle<int>, 2> tracks;
  int trackCount = 1;
};

CaptureLevelsStep::CaptureLevelsStep(CaptureHost& hostPage) : CaptureStep(hostPage) {
  setTitle("Levels");
  sendKnob.setName({});
  sendKnob.setDiameter(64);
  sendKnob.setDefaultPosition(0.6);
  sendKnob.onChange = [this](double position) {
    pendingSendDb = std::round((position * 60.0 - 60.0) * 2.0) / 2.0;
    sendPending = true;
    sendDb = pendingSendDb;
    repaint();
  };
  addAndMakeVisible(sendKnob);
  toneSwitch.onClick = [this] { setTone(toneSwitch.getToggleState()); };
  addAndMakeVisible(toneSwitch);
  resetButton.onClick = [this] { resetHold(); };
  addAndMakeVisible(resetButton);
  targetButton.onClick = [this] { chooseTarget(); };
  addAndMakeVisible(targetButton);
  noiseButton.onClick = [this] { measureNoise(); };
  addAndMakeVisible(noiseButton);
  lengthBox.onClick = [this] { chooseLength(); };
  addChildComponent(lengthBox);
}

CaptureLevelsStep::~CaptureLevelsStep() = default;

// ####################
// STATE
// ####################
void CaptureLevelsStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty())
    return;
  dryWet = session["method"].toString() == "dryWet";
  sendDb = static_cast<double>(session["levels"]["sendDb"]);
  targetDb = static_cast<double>(session["levels"]["targetDb"]);
  plannedSeconds = static_cast<double>(session["recording"]["plannedSeconds"]);
  channelText = dryWet ? "Inputs " + juce::String(static_cast<int>(session["channels"]["dry"]) + 1) + " and " +
                             juce::String(static_cast<int>(session["channels"]["wet"]) + 1)
                       : "Output " + juce::String(static_cast<int>(session["channels"]["send"]) + 1);
  toneOn = static_cast<bool>(session["audioActive"]);
  sendKnob.setPosition((sendDb + 60.0) / 60.0);
  sendKnob.setVisible(!dryWet);
  lengthBox.setVisible(dryWet);
  lengthBox.setChoice(capture::clockText(plannedSeconds));
  toneSwitch.setToggleState(toneOn, juce::dontSendNotification);
  poll();
  resized();
}

void CaptureLevelsStep::poll() {
  if (id().isEmpty())
    return;
  const EngineReply reply = host.run("capture.levelStatus", object({{"id", id()}}), {});
  pollFailed = !reply.ok;
  if (reply.ok)
    apply(reply.value);
}

void CaptureLevelsStep::apply(const juce::var& status) {
  const bool wasInRange = inRange && measured;
  dryWet = status["method"].toString() == "dryWet";
  toneOn = static_cast<bool>(status["active"]) && (dryWet || static_cast<bool>(status["tone"]));
  const juce::var level = status[dryWet ? "dryLevel" : "returnLevel"];
  holdDb = static_cast<double>(level["holdDb"]);
  peakDb = level["peakDb"].isVoid() ? holdDb : static_cast<double>(level["peakDb"]);
  clipped = static_cast<int>(level["clippedSamples"]);
  if (dryWet) {
    const juce::var wet = status["wetLevel"];
    wetHoldDb = wet.isVoid() ? static_cast<double>(session["levels"]["wetPeakDb"]) : static_cast<double>(wet["holdDb"]);
    wetPeakDb = wet["peakDb"].isVoid() ? wetHoldDb : static_cast<double>(wet["peakDb"]);
    clipped += static_cast<int>(wet["clippedSamples"]);
  }
  state = capture::levelStateOf(status["state"].toString());
  inRange = static_cast<bool>(status["inRange"]);
  measured = static_cast<bool>(status["measured"]);
  targetDb = static_cast<double>(status["target"]["db"]);
  lowDb = static_cast<double>(status["target"]["lowDb"]);
  highDb = static_cast<double>(status["target"]["highDb"]);
  sendDb = sendPending ? pendingSendDb : static_cast<double>(status["sendDb"]);
  noiseState = status["noiseFloor"]["state"].toString();
  noiseDb = static_cast<double>(status["noiseFloor"]["rmsDb"]);
  calibrationText = status["calibration"]["text"].toString();
  toneSwitch.setToggleState(toneOn, juce::dontSendNotification);
  noiseButton.setEnabled(toneOn && noiseState != "measuring");
  if (wasInRange != (inRange && measured))
    host.footerChanged();
  repaint();
}

void CaptureLevelsStep::tick(int) {
  flushSend();
  if (toneOn)
    poll();
}

void CaptureLevelsStep::flushSend() {
  if (!sendPending || id().isEmpty())
    return;
  sendPending = false;
  host.run("capture.setLevels", object({{"id", id()}, {"sendDb", pendingSendDb}}), {});
}

void CaptureLevelsStep::leave() {
  flushSend();
  if (!toneOn || id().isEmpty())
    return;
  toneOn = false;
  toneSwitch.setToggleState(false, juce::dontSendNotification);
  host.run("capture.stopTone", object({{"id", id()}}), {});
}

void CaptureLevelsStep::setTone(bool on) {
  if (id().isEmpty())
    return;
  EngineReply reply;
  if (on) {
    flushSend();
    reply = host.run("capture.startTone", object({{"id", id()}}), "Could not start the test tone");
  } else {
    reply = host.run("capture.stopTone", object({{"id", id()}}), {});
  }
  if (reply.ok)
    apply(reply.value);
  else
    toneSwitch.setToggleState(toneOn, juce::dontSendNotification);
  host.footerChanged();
}

void CaptureLevelsStep::resetHold() {
  if (id().isEmpty())
    return;
  const EngineReply reply = host.run("capture.levelStatus", object({{"id", id()}, {"resetHold", true}}), {});
  if (reply.ok)
    apply(reply.value);
  host.footerChanged();
}

void CaptureLevelsStep::measureNoise() {
  if (!toneOn) {
    host.notice("Start the test tone first", "The noise floor is measured with the tone muted for a moment, so it has to be running.");
    return;
  }
  const EngineReply reply = host.run("capture.measureNoiseFloor", object({{"id", id()}}), "Could not measure the noise floor");
  if (reply.ok)
    apply(reply.value);
}

void CaptureLevelsStep::chooseTarget() {
  juce::PopupMenu menu;
  juce::Component::SafePointer<CaptureLevelsStep> self(this);
  const std::pair<double, const char*> choices[] = {{-12.0, "-12 dB (Pedaleira)"}, {-8.0, "-8 dB (TONE3000's guide)"}};
  for (const auto& [db, text] : choices)
    menu.addItem(text, true, std::abs(db - targetDb) < 0.05, [self, db] {
      if (self == nullptr)
        return;
      const EngineReply reply = self->host.run("capture.setLevels", object({{"id", self->id()}, {"targetDb", db}}), "Could not set the target");
      if (reply.ok)
        self->host.reload();
    });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&targetButton));
}

void CaptureLevelsStep::chooseLength() {
  const double seconds[] = {120.0, 180.0, 240.0, 300.0};
  juce::StringArray names;
  int selected = -1;
  for (int i = 0; i < 4; ++i) {
    names.add(capture::clockText(seconds[i]));
    if (std::abs(seconds[i] - plannedSeconds) < 1.0)
      selected = i;
  }
  juce::Component::SafePointer<CaptureLevelsStep> self(this);
  parts::chooseFrom(lengthBox, names, selected, [self, seconds](int i) {
    if (self == nullptr)
      return;
    const EngineReply reply = self->host.run("capture.setLevels", object({{"id", self->id()}, {"recordSeconds", seconds[i]}}),
                                             "Could not set the length");
    if (reply.ok)
      self->host.reload();
  });
}

CaptureStep::Footer CaptureLevelsStep::footer() const {
  Footer footer;
  footer.primary = "Continue to record";
  footer.primaryEnabled = measured && inRange;
  return footer;
}

void CaptureLevelsStep::primary() {
  leave();
  host.moveTo(capture::Step::record);
}

// ####################
// LAYOUT
// ####################
CaptureLevelsStep::Layout CaptureLevelsStep::layout() const {
  Layout l;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  const int bodyHeight = getHeight() - top;
  const int leftWidth = getWidth() - kRightWidth - kColumnGap;
  l.returnCard = {0, top, leftWidth, bodyHeight};
  const int rightX = getWidth() - kRightWidth;
  l.sendCard = {rightX, top, kRightWidth, 172};
  l.noiseCard = {rightX, l.sendCard.getBottom() + parts::kBlockGap, kRightWidth, 82};
  l.tipsCard = {rightX, l.noiseCard.getBottom() + parts::kBlockGap, kRightWidth, bodyHeight - 172 - 82 - 2 * parts::kBlockGap};

  l.trackCount = dryWet ? 2 : 1;
  const int trackHeight = dryWet ? kMeterHeightPair : kMeterHeight;
  const int meterTotal = dryWet ? 2 * kMeterHeightPair + 12 : kMeterHeight;
  const int blockHeight = 40 + meterTotal + 36 + 16;
  const auto inner = l.returnCard.reduced(24);
  const int fixed = 16 + blockHeight + 64 + 62;
  const int gap = juce::jmax(8, (inner.getHeight() - fixed) / 3);

  l.header = {inner.getX(), inner.getY(), inner.getWidth(), 16};
  const int blockY = l.header.getBottom() + gap;
  l.targetZone = {inner.getX(), blockY, inner.getWidth(), 40};
  for (int i = 0; i < l.trackCount; ++i)
    l.tracks[static_cast<size_t>(i)] = {inner.getX(), blockY + 40 + i * (trackHeight + 12), inner.getWidth(), trackHeight};
  l.tickZone = {inner.getX(), blockY + 40 + meterTotal, inner.getWidth(), 36};
  l.legend = {inner.getX(), l.tickZone.getBottom(), inner.getWidth(), 16};
  l.readout = {inner.getX(), blockY + blockHeight + gap, inner.getWidth(), 64};
  l.states = {inner.getX(), l.readout.getBottom() + gap, inner.getWidth(), 62};
  return l;
}

void CaptureLevelsStep::resized() {
  const Layout l = layout();
  resetButton.setBounds(l.readout.getRight() - resetButton.idealWidth() - 4, l.readout.getCentreY() - 16, resetButton.idealWidth() + 4, 32);
  targetButton.setButtonText("Target " + plainDb(targetDb) + " dB");
  targetButton.setBounds(resetButton.getX() - 8 - targetButton.idealWidth() - 4, l.readout.getCentreY() - 16, targetButton.idealWidth() + 4, 32);

  const auto send = l.sendCard.reduced(20, 0);
  const int bodyTop = l.sendCard.getY() + 16 + 16 + 12;
  sendKnob.setBounds(send.getX() + 8, bodyTop, 68, 68);
  toneSwitch.setBounds(send.getX() + 84 + 20, bodyTop - 2, 40, 22);
  lengthBox.setBounds(send.getX() + 84 + 20, bodyTop + 40, send.getWidth() - 84 - 20, 36);

  noiseButton.setBounds(l.noiseCard.getRight() - 20 - noiseButton.idealWidth() - 4, l.noiseCard.getY() + 10, noiseButton.idealWidth() + 4, 28);
}

// ####################
// PAINT
// ####################
void CaptureLevelsStep::paint(juce::Graphics& g) {
  const juce::String target = plainDb(targetDb);
  parts::paintHeading(g, getLocalBounds(), "Set levels",
                      dryWet ? "Play your guitar. Adjust the input gains until both inputs peak around " + target + " dB."
                             : "We play a test tone through the device. Adjust until the return peaks around " + target + " dB.");
  if (id().isEmpty())
    return;
  const Layout l = layout();

  // ---- The return card ----
  parts::paintCard(g, l.returnCard);
  paint::label(g, dryWet ? "Input levels" : "Return level", l.header);
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText(dryWet ? channelText + " " + type::dot() + " 48 kHz"
                    : "Input " + juce::String(static_cast<int>(session["channels"]["returnIn"]) + 1) + " " + type::dot() + " Line " +
                          type::dot() + " 48 kHz",
             l.header, juce::Justification::centredRight, true);

  const auto fractionX = [&](const juce::Rectangle<int>& area, double db) {
    return area.getX() + static_cast<float>(capture::meterFraction(db)) * area.getWidth();
  };
  const auto& first = l.tracks[0];
  const auto& last = l.tracks[static_cast<size_t>(l.trackCount - 1)];

  // The target label over the band.
  {
    const float x = fractionX(first, targetDb);
    const juce::String value = plainDb(targetDb) + " dB";
    const int labelWidth = parts::textWidth(type::label(), "TARGET") + 6 + parts::textWidth(type::mono(11.0f, type::Weight::regular), value);
    const int left = juce::roundToInt(x) - labelWidth / 2;
    g.setFont(type::label());
    g.setColour(colour::iris);
    g.drawText("TARGET", left, first.getY() - 34, parts::textWidth(type::label(), "TARGET") + 2, 16, juce::Justification::centredLeft, false);
    g.setFont(type::mono(11.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(value, left + parts::textWidth(type::label(), "TARGET") + 6, first.getY() - 34, 70, 16, juce::Justification::centredLeft, false);
  }

  const juce::Colour fill = stateColour(state) == colour::muted ? colour::ok : stateColour(state);
  for (int i = 0; i < l.trackCount; ++i) {
    const auto& track = l.tracks[static_cast<size_t>(i)];
    const double current = i == 0 ? peakDb : wetPeakDb;
    const double hold = i == 0 ? holdDb : wetHoldDb;
    const auto area = track.toFloat();
    g.setColour(colour::drop);
    g.fillRoundedRectangle(area.translated(0.0f, 2.0f), 10.0f);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(area, 10.0f);
    {
      juce::Graphics::ScopedSaveState state(g);
      juce::Path clip;
      clip.addRoundedRectangle(area, 10.0f);
      g.reduceClipRegion(clip);
      // The last 6 dB: amber, then red.
      g.setColour(colour::warn.withAlpha(0.16f));
      g.fillRect(juce::Rectangle<float>(fractionX(track, -6.0), area.getY(), fractionX(track, -3.0) - fractionX(track, -6.0), area.getHeight()));
      g.setColour(colour::clip.withAlpha(0.16f));
      g.fillRect(juce::Rectangle<float>(fractionX(track, -3.0), area.getY(), fractionX(track, 0.0) - fractionX(track, -3.0), area.getHeight()));
      if (current > -90.0) {
        g.setColour(fill);
        g.fillRect(juce::Rectangle<float>(area.getX(), area.getY(), fractionX(track, current) - area.getX(), area.getHeight()));
      }
      g.setColour(colour::ground.withAlpha(0.35f));
      for (const double tick : kTicks)
        g.fillRect(fractionX(track, tick), area.getY(), 1.0f, area.getHeight());
    }
  }
  // The target band, around all the meters.
  {
    const float left = fractionX(first, lowDb);
    const float right = fractionX(first, highDb);
    g.setColour(colour::iris);
    g.drawRoundedRectangle(left, static_cast<float>(first.getY() - 8), right - left, static_cast<float>(last.getBottom() - first.getY() + 16), 8.0f, 2.0f);
  }
  // The peak holds.
  for (int i = 0; i < l.trackCount; ++i) {
    const auto& track = l.tracks[static_cast<size_t>(i)];
    const double hold = i == 0 ? holdDb : wetHoldDb;
    if (hold <= -90.0)
      continue;
    g.setColour(colour::text);
    g.fillRoundedRectangle(fractionX(track, hold) - 1.0f, static_cast<float>(track.getY() - 12), 3.0f,
                           static_cast<float>(track.getHeight() + 24), 1.5f);
  }
  if (dryWet) {
    g.setFont(type::label());
    g.setColour(colour::ground);
    for (int i = 0; i < 2; ++i)
      g.drawText(i == 0 ? "DRY" : "WET", l.tracks[static_cast<size_t>(i)].getX() + 10, l.tracks[static_cast<size_t>(i)].getY(), 60,
                 l.tracks[static_cast<size_t>(i)].getHeight(), juce::Justification::centredLeft, false);
  }
  // The scale under the meters.
  for (const double tick : kTicks) {
    const float x = fractionX(l.tickZone, tick);
    g.setColour(colour::faint);
    g.fillRect(x, static_cast<float>(l.tickZone.getY() + 14), 1.0f, 8.0f);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    const juce::String text = tick == 0.0 ? "0 dB" : juce::String(juce::roundToInt(tick));
    g.drawText(text, juce::roundToInt(x) - 30, l.tickZone.getY() + 22, 60, 14, juce::Justification::centred, false);
  }
  // The legend.
  {
    auto row = l.legend;
    const auto item = [&](const juce::String& text, int swatchWidth, int swatchHeight, bool outline, juce::Colour swatch) {
      auto swatchArea = row.removeFromLeft(swatchWidth).withSizeKeepingCentre(swatchWidth, swatchHeight).toFloat();
      g.setColour(swatch);
      if (outline)
        g.drawRoundedRectangle(swatchArea.reduced(1.0f), 3.0f, 2.0f);
      else
        g.fillRoundedRectangle(swatchArea, 2.0f);
      row.removeFromLeft(6);
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      const int width = parts::textWidth(type::sans(12.0f), text) + 2;
      g.drawText(text, row.removeFromLeft(width), juce::Justification::centredLeft, false);
      row.removeFromLeft(16);
    };
    item("Current level", 12, 8, false, fill);
    item("Peak hold", 3, 12, false, colour::text);
    item("Target band", 12, 10, true, colour::iris);
  }

  // The readout.
  {
    auto area = l.readout;
    const bool shown = measured && holdDb > -90.0;
    if (!dryWet) {
      const juce::String value = shown ? capture::dbText(holdDb) : "--";
      g.setFont(type::mono(56.0f));
      g.setColour(shown ? colour::text : colour::faint);
      const int valueWidth = parts::textWidth(type::mono(56.0f), value);
      g.drawText(value, area.getX(), area.getY(), valueWidth + 4, 64, juce::Justification::centredLeft, false);
      g.setFont(type::sans(24.0f));
      g.setColour(colour::muted);
      g.drawText("dB peak", area.getX() + valueWidth + 12, area.getY() + 14, 110, 36, juce::Justification::centredLeft, false);
      parts::paintPill(g, stateText(state), {area.getX() + valueWidth + 12 + 110 + 12, area.getCentreY() - 14,
                                             parts::pillWidth(stateText(state)), 28},
                       state == LevelState::silent ? colour::control : stateColour(state),
                       state == LevelState::silent ? colour::muted : colour::ground);
    } else {
      int x = area.getX();
      for (int i = 0; i < 2; ++i) {
        const double hold = i == 0 ? holdDb : wetHoldDb;
        const juce::String value = measured && hold > -90.0 ? capture::dbText(hold) : "--";
        g.setFont(type::label());
        g.setColour(colour::muted);
        g.drawText(i == 0 ? "DRY" : "WET", x, area.getY() + 4, 40, 16, juce::Justification::centredLeft, false);
        g.setFont(type::mono(32.0f));
        g.setColour(value == "--" ? colour::faint : colour::text);
        const int valueWidth = parts::textWidth(type::mono(32.0f), value);
        g.drawText(value, x, area.getY() + 20, valueWidth + 4, 40, juce::Justification::centredLeft, false);
        g.setFont(type::sans(16.0f));
        g.setColour(colour::muted);
        g.drawText("dB", x + valueWidth + 8, area.getY() + 28, 30, 28, juce::Justification::centredLeft, false);
        x += 190;
      }
      parts::paintPill(g, stateText(state), {x, area.getCentreY() - 14, parts::pillWidth(stateText(state)), 28},
                       state == LevelState::silent ? colour::control : stateColour(state),
                       state == LevelState::silent ? colour::muted : colour::ground);
    }
  }

  // The three states, the current one marked.
  {
    const int cardWidth = (l.states.getWidth() - 2 * 12) / 3;
    struct Card {
      juce::String title, text;
      juce::Colour colour;
      bool active;
    };
    const bool loud = state == LevelState::high || state == LevelState::clipping;
    const Card cards[3] = {
        {"Too low:", "raise the device's output or the interface gain", colour::warn, state == LevelState::low},
        {"In range:", "peaks between " + plainDb(lowDb) + " and " + plainDb(highDb) + " dB", colour::ok, state == LevelState::inRange},
        {state == LevelState::high ? "Too high:" : "Clipping:", "lower the interface input gain", state == LevelState::high ? colour::warn : colour::clip,
         loud}};
    for (int i = 0; i < 3; ++i) {
      const auto area = juce::Rectangle<int>(l.states.getX() + i * (cardWidth + 12), l.states.getY(), cardWidth, l.states.getHeight());
      const Card& card = cards[i];
      if (card.active) {
        g.setColour(colour::raised);
        g.fillRoundedRectangle(area.toFloat(), 10.0f);
      }
      g.setColour(card.active ? card.colour : colour::line);
      g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 10.0f, 1.0f);
      auto inner = area.reduced(14, 12);
      auto dot = inner.removeFromLeft(10);
      g.setColour(card.colour);
      g.fillEllipse(dot.withHeight(10).withY(dot.getY() + 4).toFloat());
      inner.removeFromLeft(10);
      juce::AttributedString string;
      string.append(card.title + " ", type::sans(13.0f, type::Weight::semibold), card.colour);
      string.append(card.text, type::sans(13.0f), colour::muted);
      string.setLineSpacing(2.0f);
      string.draw(g, inner.toFloat());
    }
  }

  // ---- Send level ----
  parts::paintCard(g, l.sendCard);
  parts::paintCardHeader(g, l.sendCard, dryWet ? "Inputs" : "Send level", channelText);
  {
    const auto send = l.sendCard.reduced(20, 0);
    const int bodyTop = l.sendCard.getY() + 16 + 16 + 12;
    if (!dryWet) {
      g.setFont(type::mono(12.0f, type::Weight::regular));
      g.setColour(colour::text);
      g.drawText(juce::String(sendDb, 1) + " dB", send.getX(), bodyTop + 70, 84, 16, juce::Justification::centred, false);
    }
    const juce::String toneText = dryWet ? (toneOn ? "Listening to the inputs" : "Listen to the inputs") : (toneOn ? "Test tone playing" : "Test tone off");
    g.setFont(type::compact());
    g.setColour(colour::text);
    g.drawText(toneText, send.getX() + 84 + 20 + 50, bodyTop - 2, send.getWidth() - 84 - 70, 22, juce::Justification::centredLeft, true);
    if (!dryWet) {
      const auto bar = juce::Rectangle<int>(send.getX() + 84 + 20, bodyTop + 52, send.getWidth() - 84 - 20, 8);
      g.setFont(type::mono(11.0f, type::Weight::regular));
      g.setColour(colour::muted);
      g.drawText("Send peak", bar.getX(), bar.getY() - 20, 100, 14, juce::Justification::centredLeft, false);
      g.drawText(capture::dbText(sendDb) + " dB", bar.getRight() - 100, bar.getY() - 20, 100, 14, juce::Justification::centredRight, false);
      g.setColour(colour::raised);
      g.fillRoundedRectangle(bar.toFloat(), 4.0f);
      g.setColour(colour::ok);
      g.fillRoundedRectangle(bar.toFloat().withWidth(static_cast<float>(capture::meterFraction(sendDb)) * bar.getWidth()), 4.0f);
      const struct {
        double db;
        const char* text;
      } scale[] = {{-60.0, "-60"}, {-24.0, "-24"}, {-12.0, "-12"}, {0.0, "0"}};
      g.setColour(colour::muted);
      for (const auto& mark : scale)
        g.drawText(mark.text, bar.getX() + juce::roundToInt(static_cast<float>(capture::meterFraction(mark.db)) * bar.getWidth()) - 20, bar.getBottom() + 6,
                   40, 14, juce::Justification::centred, false);
    }
  }

  // ---- Noise floor ----
  parts::paintCard(g, l.noiseCard);
  paint::label(g, "Noise floor", {l.noiseCard.getX() + 20, l.noiseCard.getY() + 16, 200, 16});
  {
    const juce::Rectangle<int> row(l.noiseCard.getX() + 20, l.noiseCard.getY() + 16 + 16 + 4, l.noiseCard.getWidth() - 40, 28);
    g.setFont(type::mono(22.0f));
    if (noiseState == "done") {
      const juce::String value = capture::dbText(noiseDb, 0) + " dB " + type::dot() + " ";
      g.setColour(colour::text);
      g.drawText(value, row, juce::Justification::centredLeft, false);
      const bool clean = noiseDb <= targetDb - 45.0;
      g.setColour(clean ? colour::ok : colour::warn);
      g.drawText(clean ? "OK" : noiseDb <= targetDb - 35.0 ? "Noisy" : "Too noisy",
                 row.withTrimmedLeft(parts::textWidth(type::mono(22.0f), value)), juce::Justification::centredLeft, false);
    } else {
      g.setColour(colour::faint);
      g.drawText(noiseState == "measuring" ? "Measuring..." : "Not measured", row, juce::Justification::centredLeft, false);
    }
  }

  // ---- Tips ----
  parts::paintCard(g, l.tipsCard);
  paint::label(g, "Tips", {l.tipsCard.getX() + 20, l.tipsCard.getY() + 16, 200, 16});
  {
    const bool amp = session["source"].toString() == "ampHead" || session["source"].toString() == "ampCab";
    auto area = l.tipsCard.reduced(20, 0).withTrimmedTop(16 + 16 + 12);
    const auto bullet = [&](const juce::AttributedString& string, int height) {
      auto row = area.removeFromTop(height);
      g.setColour(colour::iris);
      g.fillEllipse(row.removeFromLeft(6).toFloat().withSizeKeepingCentre(6.0f, 6.0f).withY(static_cast<float>(row.getY() + 6)));
      row.removeFromLeft(10);
      string.draw(g, row.toFloat());
      area.removeFromTop(10);
    };
    const auto plain = [&](const juce::String& text) {
      juce::AttributedString string;
      string.append(text, type::compact(), colour::text);
      string.setLineSpacing(2.0f);
      return string;
    };
    if (dryWet) {
      bullet(plain("Play the way you will during the take, hard and soft."), 36);
      bullet(plain("The louder input counts against clipping."), 18);
    } else {
      bullet(plain(amp ? "Set the amp how you want it to sound." : "Set the device how you want it to sound."), 18);
      bullet(plain(amp ? "Do not touch the amp knobs after this step." : "Do not touch the controls after this step."), 18);
    }
    juce::AttributedString calibration;
    calibration.append("Calibration ", type::compact(), colour::text);
    calibration.append(calibrationText.isEmpty() ? juce::String("0 dBFS = +12.5 dBu") : calibrationText, type::mono(12.0f, type::Weight::regular), colour::text);
    calibration.append(" at the interface input will be saved with the capture.", type::compact(), colour::text);
    calibration.setLineSpacing(2.0f);
    bullet(calibration, 54);
  }

  if (pollFailed) {
    g.setFont(type::compact());
    g.setColour(colour::warn);
    g.drawText("The levels could not be read.", l.header.withY(l.header.getY() + 24), juce::Justification::centredLeft, true);
  }
}

}  // namespace pedaleira::ui
