// The browser's filter row: one line of chips scrolling sideways under the
// gallery's edge fades, editing the BrowserState's ToneQuery.
//
//   [Profile ▾] | [Amp + Cab] [Amp Head] …
//
// The Profile chip opens a small menu (Recently used, Favorites, Created)
// and, holding a value, shows it with an × that clears it back to the
// trending feed. The gear chips narrow whichever list is shown. Nothing
// here searches the catalog: that is outside TONE3000's free API tier.
//
// Signed out (the browser's trending preview) the row is the gear chips
// alone: the profile lists need a session. The gear pick is the same query
// field, so it carries over once the user signs in.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "FilterChip.h"
#include "FilterMenu.h"
#include "services/Services.h"
#include "widgets/DragScroller.h"
#include "views/gallery/GalleryGeometry.h"

namespace t3k::ui {

class FilterBar : public juce::Component {
public:
  static constexpr int kHeight = FilterChip::kHeight;
  static constexpr int kBleed = gallery::kEdgeFadeWidth;
  static constexpr int kGap = 10;

  FilterBar(Services& services, BrowserState& state);
  ~FilterBar() override;

  // The bounds of the content column: the row extends kBleed past it.
  void setColumn(juce::Rectangle<int> column) { setBounds(column.expanded(kBleed, 0).withHeight(kHeight)); }

  // Gear chips alone (signed out), or the full row.
  void setGearOnly(bool gearOnly);

  // Any filter changed: the browser fetches page 1.
  std::function<void()> onChange;

  void resized() override;
  void paintOverChildren(juce::Graphics& g) override;

private:
  void buildChips();
  void layoutChips();
  void refreshChips();
  // The profile chip's avatar, once a signed-in user with one is known (the
  // row may be built signed out and live through the sign-in).
  void loadAvatar();
  // A filter changed: refresh the row, close any open menu (a press on a
  // chip's × while its menu is up), tell the owner.
  void changed();
  void closeMenu();
  void openProfileMenu(FilterChip& chip);

  Services& services_;
  ToneQuery& query_;  // state.query
  bool gearOnly_ = false;

  std::unique_ptr<DragScroller> scroller_;
  juce::Component row_;
  std::unique_ptr<FilterChip> profile_;
  std::vector<std::unique_ptr<FilterChip>> gear_;
  juce::String avatarUrl_;  // the one requested, if any
  juce::Rectangle<int> divider_;

  std::unique_ptr<FilterMenu> menu_;  // the open menu
};

}  // namespace t3k::ui
