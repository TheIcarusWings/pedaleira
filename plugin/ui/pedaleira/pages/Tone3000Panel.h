// The TONE3000 source of the block library (the board "Picker, TONE3000"): the
// free tier's lists as tabs (Trending, Latest, Favorites, Downloaded,
// Created), a table of tones and on the right the chosen tone with its
// captures and the button that adds one to the slot. Signed out, Trending and
// Latest still work; the tabs of the player's own tones say what they need,
// and the header has Sign in (Sign out once signed in). Signing in goes to
// TONE3000 in the system browser; while the page waits it shows the link to
// copy into any browser.
// Search is on tone3000.com (the free tier has none), one button away.
//
// What TONE3000's API terms ask is kept at every width: each tone shows its
// image, title, gear, format, creator (username and avatar) and licence (the
// creator and the licence never give way to nothing), the page is "Powered by"
// TONE3000's wordmark, and a splash with the wordmark comes first: the first
// time the page is opened for this user, and before a sign-in.
//
// The data comes from a Tone3000Feed the owner gives it; without one the page
// says browsing is not available. Nothing is downloaded until the player
// presses Add.
#pragma once

#include "../Tokens.h"
#include "../widgets/Controls.h"
#include "Tone3000Credit.h"
#include "Tone3000Feed.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace pedaleira::ui {

class Tone3000Panel : public juce::Component {
public:
  Tone3000Panel();
  ~Tone3000Panel() override;

  void setFeed(Tone3000Feed* newFeed);
  // The slot the capture will be added to (the button says which).
  void setTarget(int row, int slot);
  // The panel was chosen: the splash the first time, else reads the account
  // again and loads the shown tab.
  void show();

  // Where a sign-in starts: TONE3000's splash comes first, then the browser.
  void beginSignIn();
  // Forgets the session on this computer; the tabs of the player's own tones
  // go back to asking for an account.
  void signOut();

  // A capture was downloaded to this file, named by tone and capture; the owner
  // adds it to the slot with `attribution` (chain.addToneFromFiles' tone3000),
  // so the block keeps its creator and licence. The panel waits for `finished`
  // before it lets Add be pressed again.
  std::function<void(const juce::File& file, const juce::String& name, const juce::var& attribution,
                     std::function<void(const juce::String& error)> finished)>
      onAddFile;

