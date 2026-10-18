#include "Tone3000Panel.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <algorithm>

namespace pedaleira::ui {

namespace {

constexpr int kHeaderHeight = 44;
constexpr int kTabsHeight = 38;
constexpr int kTableHead = 24;
// Rows with the gear, format and captures in columns, and rows that fold them
// under the title when the table is narrow.
constexpr int kWideRow = 60;
constexpr int kCompactRow = 70;
constexpr int kGearColumn = 120;
constexpr int kFormatColumn = 76;
constexpr int kCapturesColumn = 86;
constexpr int kLeastNameColumn = 300;
constexpr int kCaptureRow = 34;
constexpr int kMaxCaptureRows = 6;
constexpr int kMessageHeight = 92;
// Below this the detail column drops the description and uses a smaller image.
constexpr int kRoomyDetail = 460;
const char* const kSite = "https://www.tone3000.com";

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

const char* const kNoSignIn = "Signing in from Pedaleira is not available yet. Trending and Latest work without an account.";

}  // namespace

// ####################
// SPLASH
// ####################
// TONE3000's splash: its wordmark, what it is in a line, and Continue. Shown
// over the whole page; the page behind loads nothing until it is continued.
class Tone3000Panel::Splash : public juce::Component {
public:
  Splash() {
    setTitle("TONE3000");
    addAndMakeVisible(go);
    go.onClick = [this] {
      if (onContinue)
        onContinue();
    };
  }

  std::function<void()> onContinue;

  juce::Rectangle<int> card() const {
    const int w = juce::jmin(520, getWidth() - 32);
    const int h = juce::jmin(220, getHeight() - 24);
    return getLocalBounds().withSizeKeepingCentre(w, h).withY(juce::jmax(12, (getHeight() - h) / 2 - 20));
  }

  void paint(juce::Graphics& g) override {
    g.fillAll(colour::surface);
    const auto box = card();
    paint::panel(g, box.toFloat(), colour::raised);
    auto inner = box.reduced(28, 30);
    // The wordmark with its clear space: nothing else within half its height.
    constexpr float markHeight = 28.0f;
    const auto markRow = inner.removeFromTop(juce::roundToInt(markHeight)).toFloat();
    tone3000::drawWordmark(g, markRow, juce::Justification::centred);
    inner.removeFromTop(juce::roundToInt(tone3000::wordmarkClearSpace(markHeight)) + 8);
    g.setFont(type::body());
    g.setColour(colour::text);
    g.drawFittedText("Captures and IRs from the TONE3000 community, ready to add to your rig.", inner.removeFromTop(40),
                     juce::Justification::centredTop, 2, 0.92f);
  }

  void resized() override {
    const auto box = card();
    const int w = juce::jmax(140, go.idealWidth() + 32);
    go.setBounds(box.getCentreX() - w / 2, box.getBottom() - 30 - 40, w, 40);
  }

private:
  PushButton go{"Continue", PushButton::Kind::primary};
};

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
  splash = std::make_unique<Splash>();
  splash->onContinue = [this] { continueSplash(); };
  addChildComponent(*splash);
}

Tone3000Panel::~Tone3000Panel() = default;

void Tone3000Panel::setFeed(Tone3000Feed* newFeed) {
  feed = newFeed;
  images.setFeed(newFeed);
  for (auto& t : tabs)
    t = {};
  chosenTone = chosenCapture = -1;
  capturesState = CapturesState::none;
  message = {};
  splash->setVisible(false);
  afterSplash = nullptr;
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
  // TONE3000's splash comes first, once per user.
  if (feed != nullptr && !feed->introSeen()) {
    showSplash([this] { show(); });
    return;
  }
  if (feed != nullptr)
    loadTab(tab, false);
  resized();
  repaint();
}

void Tone3000Panel::beginSignIn() {
  showSplash([this] {
    // The sign-in itself comes in a later build.
    message = kNoSignIn;
    resized();
    repaint();
  });
}

void Tone3000Panel::showSplash(std::function<void()> then) {
  afterSplash = std::move(then);
  splash->setVisible(true);
  splash->toFront(false);
  resized();
  repaint();
}

bool Tone3000Panel::splashShown() const { return splash->isVisible(); }

void Tone3000Panel::continueSplash() {
  if (!splash->isVisible())
    return;
  if (feed != nullptr)
    feed->markIntroSeen();
  splash->setVisible(false);
  auto then = std::move(afterSplash);
  afterSplash = nullptr;
  if (then)
    then();
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
  // Nothing is asked of TONE3000 before its splash was continued.
  if (feed != nullptr && !splashShown() && feed->introSeen())
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
    // The block keeps where it came from: the tone, its creator and licence.
    self->onAddFile(got.file, tone.title + " - " + capture.name, Tone3000Credit::fromTone(tone, capture.name).toAttribution(), finish);
  });
}

