#include "Tone3000Panel.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kHeaderHeight = 44;
constexpr int kTabsHeight = 38;
constexpr int kDetailWidth = 340;
constexpr int kTableHead = 24;
constexpr int kRowHeight = 56;
constexpr int kCaptureRow = 34;
constexpr int kMaxCaptureRows = 6;
constexpr int kMessageHeight = 92;
const char* const kSite = "https://www.tone3000.com";

Category categoryOfGear(const juce::String& gear) {
  const juce::String id = gear.toLowerCase();
  if (id == "amp" || id == "amp-cab" || id == "amp_cab" || id == "full-rig" || id == "full_rig")
    return Category::amp;
  if (id == "pedal")
    return Category::drive;
  if (id == "cab" || id == "ir")
    return Category::cab;
  if (id == "outboard")
    return Category::eq;
  if (id == "space" || id == "spaces")
    return Category::reverb;
  return Category::utility;
}

juce::String gearLabel(const juce::String& gear) {
  const juce::String id = gear.toLowerCase();
  if (id == "amp")
    return "Amp Head";
  if (id == "amp-cab" || id == "amp_cab" || id == "full-rig" || id == "full_rig")
    return "Amp + Cab";
  if (id == "cab")
    return "Cabinet";
  if (id == "space" || id == "spaces")
    return "Spaces";
  if (id.isEmpty())
    return "Other";
  return id.substring(0, 1).toUpperCase() + id.substring(1);
}

juce::String formatLabel(const juce::String& format) {
  return format.equalsIgnoreCase("nam") ? juce::String("NAM A2") : format.equalsIgnoreCase("ir") ? juce::String("IR")
                                                                                                 : format.toUpperCase();
}

juce::String capturesText(const Tone3000Tone& tone) {
  const juce::String unit = tone.format.equalsIgnoreCase("ir") ? "IR" : "capture";
  return juce::String(tone.captures) + " " + unit + (tone.captures == 1 ? "" : "s");
}

// What to tell the player of a failed request: the client's errors end in the HTTP
// status ("listTrending failed: 503"), 0 when nothing answered.
juce::String reachText(const juce::String& error) {
  const int status = error.fromLastOccurrenceOf(": ", false, false).getIntValue();
  if (error.contains("failed: ") && status > 0)
    return "TONE3000 answered with an error (" + juce::String(status) + "). Try again in a moment.";
  return "TONE3000 could not be reached. Check the connection and try again.";
}

}  // namespace

Tone3000Panel::Tone3000Panel() {
  setTitle("TONE3000");
  setWantsKeyboardFocus(false);
  browse.onClick = [] { juce::URL(kSite).launchInDefaultBrowser(); };
  retry.onClick = [this] { loadTab(tab, true); };
  openTone.onClick = [this] {
    if (chosenTone < 0 || chosenTone >= toneCount())
      return;
    const auto& tone = state().result.tones[static_cast<size_t>(chosenTone)];
    juce::URL(tone.url.isNotEmpty() ? tone.url : juce::String(kSite) + "/tones/" + juce::String(tone.id)).launchInDefaultBrowser();
  };
  add.onClick = [this] { addSelected(); };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&browse, &retry, &openTone, &add})
    addAndMakeVisible(c);
  retry.setVisible(false);
  openTone.setVisible(false);
  add.setVisible(false);
}

void Tone3000Panel::setFeed(Tone3000Feed* newFeed) {
  feed = newFeed;
  for (auto& t : tabs)
    t = {};
  chosenTone = chosenCapture = -1;
  capturesState = CapturesState::none;
  message = {};
}

void Tone3000Panel::setTarget(int row, int slot) {
  targetRow = row;
  targetSlot = slot;
  add.setButtonText("Add to Row " + juce::String(row + 1) + ", slot " + juce::String(slot + 1));
  resized();
}

void Tone3000Panel::show() {
  scroll = 0;
  message = {};
  if (feed != nullptr)
    loadTab(tab, false);
  resized();
  repaint();
}

int Tone3000Panel::toneCount() const { return static_cast<int>(state().result.tones.size()); }

bool Tone3000Panel::canAdd() const {
  return feed != nullptr && chosenTone >= 0 && chosenCapture >= 0 && !adding && capturesState == CapturesState::ready;
}

