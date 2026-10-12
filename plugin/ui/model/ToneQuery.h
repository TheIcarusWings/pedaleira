// What the tone browser asks the catalog for: a gear filter and, optionally,
// one of the signed-in user's own streams.
//
// Pedaleira runs on TONE3000's free API tier, which has no catalog search,
// filters or taxonomy (see Tone3000Client::kFreeTier). The default query is
// the trending feed (GET /tones/trending, the homepage's top 10, narrowed by
// gear); a profile filter (Recently used, Favorites, Created) instead pages
// one of the user's streams (GET /tones/{downloaded|favorited|created}).
// Both are bounded lists the free tier allows.
#pragma once

#include <juce_core/juce_core.h>

namespace t3k::ui {

// The Profile chip's options, in menu order. `none` is the trending feed.
enum class Profile { none, downloaded, favorited, created };
inline constexpr Profile kProfiles[] = {Profile::downloaded, Profile::favorited, Profile::created};
// "Recently used", "Favorites", "Created"; "" for none.
const char* profileLabel(Profile profile);
// The list endpoint's path segment ("downloaded", ..., "trending" for none).
const char* profileEndpoint(Profile profile);

struct ToneQuery {
  juce::String gear;  // "" = any (single, like the chip row)
  Profile profile = Profile::none;

  // The trending feed: one unpaginated page of ten.
  bool isTrending() const { return profile == Profile::none; }

  // The API path with its query string for one page. Trending takes the gear
  // alone; the profile streams page too.
  juce::String requestPath(int page, int pageSize) const;

  bool operator==(const ToneQuery& o) const { return gear == o.gear && profile == o.profile; }
  bool operator!=(const ToneQuery& o) const { return !(*this == o); }
};

}  // namespace t3k::ui
