// The testbed's pieces for the capture-finding scenarios: a canned TONE3000
// feed (no network), and the files and loads a scenario sets up first.
#pragma once

#include "EngineClient.h"
#include "pages/Tone3000Feed.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

namespace pedaleira::ui::testbed {

// A Tone3000Feed that answers at once from a list of its own. Signed out it
// refuses what a real session would: the player's lists and the captures of a
// tone. A download copies a test capture to a temporary file.
class FakeTone3000Feed : public Tone3000Feed {
public:
  struct Setup {
    bool signedIn = false;
    juce::String user = "sample-user-a";
    // Every list fails as if TONE3000 could not be reached.
    bool unreachable = false;
    // The capture a download delivers.
    juce::File capture;
    // The player has seen and continued TONE3000's splash before.
    bool introSeen = true;
  };
  explicit FakeTone3000Feed(Setup newSetup) : setup(std::move(newSetup)) {}

  bool signedIn() const override { return setup.signedIn; }
  juce::String userName() const override { return setup.signedIn ? setup.user : juce::String(); }
  void list(Tone3000List which, std::function<void(Listed)> reply) override;
  void captures(const Tone3000Tone& tone, std::function<void(Captures)> reply) override;
  void download(const Tone3000Tone& tone, const Tone3000Capture& capture, std::function<void(Downloaded)> reply) override;
  // Stand-in artwork drawn here (the testbed has no network): a tone's image
  // and a creator's avatar, answered at once.
  void image(const juce::String& url, bool avatar, std::function<void(juce::Image)> reply) override;
  bool introSeen() const override { return setup.introSeen; }
  void markIntroSeen() override { setup.introSeen = true; }

  int downloads = 0;
  int lists = 0;

private:
  Setup setup;
};

// Sets a scenario's files and loads up, steps separated by ";":
//   file:<folder>:<name>[:<fixture>[:<make>[:<model>]]]  a capture under the scratch folder
//        (fixture "old" is an LSTM, which Pedaleira can not load)
//   load:<folder>/<name>      adds that file to the rig (so it is "loaded")
//   t3kload:<folder>/<name>   adds it to row 1, slot 1 as a download from TONE3000
//                             (a canned tone, creator and licence)
//   gone:<folder>/<name>      deletes the original of a loaded file
//   root:<folder>             adds the folder as a user folder (roots.add)
//   tone3000:<folder>         makes that folder the TONE3000 app's download folder
//   pause                     a few milliseconds, so loads have distinct times
// Returns the first problem, or an empty string.
juce::String prepareNam(EngineClient& client, const juce::File& scratch, const juce::File& fixtures, const juce::String& steps);

// A capture file of the repo's test set with another name inside it.
juce::String namVariant(const juce::File& fixtureFile, const juce::String& make, const juce::String& model);

}  // namespace pedaleira::ui::testbed
