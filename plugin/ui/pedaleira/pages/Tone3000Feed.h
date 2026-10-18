// What the TONE3000 page of the block library asks of TONE3000: the bounded
// lists of the free API tier (trending, latest, and the signed-in player's
// favorites, downloads and creations), the captures of one tone, and a
// capture downloaded to a file. The page does not know how: the application
// gives it a feed made from the TONE3000 client the application already has
// (standalone/Tone3000LiveFeed), the testbed gives it a canned one.
//
// Not the engine: the engine knows nothing of the network. A downloaded
// capture reaches the rig as a file, through chain.addToneFromFiles.
// Message thread only; every reply comes back on it.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <vector>

namespace pedaleira::ui {

// One tone of a list, with what TONE3000 asks every list to show: its image,
// title, gear, format and creator (username and avatar), and its licence.
struct Tone3000Tone {
  int id = 0;
  juce::String title;
  // The creator's username, as TONE3000 shows it.
  juce::String creator;
  // The name a verified creator goes by, when there is one.
  juce::String creatorName;
  bool creatorVerified = false;
  juce::String avatarUrl;
  // The tone's first image; empty when it has none.
  juce::String imageUrl;
  // The catalog's gear id: amp, full-rig, pedal, outboard, ir, ...
  juce::String gear;
  // nam or ir.
  juce::String format;
  juce::String description;
  juce::String licence;
  juce::String url;
  // How many captures the tone has that Pedaleira loads (A2 for NAM).
  int captures = 0;
  bool favourite = false;
};

// One capture (model) of a tone.
struct Tone3000Capture {
  int id = 0;
  juce::String name;
  juce::String url;
};

enum class Tone3000List { trending, latest, favorites, downloaded, created };

inline const char* tone3000ListName(Tone3000List list) {
  switch (list) {
    case Tone3000List::trending: return "Trending";
    case Tone3000List::latest: return "Latest";
    case Tone3000List::favorites: return "Favorites";
    case Tone3000List::downloaded: return "Downloaded";
    case Tone3000List::created: return "Created";
  }
  return "";
}

// Lists that need a TONE3000 account; the other two can be read signed out.
inline bool tone3000ListNeedsAccount(Tone3000List list) {
  return list == Tone3000List::favorites || list == Tone3000List::downloaded || list == Tone3000List::created;
}

class Tone3000Feed {
public:
  virtual ~Tone3000Feed() = default;

  // The feed of this application, for the pages that show a TONE3000 tone
  // without browsing (a capture's editor): its images. Set by the block
  // library when it is given one; null when there is none.
  static Tone3000Feed* shared() { return sharedSlot(); }
  static void setShared(Tone3000Feed* feed) { sharedSlot() = feed; }

  // A TONE3000 session exists on this computer (a sign-in made by this
  // application or another build that shares its preferences).
  virtual bool signedIn() const = 0;
  // Who, when it is known; empty otherwise.
  virtual juce::String userName() const = 0;

  struct Listed {
    bool ok = false;
    // A sentence for the player when it did not work.
    juce::String error;
    // The reply says the player has to sign in.
    bool needsAccount = false;
    std::vector<Tone3000Tone> tones;
  };
  virtual void list(Tone3000List which, std::function<void(Listed)> reply) = 0;

  struct Captures {
    bool ok = false;
    juce::String error;
    bool needsAccount = false;
    std::vector<Tone3000Capture> captures;
  };
  virtual void captures(const Tone3000Tone& tone, std::function<void(Captures)> reply) = 0;

  struct Downloaded {
    // The file, named after the tone and the capture; not a File when it failed.
    juce::File file;
    juce::String error;
  };
  // Downloads one capture, only when the player asked for it (TONE3000's
  // terms: no bulk caching). The file is a .nam or .wav kept in the library's
  // TONE3000 folder; a capture downloaded before is not downloaded again.
  virtual void download(const Tone3000Tone& tone, const Tone3000Capture& capture,
                        std::function<void(Downloaded)> reply) = 0;

  // A tone's image or a creator's avatar, fetched when it is first shown;
  // a null image when it can not be had (the page draws a stand-in).
  virtual void image(const juce::String& url, bool avatar, std::function<void(juce::Image)> reply) = 0;

  // The TONE3000 splash was seen and continued by this user (kept in the
  // application's preferences), so browsing opens straight away.
  virtual bool introSeen() const = 0;
  virtual void markIntroSeen() = 0;

private:
  static Tone3000Feed*& sharedSlot() {
    static Tone3000Feed* feed = nullptr;
    return feed;
  }
};

}  // namespace pedaleira::ui
