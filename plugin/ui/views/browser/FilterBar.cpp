#include "FilterBar.h"

#include <algorithm>

#include "core/GearGlyphs.h"
#include "core/Help.h"
#include "core/Labels.h"
#include "core/Paint.h"
#include "core/Theme.h"

namespace t3k::ui {

namespace {
constexpr int kDividerHeight = 24;

// The active profile chip's short label ("Recent ×" fits where "Recently
// used" wouldn't).
const char* profileChipLabel(Profile profile) {
  return profile == Profile::downloaded ? "Recent" : profileLabel(profile);
}
}  // namespace

FilterBar::FilterBar(Services& services, BrowserState& state)
    : services_(services),
      query_(state.query),
      scroller_(std::make_unique<DragScroller>(DragScroller::Axis::horizontal)) {
  scroller_->setViewedComponent(&row_, false);
  scroller_->setFocusMargin(kBleed);  // a focused chip clears the edge fade
  scroller_->onScroll = [this] { repaint(); };
  addAndMakeVisible(*scroller_);
  buildChips();
  refreshChips();
}

FilterBar::~FilterBar() = default;

// Chips
void FilterBar::buildChips() {
  profile_ = std::make_unique<FilterChip>();
  profile_->setTrailing(FilterChip::Trailing::chevron);
  profile_->onPress = [this] {
    if (menu_) {
      closeMenu();  // pressing the chip of the open menu closes it
      return;
    }
    openProfileMenu(*profile_);
  };
  profile_->onClear = [this] {
    query_.profile = Profile::none;
    changed();
  };
  profile_->leadingAvatar();
  row_.addChildComponent(*profile_);
  loadAvatar();

  for (const auto& filter : labels::gearFilters()) {
    auto chip = std::make_unique<FilterChip>(filter.label);
    chip->setLeadingSvg(gear::svgFor(filter.id));
    chip->setHelpText(help::text(help::Key::browserGear));
    chip->onPress = [this, id = juce::String(filter.id)] {
      query_.gear = query_.gear == id ? juce::String() : id;
      changed();
    };
    row_.addChildComponent(*chip);
    gear_.push_back(std::move(chip));
  }
}

void FilterBar::loadAvatar() {
  const auto user = services_.session.user();
  if (!user || user->avatarUrl.isEmpty() || user->avatarUrl == avatarUrl_) return;
  avatarUrl_ = user->avatarUrl;
  profile_->leadingAvatar().setImage(services_.images, avatarUrl_);
}

// State
void FilterBar::setGearOnly(bool gearOnly) {
  if (gearOnly == gearOnly_) return;
  gearOnly_ = gearOnly;
  closeMenu();
  refreshChips();
  scroller_->setViewPosition(0, 0);
}

void FilterBar::changed() {
  closeMenu();
  refreshChips();
  if (onChange) onChange();
}

void FilterBar::closeMenu() { menu_.reset(); }

// Every chip's label / active state from the query, then the row.
void FilterBar::refreshChips() {
  const auto& gearFilters = labels::gearFilters();
  for (size_t i = 0; i < gear_.size(); ++i) gear_[i]->setActive(query_.gear == gearFilters[i].id);
  // Signed out, nothing but the gear chips exists to the user.
  profile_->setVisible(!gearOnly_);
  if (!gearOnly_) {
    loadAvatar();
    // The avatar is the chip's name: no label until a profile is picked. A
    // chip holding a profile shows it with an × back to the trending feed.
    const bool has = query_.profile != Profile::none;
    profile_->setLabel(has ? juce::String(profileChipLabel(query_.profile)) : juce::String());
    profile_->setTrailing(has ? FilterChip::Trailing::clear : FilterChip::Trailing::chevron);
    profile_->setActive(has);
    profile_->setHelpText(help::text(has ? help::Key::browserClearFilter : help::Key::browserProfile));
  }
  layoutChips();
}

// Layout
void FilterBar::layoutChips() {
  std::vector<FilterChip*> order;
  if (!gearOnly_) order.push_back(profile_.get());
  for (auto& chip : gear_) order.push_back(chip.get());

  // The divider parts the profile chip from the gear chips; the gear-only
  // row has none.
  FilterChip* lastExtra = gearOnly_ ? nullptr : profile_.get();
  divider_ = {};
  int x = kBleed;
  for (auto* chip : order) {
    chip->setVisible(true);
    chip->setTopLeftPosition(x, 0);
    x += chip->getWidth() + kGap;
    if (chip == lastExtra) {
      divider_ = {x, (kHeight - kDividerHeight) / 2, 1, kDividerHeight};
      x += 1 + kGap;
    }
  }
  row_.setSize(std::max(x - kGap + kBleed, getWidth()), kHeight);
  row_.repaint();
  repaint();
}

void FilterBar::resized() {
  scroller_->setBounds(getLocalBounds());
  // Shorter rows never scroll: the content is at least the viewport.
  row_.setSize(std::max(row_.getWidth(), getWidth()), kHeight);
}

void FilterBar::paintOverChildren(juce::Graphics& g) {
  g.setColour(theme::kBorder);
  g.fillRect(divider_.translated(-scroller_->getViewPositionX(), 0));
  paint::edgeFades(g, getLocalBounds().toFloat(), kBleed, juce::Colours::black);
}

// Menus
void FilterBar::openProfileMenu(FilterChip& chip) {
  std::vector<FilterMenu::Option> options;
  for (const auto profile : kProfiles) {
    // Favorites carries the bookmark it is kept with (the old tabs' icon).
    const auto icon = profile == Profile::favorited ? std::optional(Icon::Bookmark) : std::nullopt;
    options.push_back({profileEndpoint(profile), profileLabel(profile), icon});
  }
  menu_ = std::make_unique<FilterMenu>();
  menu_->setOptions(std::move(options), query_.isTrending() ? juce::String() : juce::String(profileEndpoint(query_.profile)));
  menu_->onPick = [this](const juce::String& id) {
    for (const auto profile : kProfiles)
      if (id == profileEndpoint(profile)) query_.profile = profile;
    changed();
  };
  menu_->onDismiss = [this] { closeMenu(); };
  menu_->openBelow(chip);
}

}  // namespace t3k::ui