// ####################
// DATA
// ####################
void Tone3000Panel::selectTab(Tone3000List which) {
  tab = which;
  chosenTone = chosenCapture = -1;
  capturesState = CapturesState::none;
  message = {};
  scroll = 0;
  if (feed != nullptr)
    loadTab(which, false);
  resized();
  repaint();
}

void Tone3000Panel::loadTab(Tone3000List which, bool force) {
  TabState& target = tabs[static_cast<size_t>(which)];
  if (feed == nullptr || target.loading || (target.loaded && !force))
    return;
  target.loading = true;
  juce::Component::SafePointer<Tone3000Panel> self(this);
  feed->list(which, [self, which](Tone3000Feed::Listed listed) {
    if (self == nullptr)
      return;
    TabState& done = self->tabs[static_cast<size_t>(which)];
    done.loading = false;
    done.loaded = true;
    done.result = std::move(listed);
    self->resized();
    self->repaint();
  });
}

void Tone3000Panel::selectTone(int index) {
  if (index < 0 || index >= toneCount())
    return;
  chosenTone = index;
  chosenCapture = -1;
  captureList = {};
  message = {};
  loadCaptures();
  resized();
  repaint();
}

void Tone3000Panel::loadCaptures() {
  capturesState = CapturesState::loading;
  const int generation = ++captureGeneration;
  const Tone3000Tone tone = state().result.tones[static_cast<size_t>(chosenTone)];
  juce::Component::SafePointer<Tone3000Panel> self(this);
  feed->captures(tone, [self, generation](Tone3000Feed::Captures found) {
    if (self == nullptr || generation != self->captureGeneration)
      return;
    self->captureList = std::move(found);
    self->capturesState = self->captureList.ok ? CapturesState::ready : CapturesState::failed;
    self->chosenCapture = self->captureList.ok && !self->captureList.captures.empty() ? 0 : -1;
    self->resized();
    self->repaint();
  });
}

void Tone3000Panel::selectCapture(int index) {
  if (capturesState != CapturesState::ready || index < 0 || index >= static_cast<int>(captureList.captures.size()))
    return;
  chosenCapture = index;
  resized();
  repaint();
}

void Tone3000Panel::addSelected() {
  if (!canAdd())
    return;
  const Tone3000Tone tone = state().result.tones[static_cast<size_t>(chosenTone)];
  const Tone3000Capture capture = captureList.captures[static_cast<size_t>(chosenCapture)];
  adding = true;
  message = {};
  resized();
  repaint();
  juce::Component::SafePointer<Tone3000Panel> self(this);
  feed->download(tone, capture, [self, tone, capture](Tone3000Feed::Downloaded got) {
    if (self == nullptr)
      return;
    const auto finish = [self](const juce::String& error) {
      if (self == nullptr)
        return;
      self->adding = false;
      self->message = error;
      self->resized();
      self->repaint();
    };
    if (got.error.isNotEmpty() || !got.file.existsAsFile()) {
      finish(got.error.isNotEmpty() ? got.error : juce::String("The download did not arrive."));
      return;
    }
    if (!self->onAddFile) {
      finish("Nothing is waiting for a capture here.");
      return;
    }
    self->onAddFile(got.file, tone.title + " - " + capture.name, finish);
  });
}

// ####################
// LAYOUT
// ####################
juce::Rectangle<int> Tone3000Panel::headerArea() const { return getLocalBounds().removeFromTop(kHeaderHeight); }

juce::Rectangle<int> Tone3000Panel::tabBounds(int index) const {
  int x = 0;
  for (int i = 0; i < index; ++i)
    x += juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::bodyStrong(), tone3000ListName(tabAt(i)))) + 36;
  const int width = juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::bodyStrong(), tone3000ListName(tabAt(index)))) + 36;
  return {x, kHeaderHeight, width, kTabsHeight};
}

juce::Rectangle<int> Tone3000Panel::tableArea() const {
  auto area = getLocalBounds().withTrimmedTop(kHeaderHeight + kTabsHeight + 8);
  area.removeFromRight(kDetailWidth + 16);
  return area;
}

juce::Rectangle<int> Tone3000Panel::detailArea() const {
  auto area = getLocalBounds().withTrimmedTop(kHeaderHeight + kTabsHeight + 8);
  return area.removeFromRight(kDetailWidth);
}

