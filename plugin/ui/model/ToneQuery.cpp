#include "ToneQuery.h"

namespace t3k::ui {

const char* profileLabel(Profile profile) {
  switch (profile) {
    case Profile::none: return "";
    case Profile::downloaded: return "Recently used";
    case Profile::favorited: return "Favorites";
    case Profile::created: return "Created";
  }
  return "";
}

const char* profileEndpoint(Profile profile) {
  switch (profile) {
    case Profile::none: return "trending";
    case Profile::downloaded: return "downloaded";
    case Profile::favorited: return "favorited";
    case Profile::created: return "created";
  }
  return "trending";
}

juce::String ToneQuery::requestPath(int page, int pageSize) const {
  juce::StringArray parts;
  if (!isTrending()) parts.addArray({"page=" + juce::String(page), "page_size=" + juce::String(pageSize)});
  // The trending feed names its filter `gear`, as the streams do.
  if (gear.isNotEmpty()) parts.add("gear=" + juce::URL::addEscapeChars(gear, true));
  const juce::String base = "/api/v1/tones/" + juce::String(profileEndpoint(profile));
  return parts.isEmpty() ? base : base + "?" + parts.joinIntoString("&");
}

}  // namespace t3k::ui
