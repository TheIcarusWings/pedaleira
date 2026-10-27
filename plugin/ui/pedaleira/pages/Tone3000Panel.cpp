#include "Tone3000Panel.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <algorithm>
#include <cmath>

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
// Below this the detail column uses a smaller image and a shorter description.
constexpr int kRoomyDetail = 460;
// The description has its own room in the detail column, between this much and
// that much high (a longer text scrolls in it); it is left out when the
// captures would be left with fewer than two rows.
constexpr int kLeastDescription = 36;
constexpr int kMostDescription = 56;
constexpr int kMostRoomyDescription = 88;
constexpr int kDescriptionBar = 8;
constexpr int kSignInCardHeight = 200;
const char* const kSignInHint = "Use Sign in at the top of this page.";
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

// A description laid out in `width`, wrapped by word.
juce::TextLayout describe(const juce::String& text, int width) {
  juce::AttributedString string;
  string.append(text, type::sans(12.0f), colour::text);
  string.setWordWrap(juce::AttributedString::byWord);
  string.setJustification(juce::Justification::topLeft);
  juce::TextLayout layout;
  layout.createLayout(string, static_cast<float>(juce::jmax(1, width)));
  return layout;
}

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
  signInOut.onClick = [this] {
    if (signedIn())
      signOut();
    else
      beginSignIn();
  };
  copyLink.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(signInLink()); };
  closeSignIn.onClick = [this] { closeSignInCard(); };
  trySignIn.onClick = [this] { startSignIn(); };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&browse, &retry, &openTone, &add, &signInOut, &copyLink, &closeSignIn, &trySignIn})
    addAndMakeVisible(c);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&retry, &openTone, &add, &signInOut, &copyLink, &closeSignIn, &trySignIn})
    c->setVisible(false);
  splash = std::make_unique<Splash>();
  splash->onContinue = [this] { continueSplash(); };
  addChildComponent(*splash);
}

Tone3000Panel::~Tone3000Panel() = default;

void Tone3000Panel::setFeed(Tone3000Feed* newFeed) {
  feed = newFeed;
  images.setFeed(newFeed);
  ++signInGeneration;
  signInOpen = false;
  signInProblem = {};
  resetTabs();
  message = {};
  splash->setVisible(false);
  afterSplash = nullptr;
  wasSignedIn = signedIn();
  if (feed != nullptr) {
    // The feed tells when the sign-in moved or the session came or went.
    juce::Component::SafePointer<Tone3000Panel> self(this);
    feed->onAccountChanged = [self] {
      if (self != nullptr)
        self->accountChanged();
    };
  }
  resized();
}

void Tone3000Panel::resetTabs() {
  ++listGeneration;
  ++captureGeneration;
  for (auto& t : tabs)
    t = {};
  chosenTone = chosenCapture = -1;
  captureList = {};
  capturesState = CapturesState::none;
  adding = false;
  descScroll = 0;
  scroll = 0;
}

void Tone3000Panel::accountChanged() {
  const bool in = signedIn();
  if (in != wasSignedIn) {
    wasSignedIn = in;
    resetTabs();
    message = {};
    // Nothing is asked of TONE3000 before its splash was continued.
    if (feed != nullptr && !splashShown() && feed->introSeen())
      loadTab(tab, false);
  }
  resized();
  repaint();
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
  if (feed != nullptr) {
    // Another build may have signed in or out since the page was last shown.
    if (signedIn() != wasSignedIn) {
      wasSignedIn = signedIn();
      resetTabs();
    }
    loadTab(tab, false);
  }
  resized();
  repaint();
}

void Tone3000Panel::beginSignIn() {
  if (feed == nullptr || signedIn())
    return;
  showSplash([this] { startSignIn(); });
}

void Tone3000Panel::startSignIn() {
  if (feed == nullptr || signedIn())
    return;
  signInOpen = true;
  signInProblem = {};
  message = {};
  const int generation = ++signInGeneration;
  resized();
  repaint();
  juce::Component::SafePointer<Tone3000Panel> self(this);
  feed->signIn([self, generation](bool ok, juce::String text) {
    if (self == nullptr || generation != self->signInGeneration)
      return;
    if (ok) {
      self->signInOpen = false;
      self->signInProblem = {};
      self->accountChanged();
    } else if (text.isNotEmpty()) {
      self->signInProblem = text;
    } else {
      // The player gave up waiting.
      self->signInOpen = false;
    }
    self->resized();
    self->repaint();
  });
}

