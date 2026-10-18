// The application's Tone3000Feed: the TONE3000 client the classic editor
// already has (token store, free-tier allow-list, HTTP), pointed at the same
// preferences file, so a sign-in made there is seen here. Nothing here holds a
// secret key: the client sends the publishable key as the OAuth client id and
// a session's tokens come from TONE3000's own sign-in. Tone images and avatars
// come through the classic editor's image loader, and that the TONE3000 splash
// was seen is kept in the same preferences file, per user. A downloaded capture
// is kept in the library's TONE3000 folder and a capture downloaded before is
// reused, both through the command layer (nam.keepDownload, nam.downloaded).
// Compiled with the application only; the testbed has a canned feed.
#pragma once

#include "../EngineClient.h"
#include "../pages/Tone3000Feed.h"

#include <memory>

namespace pedaleira::ui {

std::unique_ptr<Tone3000Feed> makeTone3000LiveFeed(EngineClient& engine);

}  // namespace pedaleira::ui
