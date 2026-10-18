// What TONE3000's API terms ask of every place a TONE3000 tone is shown: its
// image, title, gear, format and creator (username and avatar), and the
// licence, never hidden; and TONE3000's own wordmark where the app says where
// the tones come from. The pages that show a tone (the block library, its
// TONE3000 page, a capture's editor) draw it with these, so the rules live in
// one place.
//
// A credit is read from what the engine keeps with a block or a loaded file
// (`tone3000`, see chain.addToneFromFiles) or from a tone of the feed. A field
// TONE3000 did not give is said to be missing, never made up.
#pragma once

#include "../Tokens.h"
#include "Tone3000Feed.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <set>

namespace pedaleira::ui {

struct Tone3000Credit {
  int id = 0;
  juce::String title, url, gear, format, licence, imageUrl, capture;
  juce::String username, creatorName, avatarUrl;
  bool verified = false;

  // Something to credit: a title or a creator.
  bool known() const { return title.isNotEmpty() || username.isNotEmpty(); }

  // From the engine's `tone3000` object (a block's tone, nam.recent,
  // assets.list).
  static Tone3000Credit fromAttribution(const juce::var& attribution);
  // From a catalog tone's summary (a block loaded from TONE3000 itself).
  static Tone3000Credit fromSummary(const juce::var& tone);
  static Tone3000Credit fromTone(const Tone3000Tone& tone, const juce::String& captureName = {});
  // The `tone3000` argument of chain.addToneFromFiles.
  juce::var toAttribution() const;
};

namespace tone3000 {

// The wordmark (assets/t3k.svg, 210 x 32) drawn as supplied, scaled to `box`'s
// height and placed by `placement` inside it.
void drawWordmark(juce::Graphics& g, juce::Rectangle<float> box,
                  juce::Justification placement = juce::Justification::centredLeft);
float wordmarkWidth(float height);
// The clear space kept free around the wordmark: half its height.
float wordmarkClearSpace(float height);
// "Powered by" and the wordmark, left aligned in `area`; returns where the
// wordmark went.
juce::Rectangle<float> drawPoweredBy(juce::Graphics& g, juce::Rectangle<int> area);
// The width drawPoweredBy needs at this height.
int poweredByWidth(int height);
// Testbed: how many times the wordmark has been drawn.
int wordmarkDraws();

// A licence id as TONE3000 sends it ("cc0", "cc-by-nc") as a reader knows it
// ("CC0", "CC BY-NC"); anything else as it came, tidied.
juce::String licenceName(const juce::String& raw);
// "Licence: CC0", or that TONE3000 did not say.
juce::String licenceText(const juce::String& raw);
juce::String gearLabel(const juce::String& gear);
juce::String formatLabel(const juce::String& format);
// The block category a tone's gear stands for (its stand-in tile).
Category categoryOf(const juce::String& gear, const juce::String& format);

// A tone's image in a rounded square, or the gear's tile while it loads or
// when there is none.
void drawArt(juce::Graphics& g, juce::Rectangle<float> box, const juce::Image& art, Category category, float radius);
// TONE3000's verified-creator badge (the upstream plugin's, Brand.cpp): three
// stacked check strokes in its own red, yellow and blue, 17.5 x 20.
void drawVerifiedBadge(juce::Graphics& g, juce::Rectangle<float> box);
// A creator's avatar in a circle, or a circle with the name's first letter.
void drawAvatar(juce::Graphics& g, juce::Rectangle<float> box, const juce::Image& avatar, const juce::String& username);

// Where the creator line puts the creator and the licence, so a page and its
// checks agree. Both always get room: the username gives way first, but never
// below what a few letters need, and the licence keeps its full text when it
// can.
struct CreatorLine {
  juce::Rectangle<int> avatar, creator, badge, licence;
};
CreatorLine creatorLine(juce::Rectangle<int> area, const Tone3000Credit& credit, bool withLicence);
void drawCreatorLine(juce::Graphics& g, juce::Rectangle<int> area, const Tone3000Credit& credit, const juce::Image& avatar,
                     bool withLicence = true);

}  // namespace tone3000

// The images a component shows, fetched once each through a feed (its own,
// else the shared one) and repainted when they arrive.
class Tone3000Images {
public:
  explicit Tone3000Images(juce::Component& owner);
  void setFeed(Tone3000Feed* newFeed);
  // The image when it has arrived; asks for it the first time.
  juce::Image get(const juce::String& url, bool avatar);

private:
  struct State {
    juce::Component::SafePointer<juce::Component> owner;
    std::map<juce::String, juce::Image> images;
    std::set<juce::String> asked;
  };
  std::shared_ptr<State> state;
  Tone3000Feed* feed = nullptr;
};

}  // namespace pedaleira::ui
