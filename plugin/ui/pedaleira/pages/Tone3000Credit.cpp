#include "Tone3000Credit.h"

#include "../Icons.h"
#include "../Type.h"
#include "PedaleiraUiData.h"

namespace pedaleira::ui {

namespace {

constexpr float kWordmarkAspect = 210.0f / 32.0f;
int wordmarkCount = 0;

const juce::Drawable& wordmark() {
  // Parsed once; the artwork is TONE3000's, drawn exactly as it ships.
  static const std::unique_ptr<juce::Drawable> drawable =
      juce::Drawable::createFromImageData(PedaleiraUiData::t3k_svg, static_cast<size_t>(PedaleiraUiData::t3k_svgSize));
  jassert(drawable != nullptr);
  return *drawable;
}

juce::String text(const juce::var& value) { return value.isVoid() || value.isUndefined() ? juce::String() : value.toString().trim(); }

}  // namespace

// ####################
// CREDIT
// ####################
Tone3000Credit Tone3000Credit::fromAttribution(const juce::var& a) {
  Tone3000Credit c;
  if (!a.isObject())
    return c;
  c.id = a["id"];
  c.title = text(a["title"]);
  c.url = text(a["url"]);
  c.gear = text(a["gear"]);
  c.format = text(a["format"]);
  c.licence = text(a["license"]);
  c.imageUrl = text(a["image"]);
  c.capture = text(a["capture"]);
  const juce::var user = a["user"];
  c.username = text(user["username"]);
  c.creatorName = text(user["display_name"]);
  c.avatarUrl = text(user["avatar_url"]);
  c.verified = static_cast<bool>(user["is_verified"]);
  return c;
}

Tone3000Credit Tone3000Credit::fromSummary(const juce::var& tone) {
  Tone3000Credit c;
  if (!tone.isObject())
    return c;
  c.id = tone["id"];
  c.title = text(tone["title"]);
  c.url = text(tone["url"]);
  c.gear = text(tone["gear"]);
  c.format = text(tone["format"]);
  const juce::var licence = tone["license"];
  c.licence = text(licence.isObject() ? licence["name"] : licence);
  if (const auto* images = tone["images"].getArray(); images != nullptr && !images->isEmpty())
    c.imageUrl = text(images->getReference(0));
  const juce::var user = tone["user"];
  c.username = text(user["username"]);
  c.creatorName = text(user["display_name"]);
  c.avatarUrl = text(user["avatar_url"]);
  c.verified = static_cast<bool>(user["is_verified"]);
  return c;
}

Tone3000Credit Tone3000Credit::fromTone(const Tone3000Tone& tone, const juce::String& captureName) {
  Tone3000Credit c;
  c.id = tone.id;
  c.title = tone.title;
  c.url = tone.url;
  c.gear = tone.gear;
  c.format = tone.format;
  c.licence = tone.licence;
  c.imageUrl = tone.imageUrl;
  c.capture = captureName;
  c.username = tone.creator;
  c.creatorName = tone.creatorName;
  c.avatarUrl = tone.avatarUrl;
  c.verified = tone.creatorVerified;
  return c;
}

juce::var Tone3000Credit::toAttribution() const {
  juce::DynamicObject::Ptr out = new juce::DynamicObject();
  if (id > 0)
    out->setProperty("id", id);
  const std::pair<const char*, juce::String> fields[] = {{"title", title},   {"url", url},          {"gear", gear},
                                                         {"format", format}, {"license", licence},  {"image", imageUrl},
                                                         {"capture", capture}};
  for (const auto& [key, value] : fields)
    if (value.isNotEmpty())
      out->setProperty(key, value);
  if (username.isNotEmpty()) {
    juce::DynamicObject::Ptr user = new juce::DynamicObject();
    user->setProperty("username", username);
    if (creatorName.isNotEmpty())
      user->setProperty("display_name", creatorName);
    if (avatarUrl.isNotEmpty())
      user->setProperty("avatar_url", avatarUrl);
    user->setProperty("is_verified", verified);
    out->setProperty("user", juce::var(user.get()));
  }
  return juce::var(out.get());
}

namespace tone3000 {

// ####################
// WORDMARK
// ####################
float wordmarkWidth(float height) { return height * kWordmarkAspect; }

float wordmarkClearSpace(float height) { return height * 0.5f; }

void drawWordmark(juce::Graphics& g, juce::Rectangle<float> box, juce::Justification placement) {
  // Its own proportions, whatever the box: never stretched.
  const float h = juce::jmin(box.getHeight(), box.getWidth() / kWordmarkAspect);
  const auto mark = placement.appliedToRectangle(juce::Rectangle<float>(wordmarkWidth(h), h), box);
  wordmark().drawWithin(g, mark, juce::RectanglePlacement::centred, 1.0f);
  ++wordmarkCount;
}

int wordmarkDraws() { return wordmarkCount; }

namespace {
constexpr float kPoweredByMark = 14.0f;
const char* const kPoweredBy = "Powered by";
}  // namespace

int poweredByWidth(int height) {
  juce::ignoreUnused(height);
  return juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f, type::Weight::medium), kPoweredBy) + 8.0f +
                          wordmarkWidth(kPoweredByMark));
}