  // Testbed and tests: what the mouse does.
  void selectTab(Tone3000List which);
  void selectTone(int index);
  void selectCapture(int index);
  void addSelected();
  Tone3000List currentTab() const { return tab; }
  int toneCount() const;
  int selectedTone() const { return chosenTone; }
  int selectedCapture() const { return chosenCapture; }
  bool canAdd() const;
  // The sentence the page shows when a list or a capture could not be had.
  const juce::String& problem() const { return message; }
  // The splash is up, and its Continue.
  bool splashShown() const;
  void continueSplash();
  // Where a listed tone's creator and licence are drawn, in the panel's
  // coordinates (empty when the row is not shown), and the same for the
  // chosen tone's detail.
  tone3000::CreatorLine rowCredit(int index) const;
  tone3000::CreatorLine detailCredit() const;
  juce::Rectangle<int> detailLicence() const;
  juce::Rectangle<int> tableBounds() const { return tableArea(); }
  // The sign-in card is up (waiting, finishing or failed), and what it says.
  bool signInShown() const { return signInOpen; }
  bool signInWaiting() const;
  bool signInFailed() const { return signInOpen && signInProblem.isNotEmpty(); }
  const juce::String& signInError() const { return signInProblem; }
  juce::String signInLink() const;
  // What the header says of the account, and its button (Sign out when signed in).
  juce::String accountText() const;
  juce::String signInButtonText() const { return signInOut.getButtonText(); }
  void pressSignInButton() { signInOut.onClick(); }
  void pressCancelSignIn() { closeSignIn.onClick(); }
  void pressTrySignIn() { trySignIn.onClick(); }
  // Every part of the detail column and where it is drawn, in the panel's
  // coordinates (empty ones left out). None of them overlap.
  std::vector<std::pair<juce::String, juce::Rectangle<int>>> detailElements() const;
  juce::Rectangle<int> detailBounds() const { return detailArea(); }
  // The description is longer than its room and scrolls within it.
  bool descriptionScrolls() const;
  int descriptionScrollPosition() const { return descScroll; }
  void scrollDescription(int by);

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseUp(const juce::MouseEvent& event) override;
  void mouseMove(const juce::MouseEvent& event) override;
  void mouseExit(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
  struct TabState {
    bool loaded = false;
    bool loading = false;
    Tone3000Feed::Listed result;
  };
  enum class CapturesState { none, loading, ready, failed };
  class Splash;

  // Where a row puts things.
  struct RowLayout {
    juce::Rectangle<int> art, title, meta, creatorLine, gear, format, captures;
    bool columns = false;
  };
  // Where the detail column puts things, from its top.
  // Every rectangle is inside the column and apart from the others: the top
  // (image, title, chips, creator, licence), the description in its own room,
  // the captures, then what went wrong with an Add and the two buttons.
  struct DetailLayout {
    juce::Rectangle<int> art, title, chips, creatorLine, licence, description, captureLabel, captureList, note, open, add;
    int capturesTop = 0;
    int capturesBottom = 0;
  };

  static constexpr int kTabCount = 5;
  static Tone3000List tabAt(int index) { return static_cast<Tone3000List>(index); }

  TabState& state() { return tabs[static_cast<size_t>(tab)]; }
  const TabState& state() const { return tabs[static_cast<size_t>(tab)]; }
  void loadTab(Tone3000List which, bool force);
  void loadCaptures();
  bool signedIn() const { return feed != nullptr && feed->signedIn(); }
  void showSplash(std::function<void()> then);
  void startSignIn();
  void closeSignInCard();
  // The session came or went (here, in another build, or it expired): the
  // tabs are read again and the header says who.
  void accountChanged();
  void resetTabs();
  juce::Rectangle<int> signInCard() const;
  void paintSignIn(juce::Graphics& g, juce::Rectangle<int> area);
  juce::Rectangle<int> descriptionRoom() const;
  int descriptionHeight(int width) const;

  int detailWidth() const;
  bool wideRows() const;
  int rowHeight() const;
  juce::Rectangle<int> headerArea() const;
  juce::Rectangle<int> tabBounds(int index) const;
  juce::Rectangle<int> tableArea() const;
  juce::Rectangle<int> detailArea() const;
  juce::Rectangle<int> rowBounds(int index) const;
  RowLayout rowLayout(juce::Rectangle<int> row) const;
  DetailLayout detailLayout() const;
  juce::Rectangle<int> captureBounds(int index) const;
  int capturesThatFit() const;
  int firstShownCapture() const;
  int rowAt(juce::Point<int> position) const;
  void paintHeader(juce::Graphics& g);
  void paintRow(juce::Graphics& g, const Tone3000Tone& tone, juce::Rectangle<int> row, bool selected, bool hovered);
  void paintDetail(juce::Graphics& g, juce::Rectangle<int> area);
  void paintMessage(juce::Graphics& g, juce::Rectangle<int> area);
  juce::String emptyText() const;

  Tone3000Feed* feed = nullptr;
  int targetRow = 0, targetSlot = 0;
  Tone3000List tab = Tone3000List::trending;
  std::array<TabState, kTabCount> tabs;
  int chosenTone = -1;
  int chosenCapture = -1;
  int hoverRow = -1;
  int scroll = 0;
  int descScroll = 0;
  // The sign-in card: shown from the moment a sign-in starts until it ends in
  // success, or the player closes it. `signInProblem` is set when it failed.
  bool signInOpen = false;
  juce::String signInProblem;
  bool wasSignedIn = false;
  // A sign-in answered after the page moved on (or was started again) is dropped.
  int signInGeneration = 0;
  // A list that arrives after the account changed is dropped.
  int listGeneration = 0;
  CapturesState capturesState = CapturesState::none;
  Tone3000Feed::Captures captureList;
  bool adding = false;
  juce::String message;
  // A capture list that arrives after the player chose another tone is dropped.
  int captureGeneration = 0;
  Tone3000Images images{*this};

  PushButton browse{juce::String::fromUTF8("Open TONE3000 in the browser")};
  PushButton retry{"Try again"};
  PushButton openTone{"Open on TONE3000"};
  PushButton add{"Add", PushButton::Kind::primary};
  PushButton signInOut{"Sign in"};
  PushButton copyLink{"Copy link"};
  PushButton closeSignIn{"Cancel"};
  PushButton trySignIn{"Try again", PushButton::Kind::primary};
  std::unique_ptr<Splash> splash;
  // What Continue on the splash leads to.
  std::function<void()> afterSplash;
};

}  // namespace pedaleira::ui