void Tone3000Panel::closeSignInCard() {
  // Counted first: the feed answers a cancelled sign-in, and nobody listens.
  ++signInGeneration;
  signInOpen = false;
  signInProblem = {};
  if (feed != nullptr)
    feed->cancelSignIn();
  resized();
  repaint();
}

void Tone3000Panel::signOut() {
  if (feed == nullptr)
    return;
  feed->signOut();
  accountChanged();
}

bool Tone3000Panel::signInWaiting() const {
  return signInOpen && signInProblem.isEmpty() && feed != nullptr && feed->signInState().phase == Tone3000Feed::SignInState::Phase::waiting;
}

juce::String Tone3000Panel::signInLink() const {
  return signInOpen && feed != nullptr ? feed->signInState().authorizeUrl : juce::String();
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
  descScroll = 0;
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
  const int generation = listGeneration;
  feed->list(which, [self, which, generation](Tone3000Feed::Listed listed) {
    if (self == nullptr || generation != self->listGeneration)
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
  descScroll = 0;
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
  if (signInOpen || !area.contains(position))
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
  auto r = area.reduced(16, 0).withTrimmedTop(14).withTrimmedBottom(16);
  // From the bottom: the two buttons, and above them the line that says what
  // went wrong with an add (or that one is on its way).
  l.add = r.removeFromBottom(40);
  r.removeFromBottom(8);
  l.open = r.removeFromBottom(36);
  r.removeFromBottom(8);
  if (message.isNotEmpty() || adding) {
    l.note = r.removeFromBottom(36);
    r.removeFromBottom(8);
  }
  // From the top: the image, title and chips, the creator and the licence.
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
  // What is left is the description's, when the captures keep two rows, and
  // the captures'. A description that is longer than its room scrolls in it.
  const int capturesNeed = 16 + 8 + 2 * kCaptureRow + 4;
  const int spare = r.getHeight() - capturesNeed - 12;
  if (spare >= kLeastDescription) {
    l.description = r.removeFromTop(juce::jmin(spare, roomy ? kMostRoomyDescription : kMostDescription));
    r.removeFromTop(12);
  }
  l.captureLabel = r.removeFromTop(16);
  r.removeFromTop(8);
  l.captureList = r;
  l.capturesTop = r.getY();
  l.capturesBottom = r.getBottom();
  return l;
}

juce::Rectangle<int> Tone3000Panel::signInCard() const {
  const auto area = tableArea().withTrimmedTop(kTableHead);
  const int width = juce::jmax(0, juce::jmin(560, area.getWidth() - 40));
  return {area.getCentreX() - width / 2, area.getY() + 30, width, kSignInCardHeight};
}

juce::Rectangle<int> Tone3000Panel::descriptionRoom() const {
  if (chosenTone < 0 || chosenTone >= toneCount() || splashShown())
    return {};
  return detailLayout().description;
}

int Tone3000Panel::descriptionHeight(int width) const {
  if (chosenTone < 0 || chosenTone >= toneCount())
    return 0;
  const auto& text = state().result.tones[static_cast<size_t>(chosenTone)].description;
  return juce::roundToInt(std::ceil(describe(text.isNotEmpty() ? text : juce::String("No description."), width).getHeight()));
}

bool Tone3000Panel::descriptionScrolls() const {
  const auto room = descriptionRoom();
  return !room.isEmpty() && descriptionHeight(room.getWidth() - kDescriptionBar) > room.getHeight();
}

void Tone3000Panel::scrollDescription(int by) {
  const auto room = descriptionRoom();
  if (room.isEmpty())
    return;
  const int most = juce::jmax(0, descriptionHeight(room.getWidth() - kDescriptionBar) - room.getHeight());
  descScroll = juce::jlimit(0, most, descScroll + by);
  repaint(room);
}

std::vector<std::pair<juce::String, juce::Rectangle<int>>> Tone3000Panel::detailElements() const {
  std::vector<std::pair<juce::String, juce::Rectangle<int>>> out;
  if (chosenTone < 0 || chosenTone >= toneCount() || splashShown())
    return out;
  const auto l = detailLayout();
  const auto put = [&out](const char* name, juce::Rectangle<int> bounds) {
    if (!bounds.isEmpty())
      out.emplace_back(name, bounds);
  };
  put("image", l.art);
  put("title", l.title);
  put("chips", l.chips);
  put("creator", l.creatorLine);
  put("licence", l.licence);
  put("description", l.description);
  put("captures heading", l.captureLabel);
  put("captures", l.captureList);
  put("note", l.note);
  // The buttons as they are placed, not as they were meant to be.
  put("open", openTone.getBounds());
  put("add", add.getBounds());
  return out;
}

int Tone3000Panel::capturesThatFit() const {
  const auto l = detailLayout();
  return juce::jlimit(0, kMaxCaptureRows, (l.capturesBottom - l.capturesTop + 4) / (kCaptureRow + 4));
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
  if (index < 0 || index >= toneCount() || splashShown() || signInOpen)
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
  const bool haveTone = chosenTone >= 0 && chosenTone < toneCount();
  const auto l = detailLayout();
  add.setBounds(l.add);
  openTone.setBounds(l.open);
  add.setVisible(haveTone);
  openTone.setVisible(haveTone);
  add.setEnabled(canAdd());
  const auto table = tableArea();
  retry.setBounds(table.getCentreX() - 50, table.getY() + kTableHead + 30 + kMessageHeight + 12, 100, 32);
  retry.setVisible(feed != nullptr && !signInOpen && state().loaded && !state().result.ok && !state().result.needsAccount);

  // Sign in (or out) beside the link to TONE3000 in the header.
  signInOut.setButtonText(signedIn() ? "Sign out" : "Sign in");
  const int inOutWidth = signInOut.idealWidth() + 8;
  signInOut.setBounds(browse.getX() - 8 - inOutWidth, header.getY() + 6, inOutWidth, 32);
  signInOut.setVisible(feed != nullptr);
  signInOut.setEnabled(!signInOpen);

  // The sign-in card's buttons, along its bottom edge.
  const auto card = signInCard();
  const bool failed = signInFailed();
  closeSignIn.setButtonText(failed ? "Close" : "Cancel");
  int x = card.getX() + 20;
  const int y = card.getBottom() - 16 - 32;
  const auto place = [&x, y](PushButton& button) {
    const int w = button.idealWidth() + 8;
    button.setBounds(x, y, w, 32);
    x += w + 8;
  };
  const bool linked = signInWaiting() && signInLink().isNotEmpty();
  if (failed)
    place(trySignIn);
  else if (linked)
    place(copyLink);
  place(closeSignIn);
  copyLink.setVisible(linked);
  trySignIn.setVisible(failed);
  closeSignIn.setVisible(signInOpen);
}

// ####################
// PAINT
// ####################
juce::String Tone3000Panel::emptyText() const {
  if (feed == nullptr)
    return "Browsing TONE3000 is not available in this build.";
  const TabState& s = state();
  const juce::String needs = juce::String(tone3000ListName(tab)) + " needs a TONE3000 account. " + kSignInHint;
  if (tone3000ListNeedsAccount(tab) && !signedIn() && !(s.loaded && s.result.ok))
    return needs;
  if (s.loading || !s.loaded)
    return juce::String::fromUTF8("Loading from TONE3000\xe2\x80\xa6");
  if (!s.result.ok)
    return s.result.needsAccount ? needs : reachText(s.result.error);
  return "Nothing here yet.";
}

juce::String Tone3000Panel::accountText() const {
  if (!signedIn())
    return "Not signed in to TONE3000";
  return feed->userName().isNotEmpty() ? "Signed in as " + feed->userName() : juce::String("Signed in to TONE3000");
}

void Tone3000Panel::paintHeader(juce::Graphics& g) {
  // The attribution TONE3000 asks for: its own wordmark, then who is signed in.
  const auto header = headerArea();
  tone3000::drawPoweredBy(g, header.withWidth(tone3000::poweredByWidth(header.getHeight())));
  const bool in = signedIn();
  const juce::String who = accountText();
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
  const int whoRight = signInOut.isVisible() ? signInOut.getX() : browse.getX();
  g.drawText(who, x + 16, header.getY(), juce::jmax(0, whoRight - 12 - (x + 16)), header.getHeight(), juce::Justification::centredLeft, true);
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
    if (signInOpen)
      paintSignIn(g, table.withTrimmedTop(kTableHead));
    else if (feed != nullptr && s.loaded && s.result.ok && !s.result.tones.empty())
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

void Tone3000Panel::paintSignIn(juce::Graphics& g, juce::Rectangle<int>) {
  const auto box = signInCard();
  g.setColour(colour::raised);
  g.fillRoundedRectangle(box.toFloat(), size::buttonRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
  const auto state = feed != nullptr ? feed->signInState() : Tone3000Feed::SignInState();
  const bool failed = signInFailed();
  const bool finishing = !failed && state.phase == Tone3000Feed::SignInState::Phase::finishing;
  auto inner = box.reduced(20, 16);
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText(failed ? "Signing in did not finish" : finishing ? "Finishing sign-in" : "Signing in to TONE3000", inner.removeFromTop(22),
             juce::Justification::centredLeft, true);
  inner.removeFromTop(6);
  g.setFont(type::body());
  auto body = inner.removeFromTop(48);
  if (failed) {
    g.setColour(colour::warn);
    g.drawFittedText(signInProblem, body, juce::Justification::topLeft, 3, 1.0f);
    return;
  }
  g.setColour(colour::text);
  if (finishing) {
    g.drawFittedText(juce::String::fromUTF8("Back from TONE3000. Finishing the sign-in\xe2\x80\xa6"), body, juce::Justification::topLeft, 3, 1.0f);
    return;
  }
  const juce::String ask = state.browserProblem.isNotEmpty()
                               ? state.browserProblem + " Copy this link into a browser instead."
                               : juce::String("Finish signing in in your browser, then come back here. If no browser opened, copy this link into one.");
  g.drawFittedText(ask, body, juce::Justification::topLeft, 3, 1.0f);
  inner.removeFromTop(8);
  if (state.authorizeUrl.isNotEmpty()) {
    const auto field = inner.removeFromTop(28);
    g.setColour(colour::ground);
    g.fillRoundedRectangle(field.toFloat(), size::buttonRadius);
    g.setColour(colour::line);
    g.drawRoundedRectangle(field.toFloat().reduced(0.5f), size::buttonRadius, 1.0f);
    g.setFont(type::mono(11.0f, type::Weight::regular));
    g.setColour(colour::muted);
    g.drawText(state.authorizeUrl, field.reduced(10, 0), juce::Justification::centredLeft, true);
  }
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
    // In its own room: a longer text is clipped to it and scrolls.
    const int textWidth = l.description.getWidth() - kDescriptionBar;
    const auto layout = describe(tone.description.isNotEmpty() ? tone.description : juce::String("No description."), textWidth);
    const int content = juce::roundToInt(std::ceil(layout.getHeight()));
    const int most = juce::jmax(0, content - l.description.getHeight());
    const int offset = juce::jmin(descScroll, most);
    {
      juce::Graphics::ScopedSaveState saved(g);
      g.reduceClipRegion(l.description);
      layout.draw(g, juce::Rectangle<float>(static_cast<float>(l.description.getX()), static_cast<float>(l.description.getY() - offset),
                                            static_cast<float>(textWidth), static_cast<float>(juce::jmax(content, l.description.getHeight()))));
    }
    if (most > 0) {
      const int barHeight = juce::jmax(16, l.description.getHeight() * l.description.getHeight() / content);
      const int barY = l.description.getY() + (l.description.getHeight() - barHeight) * offset / most;
      g.setColour(colour::faint);
      g.fillRoundedRectangle(static_cast<float>(l.description.getRight() - 4), static_cast<float>(barY), 3.0f, static_cast<float>(barHeight), 1.5f);
    }
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
                                  ? juce::String("Adding a capture needs a TONE3000 account. ") + kSignInHint
                                  : "The captures could not be loaded. " + reachText(captureList.error);
    g.setFont(type::sans(12.0f));
    g.setColour(capturesState == CapturesState::failed && !captureList.needsAccount ? colour::warn : colour::muted);
    // Within the captures' room, whatever its height.
    g.drawFittedText(text, l.captureList, juce::Justification::topLeft, juce::jmax(1, l.captureList.getHeight() / 15), 1.0f);
  }

  // What went wrong with the last Add, over the buttons.
  if (message.isNotEmpty()) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::warn);
    g.drawFittedText(message, l.note, juce::Justification::bottomLeft, 2, 1.0f);
  } else if (adding) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(juce::String::fromUTF8("Downloading\xe2\x80\xa6"), l.note, juce::Justification::bottomLeft, false);
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

void Tone3000Panel::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
  // Over a long description, the wheel moves the description.
  if (descriptionRoom().contains(event.getPosition())) {
    scrollDescription(-juce::roundToInt(wheel.deltaY * 120.0f));
    return;
  }
  const int content = toneCount() * (rowHeight() + 2);
  const int maxScroll = juce::jmax(0, content + kTableHead - tableArea().getHeight());
  scroll = juce::jlimit(0, maxScroll, scroll - juce::roundToInt(wheel.deltaY * 200.0f));
  repaint(tableArea());
}

}  // namespace pedaleira::ui