juce::Rectangle<float> drawPoweredBy(juce::Graphics& g, juce::Rectangle<int> area) {
  const juce::Font font = type::sans(12.0f, type::Weight::medium);
  const int textWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, kPoweredBy));
  g.setFont(font);
  g.setColour(colour::muted);
  g.drawText(kPoweredBy, area.withWidth(textWidth + 2), juce::Justification::centredLeft, false);
  // The mark sits on the text's baseline band, with its clear space kept free
  // of anything but the page (the text ends a full clear space before it).
  const auto box = juce::Rectangle<float>(static_cast<float>(area.getX() + textWidth + 8), area.getCentreY() - kPoweredByMark / 2.0f + 1.0f,
                                          wordmarkWidth(kPoweredByMark), kPoweredByMark);
  drawWordmark(g, box);
  return box;
}

// ####################
// LABELS
// ####################
juce::String licenceName(const juce::String& raw) {
  const juce::String id = raw.trim();
  if (id.isEmpty())
    return {};
  // Already a name ("CC BY-NC 4.0"): as it came.
  if (id.containsAnyOf(" ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
    return id;
  const juce::String lower = id.toLowerCase().replaceCharacter('_', '-');
  if (lower == "cc0" || lower == "cc-0" || lower == "cc-zero")
    return "CC0";
  if (lower.startsWith("cc-"))
    return "CC " + lower.substring(3).toUpperCase();
  const juce::String words = lower.replaceCharacter('-', ' ');
  return words.substring(0, 1).toUpperCase() + words.substring(1);
}

juce::String licenceText(const juce::String& raw) {
  const juce::String name = licenceName(raw);
  return name.isNotEmpty() ? "Licence: " + name : juce::String("Licence not given by TONE3000");
}

juce::String gearLabel(const juce::String& gear) {
  const juce::String id = gear.toLowerCase();
  if (id == "amp")
    return "Amp Head";
  if (id == "amp-cab" || id == "amp_cab" || id == "full-rig" || id == "full_rig")
    return "Amp + Cab";
  if (id == "cab")
    return "Cabinet";
  if (id == "ir")
    return "IR";
  if (id == "space" || id == "spaces")
    return "Spaces";
  if (id.isEmpty())
    return "Gear not given";
  return id.substring(0, 1).toUpperCase() + id.substring(1);
}

juce::String formatLabel(const juce::String& format) {
  if (format.equalsIgnoreCase("nam"))
    return "NAM A2";
  if (format.equalsIgnoreCase("ir"))
    return "IR";
  return format.isEmpty() ? juce::String("Format not given") : format.toUpperCase();
}

Category categoryOf(const juce::String& gear, const juce::String& format) {
  const juce::String id = format.equalsIgnoreCase("ir") ? juce::String("ir") : gear.toLowerCase();
  if (id == "amp" || id == "amp-cab" || id == "amp_cab" || id == "full-rig" || id == "full_rig")
    return Category::amp;
  if (id == "pedal")
    return Category::drive;
  if (id == "cab" || id == "ir")
    return Category::cab;
  if (id == "outboard")
    return Category::eq;
  if (id == "space" || id == "spaces")
    return Category::reverb;
  return Category::utility;
}

// ####################
// ART AND AVATARS
// ####################
void drawArt(juce::Graphics& g, juce::Rectangle<float> box, const juce::Image& art, Category category, float radius) {
  if (art.isValid()) {
    juce::Graphics::ScopedSaveState saved(g);
    juce::Path clip;
    clip.addRoundedRectangle(box, radius);
    g.reduceClipRegion(clip);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    // Cover: the square's middle.
    g.drawImage(art, box, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    return;
  }
  g.setColour(categoryTint(category));
  g.fillRoundedRectangle(box, radius);
  const float inset = box.getWidth() * 0.24f;
  icons::draw(g, icons::path(category), box.reduced(inset), categoryColour(category));
}

// The same geometry and colours as the upstream plugin's badge (core/Brand.cpp):
// one check stroked three times, flattened to filled polygons.
constexpr const char* kVerifiedBadgeSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 17.5 20">
<polygon points="17.50,5.88 5.47,20.00 0.00,12.96 2.83,11.81 5.58,15.34 11.60,8.27" fill="#0000ff"/>
<polygon points="17.50,0.00 5.47,14.12 0.00,7.08 2.83,5.93 5.58,9.46 11.60,2.38" fill="#ff0000"/>
<polygon points="17.50,2.94 5.47,17.06 0.00,10.02 2.83,8.87 5.58,12.40 11.60,5.32" fill="#ffff00"/>
</svg>)svg";

void drawVerifiedBadge(juce::Graphics& g, juce::Rectangle<float> box) {
  static const std::unique_ptr<juce::Drawable> badge = juce::Drawable::createFromSVGString(kVerifiedBadgeSvg);
  if (badge != nullptr)
    badge->drawWithin(g, box, juce::RectanglePlacement::centred, 1.0f);
}

void drawAvatar(juce::Graphics& g, juce::Rectangle<float> box, const juce::Image& avatar, const juce::String& username) {
  if (avatar.isValid()) {
    juce::Graphics::ScopedSaveState saved(g);
    juce::Path clip;
    clip.addEllipse(box);
    g.reduceClipRegion(clip);
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    g.drawImage(avatar, box, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    return;
  }
  g.setColour(colour::control);
  g.fillEllipse(box);
  g.setColour(colour::muted);
  g.setFont(type::sans(box.getHeight() * 0.56f, type::Weight::semibold));
  g.drawText(username.substring(0, 1).toUpperCase(), box, juce::Justification::centred, false);
}

// ####################
// CREATOR LINE
// ####################
namespace {
juce::Font creatorFont() { return type::sans(12.0f, type::Weight::semibold); }
juce::Font licenceFont() { return type::sans(12.0f); }
juce::String creatorText(const Tone3000Credit& credit) {
  return credit.username.isNotEmpty() ? credit.username : juce::String("Creator not given by TONE3000");
}
constexpr int kSeparator = 18;
// The verified badge after the username: 12 x 14 and its gap.
constexpr int kBadge = 12 + 6;
// The least either text is given: a few letters and the ellipsis.
constexpr int kLeast = 52;
}  // namespace

CreatorLine creatorLine(juce::Rectangle<int> area, const Tone3000Credit& credit, bool withLicence) {
  CreatorLine l;
  const int side = juce::jlimit(14, 20, area.getHeight());
  l.avatar = area.removeFromLeft(side).withSizeKeepingCentre(side, side);
  area.removeFromLeft(6);
  const int creatorWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(creatorFont(), creatorText(credit))) + 2;
  // A verified creator's badge stays beside the name.
  const int badge = credit.verified ? kBadge : 0;
  const auto place = [&l, badge](juce::Rectangle<int>& row, int width) {
    l.creator = row.removeFromLeft(juce::jmax(0, width - badge));
    if (badge > 0)
      l.badge = row.removeFromLeft(badge).withTrimmedLeft(6).withSizeKeepingCentre(12, 14);
  };
  if (!withLicence) {
    place(area, juce::jmin(area.getWidth(), creatorWidth + badge));
    return l;
  }
  const int licenceWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(licenceFont(), licenceText(credit.licence))) + 2;
  const int room = area.getWidth() - kSeparator;
  int forCreator = creatorWidth, forLicence = licenceWidth;
  if (forCreator + badge + forLicence > room) {
    // The username gives way first, down to a few letters; then the licence.
    forCreator = juce::jmax(juce::jmin(creatorWidth, kLeast), room - badge - licenceWidth);
    forLicence = juce::jmax(juce::jmin(licenceWidth, kLeast), room - badge - forCreator);
    forCreator = juce::jmax(0, room - badge - forLicence);
  }
  forCreator += badge;
  place(area, forCreator);
  area.removeFromLeft(kSeparator);
  l.licence = area.removeFromLeft(forLicence);
  return l;
}

void drawCreatorLine(juce::Graphics& g, juce::Rectangle<int> area, const Tone3000Credit& credit, const juce::Image& avatar,
                     bool withLicence) {
  const CreatorLine l = creatorLine(area, credit, withLicence);
  drawAvatar(g, l.avatar.toFloat(), avatar, credit.username);
  g.setFont(creatorFont());
  g.setColour(credit.username.isNotEmpty() ? colour::text : colour::muted);
  g.drawText(creatorText(credit), l.creator, juce::Justification::centredLeft, true);
  if (!l.badge.isEmpty())
    drawVerifiedBadge(g, l.badge.toFloat());
  if (!withLicence)
    return;
  const int end = l.badge.isEmpty() ? l.creator.getRight() : l.badge.getRight();
  g.setColour(colour::faint);
  g.setFont(licenceFont());
  g.drawText(type::dot(), end, l.creator.getY(), kSeparator, l.creator.getHeight(), juce::Justification::centred, false);
  g.setColour(colour::muted);
  g.drawText(licenceText(credit.licence), l.licence, juce::Justification::centredLeft, true);
}

}  // namespace tone3000