juce::Rectangle<int> Tone3000Panel::rowBounds(int index) const {
  const auto area = tableArea();
  return {area.getX(), area.getY() + kTableHead + index * (kRowHeight + 2) - scroll, area.getWidth(), kRowHeight};
}

int Tone3000Panel::rowAt(juce::Point<int> position) const {
  const auto area = tableArea().withTrimmedTop(kTableHead);
  if (!area.contains(position))
    return -1;
  for (int i = 0; i < toneCount(); ++i)
    if (rowBounds(i).contains(position))
      return i;
  return -1;
}

namespace {
// Where the detail column puts things, from its top.
struct DetailLayout {
  juce::Rectangle<int> title, creator, chips, description, licence, captureLabel;
  int capturesTop = 0;
};

DetailLayout detailLayout(juce::Rectangle<int> area) {
  DetailLayout l;
  auto r = area.reduced(16, 16);
  l.title = r.removeFromTop(46);
  r.removeFromTop(4);
  l.creator = r.removeFromTop(20);
  r.removeFromTop(8);
  l.chips = r.removeFromTop(24);
  r.removeFromTop(10);
  l.description = r.removeFromTop(84);
  r.removeFromTop(6);
  l.licence = r.removeFromTop(18);
  r.removeFromTop(14);
  l.captureLabel = r.removeFromTop(16);
  r.removeFromTop(8);
  l.capturesTop = r.getY();
  return l;
}
}  // namespace

juce::Rectangle<int> Tone3000Panel::captureBounds(int index) const {
  const auto l = detailLayout(detailArea());
  return {l.captureLabel.getX(), l.capturesTop + index * (kCaptureRow + 4), l.captureLabel.getWidth(), kCaptureRow};
}

void Tone3000Panel::resized() {
  const auto header = headerArea();
  const int browseWidth = browse.idealWidth() + 8;
  browse.setBounds(header.getRight() - browseWidth, header.getY() + 6, browseWidth, 32);
  const auto detail = detailArea();
  const bool haveTone = chosenTone >= 0 && chosenTone < toneCount();
  add.setBounds(detail.getX() + 16, detail.getBottom() - 16 - 40, detail.getWidth() - 32, 40);
  openTone.setBounds(detail.getX() + 16, add.getY() - 8 - 36, detail.getWidth() - 32, 36);
  add.setVisible(haveTone);
  openTone.setVisible(haveTone);
  add.setEnabled(canAdd());
  const auto table = tableArea();
  retry.setBounds(table.getCentreX() - 50, table.getY() + kTableHead + 30 + kMessageHeight + 12, 100, 32);
  retry.setVisible(feed != nullptr && state().loaded && !state().result.ok && !state().result.needsAccount);
}

// ####################
// PAINT
// ####################
juce::String Tone3000Panel::emptyText() const {
  if (feed == nullptr)
    return "Browsing TONE3000 is not available in this build.";
  const TabState& s = state();
  if (tone3000ListNeedsAccount(tab) && !signedIn() && !(s.loaded && s.result.ok))
    return juce::String(tone3000ListName(tab)) + " needs a TONE3000 account. Signing in from Pedaleira is not available yet. "
                                                 "Trending and Latest work without one.";
  if (s.loading || !s.loaded)
    return juce::String::fromUTF8("Loading from TONE3000\xe2\x80\xa6");
  if (!s.result.ok)
    return s.result.needsAccount ? juce::String(tone3000ListName(tab)) + " needs a TONE3000 account. Signing in from Pedaleira is not "
                                                                         "available yet. Trending and Latest work without one."
                                 : reachText(s.result.error);
  return "Nothing here yet.";
}

