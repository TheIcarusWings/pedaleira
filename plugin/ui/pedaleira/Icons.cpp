#include "Icons.h"

#include <map>

namespace pedaleira::ui::icons {

namespace {

// From the canvas (the rail of every board and the board "Design system").
const char* source(Icon icon) {
  switch (icon) {
    case Icon::grid: return "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z";
    case Icon::library: return "M5 4v16M10 4v16M15 5l4 15";
    case Icon::capture:
      return "M12 3a3 3 0 0 0-3 3v5a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM6 11a6 6 0 0 0 12 0M12 17v4";
    case Icon::import: return "M12 4v11M7 10l5 5 5-5M4 20h16";
    case Icon::live: return "M4 12a8 8 0 1 0 16 0a8 8 0 1 0-16 0M12 8v4l3 2";
    case Icon::settings:
      return "M4 7h10M18 7h2M4 17h2M10 17h10M14 7a2 2 0 1 0 4 0a2 2 0 1 0-4 0M6 17a2 2 0 1 0 4 "
             "0a2 2 0 1 0-4 0";
    case Icon::check: return "M5 12l5 5 9-10";
    case Icon::plus: return "M12 5v14M5 12h14";
    case Icon::minus: return "M5 12h14";
    case Icon::close: return "M6 6l12 12M18 6L6 18";
    case Icon::chevronUp: return "M6 15l6-6 6 6";
    case Icon::chevronDown: return "M6 9l6 6 6-6";
    case Icon::chevronRight: return "M9 6l6 6-6 6";
    case Icon::arrowLeft: return "M19 12H6M11 6l-6 6 6 6";
    case Icon::star: return "M12 4l2.5 5.2 5.7.8-4.1 4 1 5.7L12 17l-5.1 2.7 1-5.7-4.1-4 5.7-.8z";
    case Icon::search: return "M11 4a7 7 0 1 0 0 14a7 7 0 1 0 0-14M21 21l-5-5";
    case Icon::warning: return "M12 4l9 16H3zM12 10v5M12 17.5v.5";
    case Icon::info: return "M4 12a8 8 0 1 0 16 0a8 8 0 1 0-16 0M12 11v5M12 8v.5";
    case Icon::attention: return "M12 8v5M12 16.5v.5M4 12a8 8 0 1 0 16 0a8 8 0 1 0-16 0";
    case Icon::ear: return "M8 9a4 4 0 0 1 8 0c0 3-3 3.5-3 6a2.5 2.5 0 0 1-5 0";
    case Icon::lock: return "M7 11V8a5 5 0 0 1 10 0v3M6 11h12v9H6z";
    case Icon::device:
      return "M5 4h14a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2zM7 8h10v4H7zM8 "
             "16a1.25 1.25 0 1 0 2.5 0a1.25 1.25 0 1 0-2.5 0M13.5 16a1.25 1.25 0 1 0 2.5 0a1.25 1.25 "
             "0 1 0-2.5 0";
    case Icon::helix:
      return "M4 5h16a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1zM7 9h6M6 15a1 "
             "1 0 1 0 2 0a1 1 0 1 0-2 0M11 15a1 1 0 1 0 2 0a1 1 0 1 0-2 0M16 15a1 1 0 1 0 2 0a1 1 "
             "0 1 0-2 0";
    case Icon::document: return "M7 3h7l5 5v13H7zM14 3v5h5M10 14h6M10 17.5h4";
    case Icon::pedaleiraPresets:
      return "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM15.5 17h5M18 14.5l2.5 2.5-2.5 2.5";
    case Icon::capturePedal:
      return "M6 3h12a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM8 7a1 1 0 1 0 2 "
             "0a1 1 0 1 0-2 0M14 7a1 1 0 1 0 2 0a1 1 0 1 0-2 0M9.5 16.5a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 "
             "0-5 0M2 10h2M20 10h2";
    case Icon::captureAmpHead:
      return "M3 9h18v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1zM8 9V6.5a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1V9M7 "
             "14h.01M10.5 14h.01M14 14h.01M17.5 14h.01";
    case Icon::captureAmpCab:
      return "M4 6h10a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1zM5.5 15a3.5 "
             "3.5 0 1 0 7 0a3.5 3.5 0 1 0-7 0M6 9h.01M9 9h.01M12 9h.01M19 3a2 2 0 0 1 2 2v3a2 2 0 0 "
             "1-4 0V5a2 2 0 0 1 2-2zM19 12v7M16.5 20h5";
    case Icon::captureModeller:
      return "M3 5h18a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1zM6 8.5h12v4H6zM7.5 "
             "10.5l1.5-1 1.5 2 1.5-2 1.5 1M7 16h.01M11 16h.01M15 16h.01";
    case Icon::play: return "M8 5v14l11-7z";
    case Icon::power: return "M12 3v8M7.05 6.05a7 7 0 1 0 9.9 0";
    case Icon::trash: return "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3";
  }
  return "";
}

const char* source(Category category) {
  switch (category) {
    case Category::amp:
      return "M5 5h14a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2zM3 9h18M9.5 "
             "14a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0";
    case Category::drive:
      return "M8 3h8a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM10 8a2 2 0 1 0 "
             "4 0a2 2 0 1 0-4 0M9 16h6";
    case Category::cab:
      return "M6 3h12a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM7 12a5 5 0 1 0 "
             "10 0a5 5 0 1 0-10 0M11 12a1 1 0 1 0 2 0a1 1 0 1 0-2 0";
    case Category::dynamics: return "M4 6l5 6-5 6M20 6l-5 6 5 6M12 4v16";
    case Category::eq: return "M6 4v16M12 4v16M18 4v16M4 9h4M10 15h4M16 7h4";
    case Category::delay: return "M5 5v14M10 8v8M15 10v4M19 11v2";
    case Category::reverb: return "M4 20a16 16 0 0 1 16-16M4 14a10 10 0 0 1 10-10M4 8a4 4 0 0 1 4-4";
    case Category::modulation: return "M3 12c3-8 6-8 9 0s6 8 9 0";
    case Category::pitch: return "M8 20V5M4 9l4-4 4 4M16 4v15M12 15l4 4 4-4";
    case Category::filter: return "M3 8h8c4 0 5 3 6 10M3 20h18";
    case Category::creative: return "M12 3l2 7 7 2-7 2-2 7-2-7-7-2 7-2z";
    case Category::plugin: return "M9 3v5M15 3v5M7 8h10v4a5 5 0 0 1-10 0zM12 17v4";
    case Category::utility: break;
  }
  return "M4 12a8 8 0 1 0 16 0a8 8 0 1 0-16 0M12 12l4-4";
}

template <typename Key>
const juce::Path& cached(Key key, const char* svg) {
  static std::map<Key, juce::Path> made;
  auto found = made.find(key);
  if (found == made.end())
    found = made.emplace(key, juce::Drawable::parseSVGPath(svg)).first;
  return found->second;
}

}  // namespace

const juce::Path& path(Icon icon) { return cached(icon, source(icon)); }
const juce::Path& path(Category category) { return cached(category, source(category)); }

void draw(juce::Graphics& g, const juce::Path& icon, juce::Rectangle<float> area, juce::Colour colour,
          float stroke) {
  const float scale = juce::jmin(area.getWidth(), area.getHeight()) / 24.0f;
  const juce::AffineTransform place = juce::AffineTransform::scale(scale).translated(
      area.getCentreX() - 12.0f * scale, area.getCentreY() - 12.0f * scale);
  g.setColour(colour);
  // The transform is applied before stroking, so the width is scaled too:
  // the stroke is in the icon's own 24 px units, as the canvas draws it.
  g.strokePath(icon,
               juce::PathStrokeType(stroke * scale, juce::PathStrokeType::curved,
                                    juce::PathStrokeType::rounded),
               place);
}

}  // namespace pedaleira::ui::icons