// ####################
// IMAGES
// ####################
Tone3000Images::Tone3000Images(juce::Component& owner) : state(std::make_shared<State>()) { state->owner = &owner; }

void Tone3000Images::setFeed(Tone3000Feed* newFeed) {
  if (newFeed == feed)
    return;
  feed = newFeed;
  // Another feed may answer what the last one could not.
  state->asked.clear();
  state->images.clear();
}

juce::Image Tone3000Images::get(const juce::String& url, bool avatar) {
  if (url.isEmpty())
    return {};
  if (const auto found = state->images.find(url); found != state->images.end())
    return found->second;
  Tone3000Feed* source = feed != nullptr ? feed : Tone3000Feed::shared();
  if (source == nullptr || state->asked.count(url) > 0)
    return {};
  state->asked.insert(url);
  std::weak_ptr<State> weak = state;
  source->image(url, avatar, [weak, url](juce::Image image) {
    const auto alive = weak.lock();
    if (alive == nullptr || !image.isValid())
      return;
    alive->images[url] = image;
    if (alive->owner != nullptr)
      alive->owner->repaint();
  });
  // A feed that answers at once has already put it in.
  if (const auto found = state->images.find(url); found != state->images.end())
    return found->second;
  return {};
}

}  // namespace pedaleira::ui