void Tone3000Panel::paint(juce::Graphics& g) {
  // Who is signed in, and the attribution TONE3000 asks for.
  const auto header = headerArea();
  g.setFont(type::sans(12.0f, type::Weight::semibold));
  g.setColour(colour::muted);
  g.drawText("Powered by TONE3000", header.getX(), header.getY(), 160, header.getHeight(), juce::Justification::centredLeft, false);
  const bool in = signedIn();
  const juce::String who = !in ? juce::String("Not signed in to TONE3000")
                               : feed->userName().isNotEmpty() ? "Signed in as " + feed->userName() : juce::String("Signed in to TONE3000");
  const auto dotArea = juce::Rectangle<float>(static_cast<float>(header.getX() + 176), header.getCentreY() - 4.0f, 8.0f, 8.0f);
  if (in) {
    g.setColour(colour::ok);
    g.fillEllipse(dotArea);
  } else {
    g.setColour(colour::muted);
    g.drawEllipse(dotArea.reduced(1.0f), 2.0f);
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(who, header.getX() + 192, header.getY(), 260, header.getHeight(), juce::Justification::centredLeft, true);

  // The tabs.
  g.setColour(colour::line);
  g.fillRect(0, kHeaderHeight + kTabsHeight - 1, getWidth(), 1);
  for (int i = 0; i < kTabCount; ++i) {
    const auto r = tabBounds(i);
    const bool on = tabAt(i) == tab;
    const bool locked = tone3000ListNeedsAccount(tabAt(i)) && !in;
    g.setFont(type::bodyStrong());
    g.setColour(on ? colour::text : (locked ? colour::faint : colour::muted));
    g.drawText(tone3000ListName(tabAt(i)), r, juce::Justification::centred, false);
    if (on) {
      g.setColour(colour::iris);
      g.fillRect(r.getX() + 8, r.getBottom() - 2, r.getWidth() - 16, 2);
    }
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  const juce::String note = tab == Tone3000List::trending ? "Top tones this week" : tab == Tone3000List::latest ? "The newest tones" : "Your own tones";
  g.drawText(note, getWidth() - kDetailWidth - 16 - 220, kHeaderHeight, 220, kTabsHeight, juce::Justification::centredRight, true);

  // The table.
  const auto table = tableArea();
  const int nameWidth = table.getWidth() - 150 - 90 - 100 - 3 * 10 - 20;
  {
    auto head = table.withHeight(kTableHead).reduced(10, 0);
    paint::label(g, "Tone and creator", head.removeFromLeft(nameWidth));
    head.removeFromLeft(10);
    paint::label(g, "Gear", head.removeFromLeft(150));
    head.removeFromLeft(10);
    paint::label(g, "Format", head.removeFromLeft(90));
    head.removeFromLeft(10);
    paint::label(g, "Captures", head.removeFromLeft(100));
  }
  {
    juce::Graphics::ScopedSaveState saved(g);
    g.reduceClipRegion(table.withTrimmedTop(kTableHead));
    const TabState& s = state();
    if (feed != nullptr && s.loaded && s.result.ok && !s.result.tones.empty())
      for (int i = 0; i < toneCount(); ++i)
        paintRow(g, s.result.tones[static_cast<size_t>(i)], rowBounds(i), i == chosenTone, i == hoverRow);
    else
      paintMessage(g, table.withTrimmedTop(kTableHead));
  }

  // The detail column.
  const auto detail = detailArea();
  g.setColour(colour::line);
  g.fillRect(detail.getX() - 1, detail.getY(), 1, detail.getHeight());
  paintDetail(g, detail);
}

void Tone3000Panel::paintMessage(juce::Graphics& g, juce::Rectangle<int> area) {
  const juce::String text = emptyText();
  const bool needsAccount = text.contains("needs a TONE3000 account");
  auto box = area.withSizeKeepingCentre(juce::jmin(520, area.getWidth() - 40), kMessageHeight).withY(area.getY() + 30);
  g.setColour(colour::raised);
  g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
  auto inner = box.reduced(16, 12);
  if (needsAccount) {
    icons::draw(g, icons::path(icons::Icon::lock), inner.removeFromLeft(24).removeFromTop(24).toFloat().reduced(2.0f), colour::muted);
    inner.removeFromLeft(8);
  }
  g.setFont(type::body());
  g.setColour(colour::text);
  g.drawFittedText(text, inner, juce::Justification::centredLeft, 4, 1.0f);
}

void Tone3000Panel::paintRow(juce::Graphics& g, const Tone3000Tone& tone, juce::Rectangle<int> row, bool selected,
                             bool hovered) {
  if (selected || hovered) {
    g.setColour(colour::raised);
    g.fillRoundedRectangle(row.toFloat(), size::buttonRadius);
  }
  if (selected) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(row.toFloat().reduced(1.0f), size::buttonRadius, 2.0f);
  }
  const int nameWidth = row.getWidth() - 150 - 90 - 100 - 3 * 10 - 20;
  auto r = row.reduced(10, 0);
  auto name = r.removeFromLeft(nameWidth);
  const Category category = categoryOfGear(tone.format.equalsIgnoreCase("ir") ? juce::String("ir") : tone.gear);
  const auto tile = name.removeFromLeft(40).withSizeKeepingCentre(40, 40).toFloat();
  g.setColour(categoryTint(category));
  g.fillRoundedRectangle(tile, 8.0f);
  icons::draw(g, icons::path(category), tile.reduced(9.0f), categoryColour(category));
  name.removeFromLeft(12);
  g.setFont(type::sans(13.0f, type::Weight::semibold));
  g.setColour(colour::text);
  g.drawText(tone.title, name.removeFromTop(row.getHeight() / 2 + 4).withTrimmedTop(6), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(tone.creator + (tone.creatorVerified ? juce::String("  ") + type::dot() + "  Verified" : juce::String()),
             name.withTrimmedBottom(6), juce::Justification::centredLeft, true);
  r.removeFromLeft(10);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::text);
  g.drawText(gearLabel(tone.gear), r.removeFromLeft(150), juce::Justification::centredLeft, true);
  r.removeFromLeft(10);
  g.setFont(type::mono(12.0f, type::Weight::regular));
  g.setColour(colour::muted);
  g.drawText(formatLabel(tone.format), r.removeFromLeft(90), juce::Justification::centredLeft, true);
  r.removeFromLeft(10);
  g.setFont(type::sans(12.0f));
  g.drawText(capturesText(tone), r.removeFromLeft(100), juce::Justification::centredLeft, true);
}

void Tone3000Panel::paintDetail(juce::Graphics& g, juce::Rectangle<int> area) {
  const bool haveTone = chosenTone >= 0 && chosenTone < toneCount();
  if (!haveTone) {
    auto box = area.reduced(16, 16).removeFromTop(86);
    paint::dashedOutline(g, box.toFloat(), size::buttonRadius, colour::faint);
    g.setFont(type::sans(13.0f));
    g.setColour(colour::muted);
    g.drawFittedText(feed == nullptr ? juce::String("Nothing to show.") : juce::String("Pick a tone to see its captures and add one to Row ") +
                                                               juce::String(targetRow + 1) + ", slot " + juce::String(targetSlot + 1) + ".",
                     box.reduced(14, 10), juce::Justification::centredLeft, 4, 1.0f);
    return;
  }
  const Tone3000Tone& tone = state().result.tones[static_cast<size_t>(chosenTone)];
  const auto l = detailLayout(area);
  g.setFont(type::sans(18.0f, type::Weight::semibold));
  g.setColour(colour::text);
  g.drawFittedText(tone.title, l.title, juce::Justification::topLeft, 2, 1.0f);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(tone.creator + (tone.creatorVerified ? juce::String("  ") + type::dot() + "  Verified" : juce::String()), l.creator,
             juce::Justification::centredLeft, true);
  {
    auto chips = l.chips;
    for (const auto& text : {gearLabel(tone.gear), formatLabel(tone.format)}) {
      const int w = paint::chipWidth(text);
      paint::chip(g, text, chips.removeFromLeft(w).toFloat(), paint::ChipStyle::outline);
      chips.removeFromLeft(8);
    }
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::text);
  g.drawFittedText(tone.description.isNotEmpty() ? tone.description : juce::String("No description."), l.description,
                   juce::Justification::topLeft, 5, 1.0f);
  g.setColour(colour::muted);
  g.drawText("Licence: " + (tone.licence.isNotEmpty() ? tone.licence : juce::String("see the tone on TONE3000")), l.licence,
             juce::Justification::centredLeft, true);
  paint::label(g, tone.format.equalsIgnoreCase("ir") ? "IRs in this tone" : "Captures in this tone", l.captureLabel);

  if (capturesState == CapturesState::ready) {
    const int shown = juce::jmin(static_cast<int>(captureList.captures.size()), kMaxCaptureRows);
    for (int i = 0; i < shown; ++i) {
      const auto r = captureBounds(i);
      const bool on = i == chosenCapture;
      g.setColour(on ? colour::control : colour::ground);
      g.fillRoundedRectangle(r.toFloat(), size::buttonRadius);
      g.setColour(on ? colour::iris : colour::line);
      g.drawRoundedRectangle(r.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
      const auto ring = juce::Rectangle<float>(static_cast<float>(r.getX() + 12), r.getCentreY() - 7.0f, 14.0f, 14.0f);
      g.setColour(on ? colour::iris : colour::muted);
      g.drawEllipse(ring.reduced(1.0f), 1.75f);
      if (on)
        g.fillEllipse(ring.reduced(4.5f));
      g.setFont(type::sans(13.0f, type::Weight::medium));
      g.setColour(colour::text);
      g.drawText(captureList.captures[static_cast<size_t>(i)].name, r.withTrimmedLeft(36).withTrimmedRight(10),
                 juce::Justification::centredLeft, true);
    }
    if (static_cast<int>(captureList.captures.size()) > shown) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("and " + juce::String(static_cast<int>(captureList.captures.size()) - shown) + " more on TONE3000",
                 captureBounds(shown).withHeight(20), juce::Justification::centredLeft, false);
    }
    if (captureList.captures.empty()) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText("Pedaleira loads no capture of this tone (A2 only).", captureBounds(0), juce::Justification::centredLeft, true);
    }
  } else {
    const juce::String text = capturesState == CapturesState::loading ? juce::String::fromUTF8("Loading\xe2\x80\xa6")
                              : captureList.needsAccount
                                  ? juce::String("Adding a capture needs a TONE3000 account. Signing in from Pedaleira is not "
                                                 "available yet; open the tone on TONE3000 to download it, then load the file.")
                                  : "The captures could not be loaded. " + reachText(captureList.error);
    g.setFont(type::sans(12.0f));
    g.setColour(capturesState == CapturesState::failed && !captureList.needsAccount ? colour::warn : colour::muted);
    g.drawFittedText(text, juce::Rectangle<int>(l.captureLabel.getX(), l.capturesTop, l.captureLabel.getWidth(), 90),
                     juce::Justification::topLeft, 5, 1.0f);
  }

  // What went wrong with the last Add, over the buttons.
  if (message.isNotEmpty()) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::warn);
    g.drawFittedText(message, juce::Rectangle<int>(area.getX() + 16, openTone.getY() - 62, area.getWidth() - 32, 54),
                     juce::Justification::bottomLeft, 3, 1.0f);
  } else if (adding) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(juce::String::fromUTF8("Downloading\xe2\x80\xa6"), openTone.getX(), openTone.getY() - 28, 200, 20,
               juce::Justification::centredLeft, false);
  }
}