// ####################
// LAYOUT
// ####################
int Tone3000Panel::detailWidth() const { return juce::jlimit(272, 340, juce::roundToInt(getWidth() * 0.38f)); }

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
  area.removeFromRight(detailWidth() + 16);
  return area;
}

juce::Rectangle<int> Tone3000Panel::detailArea() const {
  auto area = getLocalBounds().withTrimmedTop(kHeaderHeight + kTabsHeight + 8);
  return area.removeFromRight(detailWidth());
}

bool Tone3000Panel::wideRows() const {
  return tableArea().getWidth() - 20 - kGearColumn - kFormatColumn - kCapturesColumn - 3 * 10 >= kLeastNameColumn;
}

int Tone3000Panel::rowHeight() const { return wideRows() ? kWideRow : kCompactRow; }

juce::Rectangle<int> Tone3000Panel::rowBounds(int index) const {
  const auto area = tableArea();
  return {area.getX(), area.getY() + kTableHead + index * (rowHeight() + 2) - scroll, area.getWidth(), rowHeight()};
}

Tone3000Panel::RowLayout Tone3000Panel::rowLayout(juce::Rectangle<int> row) const {
  RowLayout l;
  l.columns = wideRows();
  auto r = row.reduced(10, 0);
  if (l.columns) {
    l.captures = r.removeFromRight(kCapturesColumn);
    r.removeFromRight(10);
    l.format = r.removeFromRight(kFormatColumn);
    r.removeFromRight(10);
    l.gear = r.removeFromRight(kGearColumn);
    r.removeFromRight(10);
  }
  const int side = l.columns ? 44 : 50;
  l.art = r.removeFromLeft(side).withSizeKeepingCentre(side, side);
  r.removeFromLeft(12);
  if (l.columns) {
    l.title = {r.getX(), row.getY() + 9, r.getWidth(), 18};
    l.creatorLine = {r.getX(), row.getY() + 32, r.getWidth(), 18};
  } else {
    l.title = {r.getX(), row.getY() + 8, r.getWidth(), 18};
    l.meta = {r.getX(), row.getY() + 27, r.getWidth(), 16};
    l.creatorLine = {r.getX(), row.getY() + 46, r.getWidth(), 18};
  }
  return l;
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

Tone3000Panel::DetailLayout Tone3000Panel::detailLayout() const {
  DetailLayout l;
  const auto area = detailArea();
  const bool roomy = area.getHeight() >= kRoomyDetail;
  auto r = area.reduced(16, 14);
  const int side = roomy ? 72 : 52;
  auto top = r.removeFromTop(side);
  l.art = top.removeFromLeft(side);
  top.removeFromLeft(12);
  l.chips = top.removeFromBottom(22);
  l.title = top.withTrimmedBottom(4);
  r.removeFromTop(12);
  l.creatorLine = r.removeFromTop(20);
  r.removeFromTop(4);
  l.licence = r.removeFromTop(18);
  r.removeFromTop(12);
  // Above the buttons, and the line that says what went wrong with an add.
  l.capturesBottom = openTone.getY() - 8 - (message.isNotEmpty() || adding ? 40 : 0);
  if (roomy) {
    l.description = r.removeFromTop(52);
    r.removeFromTop(12);
  }
  l.captureLabel = r.removeFromTop(16);
  r.removeFromTop(8);
  l.capturesTop = r.getY();
  return l;
}

int Tone3000Panel::capturesThatFit() const {
  const auto l = detailLayout();
  return juce::jlimit(1, kMaxCaptureRows, (l.capturesBottom - l.capturesTop + 4) / (kCaptureRow + 4));
}

// The chosen capture is always among those shown.
int Tone3000Panel::firstShownCapture() const {
  const int total = static_cast<int>(captureList.captures.size());
  const int shown = juce::jmin(total, capturesThatFit());
  return juce::jlimit(0, juce::jmax(0, total - shown), chosenCapture - shown + 1);
}

juce::Rectangle<int> Tone3000Panel::captureBounds(int index) const {
  const auto l = detailLayout();
  return {l.captureLabel.getX(), l.capturesTop + index * (kCaptureRow + 4), l.captureLabel.getWidth(), kCaptureRow};
}

tone3000::CreatorLine Tone3000Panel::rowCredit(int index) const {
  if (index < 0 || index >= toneCount() || splashShown())
    return {};
  const auto row = rowBounds(index);
  if (!row.intersects(tableArea().withTrimmedTop(kTableHead)))
    return {};
  const auto credit = Tone3000Credit::fromTone(state().result.tones[static_cast<size_t>(index)]);
  return tone3000::creatorLine(rowLayout(row).creatorLine, credit, true);
}

tone3000::CreatorLine Tone3000Panel::detailCredit() const {
  if (chosenTone < 0 || chosenTone >= toneCount() || splashShown())
    return {};
  const auto credit = Tone3000Credit::fromTone(state().result.tones[static_cast<size_t>(chosenTone)]);
  return tone3000::creatorLine(detailLayout().creatorLine, credit, false);
}

juce::Rectangle<int> Tone3000Panel::detailLicence() const {
  if (chosenTone < 0 || chosenTone >= toneCount() || splashShown())
    return {};
  return detailLayout().licence;
}

void Tone3000Panel::resized() {
  splash->setBounds(getLocalBounds());
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
  const juce::String needs = juce::String(tone3000ListName(tab)) + " needs a TONE3000 account. " + kNoSignIn;
  if (tone3000ListNeedsAccount(tab) && !signedIn() && !(s.loaded && s.result.ok))
    return needs;
  if (s.loading || !s.loaded)
    return juce::String::fromUTF8("Loading from TONE3000\xe2\x80\xa6");
  if (!s.result.ok)
    return s.result.needsAccount ? needs : reachText(s.result.error);
  return "Nothing here yet.";
}

void Tone3000Panel::paintHeader(juce::Graphics& g) {
  // The attribution TONE3000 asks for: its own wordmark, then who is signed in.
  const auto header = headerArea();
  tone3000::drawPoweredBy(g, header.withWidth(tone3000::poweredByWidth(header.getHeight())));
  const bool in = signedIn();
  const juce::String who = !in ? juce::String("Not signed in to TONE3000")
                               : feed->userName().isNotEmpty() ? "Signed in as " + feed->userName() : juce::String("Signed in to TONE3000");
  const int x = header.getX() + tone3000::poweredByWidth(header.getHeight()) + 24;
  const auto dotArea = juce::Rectangle<float>(static_cast<float>(x), header.getCentreY() - 4.0f, 8.0f, 8.0f);
  if (in) {
    g.setColour(colour::ok);
    g.fillEllipse(dotArea);
  } else {
    g.setColour(colour::muted);
    g.drawEllipse(dotArea.reduced(1.0f), 2.0f);
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(who, x + 16, header.getY(), juce::jmax(0, browse.getX() - 12 - (x + 16)), header.getHeight(), juce::Justification::centredLeft,
             true);
}

void Tone3000Panel::paint(juce::Graphics& g) {
  paintHeader(g);

  // The tabs.
  const bool in = signedIn();
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
  // What the tab lists, where there is room after the tabs.
  {
    const int right = getWidth() - detailWidth() - 16;
    const int left = tabBounds(kTabCount - 1).getRight() + 16;
    if (right - left >= 120) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      const juce::String note = tab == Tone3000List::trending ? "Top tones this week" : tab == Tone3000List::latest ? "The newest tones" : "Your own tones";
      g.drawText(note, left, kHeaderHeight, right - left, kTabsHeight, juce::Justification::centredRight, true);
    }
  }

  // The table.
  const auto table = tableArea();
  {
    const auto l = rowLayout(table.withHeight(kTableHead));
    paint::label(g, "Tone, creator and licence", table.withHeight(kTableHead).reduced(10, 0).withRight(l.columns ? l.gear.getX() - 10 : table.getRight()));
    if (l.columns) {
      paint::label(g, "Gear", l.gear.withY(table.getY()).withHeight(kTableHead));
      paint::label(g, "Format", l.format.withY(table.getY()).withHeight(kTableHead));
      paint::label(g, "Captures", l.captures.withY(table.getY()).withHeight(kTableHead));
    }
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
  const auto l = rowLayout(row);
  const Tone3000Credit credit = Tone3000Credit::fromTone(tone);
  tone3000::drawArt(g, l.art.toFloat(), images.get(tone.imageUrl, false), tone3000::categoryOf(tone.gear, tone.format), 6.0f);
  g.setFont(type::sans(13.0f, type::Weight::semibold));
  g.setColour(colour::text);
  g.drawText(tone.title, l.title, juce::Justification::centredLeft, true);
  tone3000::drawCreatorLine(g, l.creatorLine, credit, images.get(tone.avatarUrl, true));
  if (l.columns) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::text);
    g.drawText(tone3000::gearLabel(tone.gear), l.gear, juce::Justification::centredLeft, true);
    g.setFont(type::mono(12.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(tone3000::formatLabel(tone.format), l.format, juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.drawText(capturesText(tone), l.captures, juce::Justification::centredLeft, true);
  } else {
    // Narrow: the gear, format and captures under the title.
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(tone3000::gearLabel(tone.gear) + "  " + type::dot() + "  " + tone3000::formatLabel(tone.format) + "  " + type::dot() + "  " +
                   capturesText(tone),
               l.meta, juce::Justification::centredLeft, true);
  }
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
    if (message.isNotEmpty()) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::warn);
      g.drawFittedText(message, area.reduced(16, 0).withTop(box.getBottom() + 12).withHeight(54), juce::Justification::topLeft, 3, 1.0f);
    }
    return;
  }
  const Tone3000Tone& tone = state().result.tones[static_cast<size_t>(chosenTone)];
  const Tone3000Credit credit = Tone3000Credit::fromTone(tone);
  const auto l = detailLayout();
  tone3000::drawArt(g, l.art.toFloat(), images.get(tone.imageUrl, false), tone3000::categoryOf(tone.gear, tone.format), 8.0f);
  g.setFont(type::sans(l.art.getHeight() >= 72 ? 18.0f : 16.0f, type::Weight::semibold));
  g.setColour(colour::text);
  g.drawFittedText(tone.title, l.title, juce::Justification::topLeft, l.art.getHeight() >= 72 ? 2 : 1, 1.0f);
  {
    auto chips = l.chips;
    for (const auto& text : {tone3000::gearLabel(tone.gear), tone3000::formatLabel(tone.format)}) {
      const int w = juce::jmin(paint::chipWidth(text), chips.getWidth());
      paint::chip(g, text, chips.removeFromLeft(w).toFloat(), paint::ChipStyle::outline);
      chips.removeFromLeft(8);
    }
  }
  // The creator with their avatar (and TONE3000's badge when verified), and
  // the licence on its own line: never given up for room.
  tone3000::drawCreatorLine(g, l.creatorLine, credit, images.get(tone.avatarUrl, true), false);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(tone3000::licenceText(tone.licence), l.licence, juce::Justification::centredLeft, true);
  if (!l.description.isEmpty()) {
    g.setColour(colour::text);
    g.drawFittedText(tone.description.isNotEmpty() ? tone.description : juce::String("No description."), l.description,
                     juce::Justification::topLeft, 3, 1.0f);
  }
  paint::label(g, tone.format.equalsIgnoreCase("ir") ? "IRs in this tone" : "Captures in this tone", l.captureLabel);

  if (capturesState == CapturesState::ready) {
    const int shown = juce::jmin(static_cast<int>(captureList.captures.size()), capturesThatFit());
    const int first = firstShownCapture();
    for (int i = first; i < first + shown; ++i) {
      const auto r = captureBounds(i - first);
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
    // More than there is room for: how many are shown, beside the label.
    if (static_cast<int>(captureList.captures.size()) > shown) {
      g.setFont(type::sans(12.0f));
      g.setColour(colour::muted);
      g.drawText(juce::String(shown) + " of " + juce::String(static_cast<int>(captureList.captures.size())) + " shown", l.captureLabel,
                 juce::Justification::centredRight, false);
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
    g.drawFittedText(text, juce::Rectangle<int>(l.captureLabel.getX(), l.capturesTop, l.captureLabel.getWidth(),
                                                juce::jmax(36, l.capturesBottom - l.capturesTop)),
                     juce::Justification::topLeft, 5, 1.0f);
  }

  // What went wrong with the last Add, over the buttons.
  if (message.isNotEmpty()) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::warn);
    g.drawFittedText(message, juce::Rectangle<int>(area.getX() + 16, openTone.getY() - 44, area.getWidth() - 32, 36),
                     juce::Justification::bottomLeft, 2, 1.0f);
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
  // The wordmark links to tone3000.com, as it does in TONE3000's own plugin.
  if (headerArea().withWidth(tone3000::poweredByWidth(kHeaderHeight)).contains(at)) {
    juce::URL(kSite).launchInDefaultBrowser();
    return;
  }
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
    for (int i = 0; i < juce::jmin(static_cast<int>(captureList.captures.size()), capturesThatFit()); ++i)
      if (captureBounds(i).contains(at)) {
        selectCapture(firstShownCapture() + i);
        return;
      }
}

void Tone3000Panel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
  const int content = toneCount() * (rowHeight() + 2);
  const int maxScroll = juce::jmax(0, content + kTableHead - tableArea().getHeight());
  scroll = juce::jlimit(0, maxScroll, scroll - juce::roundToInt(wheel.deltaY * 200.0f));
  repaint(tableArea());
}

}  // namespace pedaleira::ui
