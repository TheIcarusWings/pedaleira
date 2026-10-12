// What the Select tone screen remembers between visits: the browser is
// mounted only while open, so its query, filter row and last page live
// here for the editor's lifetime instead. Plain data; ToneBrowser and
// FilterBar read and write it directly.
#pragma once

#include <optional>

#include "ToneSession.h"
#include "model/ToneQuery.h"

namespace t3k::ui {

struct BrowserState {
  ToneQuery query;
  int page = 1;
  // The page last shown, rendered again at once on return; it only
  // refreshes on the user's next filter change or page turn.
  std::optional<TonePage> result;
  // Whether `result` is the trending feed (one page, narrowed by
  // `query.gear` alone; the signed-out preview always shows it) rather than
  // a page of a profile stream: a return reuses it only while that still
  // matches the session state and the query.
  bool resultIsTrending = false;
};

}  // namespace t3k::ui