// ####################
// MOUSE
// ####################
void Tone3000Panel::mouseMove(const juce::MouseEvent& event) {
  const int now = rowAt(event.getPosition());
  if (now != hoverRow) {
    hoverRow = now;
    repaint(tableArea());
  }
}

void Tone3000Panel::mouseExit(const juce::MouseEvent&) {
  if (hoverRow != -1) {
    hoverRow = -1;
    repaint(tableArea());
  }
}

void Tone3000Panel::mouseUp(const juce::MouseEvent& event) {
  const auto at = event.getPosition();
  for (int i = 0; i < kTabCount; ++i)
    if (tabBounds(i).contains(at)) {
      selectTab(tabAt(i));
      return;
    }
  if (const int row = rowAt(at); row >= 0) {
    selectTone(row);
    return;
  }
  if (capturesState == CapturesState::ready)
    for (int i = 0; i < juce::jmin(static_cast<int>(captureList.captures.size()), kMaxCaptureRows); ++i)
      if (captureBounds(i).contains(at)) {
        selectCapture(i);
        return;
      }
}

void Tone3000Panel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
  const int content = toneCount() * (kRowHeight + 2);
  const int maxScroll = juce::jmax(0, content + kTableHead - tableArea().getHeight());
  scroll = juce::jlimit(0, maxScroll, scroll - juce::roundToInt(wheel.deltaY * 200.0f));
  repaint(tableArea());
}

}  // namespace pedaleira::ui
