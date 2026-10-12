#include "Tokens.h"

#include <utility>

namespace pedaleira::ui {

juce::Colour categoryColour(Category category) {
  switch (category) {
    case Category::amp: return juce::Colour(0xfff08a4b);
    case Category::drive: return juce::Colour(0xfff2c14e);
    case Category::cab: return juce::Colour(0xffc8a27a);
    case Category::dynamics: return juce::Colour(0xff62d08f);
    case Category::eq: return juce::Colour(0xff3dbfb0);
    case Category::delay: return juce::Colour(0xff4da3ff);
    case Category::reverb: return juce::Colour(0xffe27fd0);
    case Category::modulation: return juce::Colour(0xffb7e04a);
    case Category::pitch: return juce::Colour(0xffff7a90);
    case Category::filter: return juce::Colour(0xff7fe3f2);
    case Category::plugin: return juce::Colour(0xffc4b5ff);
    case Category::utility: break;
  }
  return colour::muted;
}

juce::Colour categoryTint(Category category) {
  return colour::raised.interpolatedWith(categoryColour(category), 0.16f);
}

juce::String categoryName(Category category) {
  switch (category) {
    case Category::amp: return "Amp";
    case Category::drive: return "Drive";
    case Category::cab: return "Cab";
    case Category::dynamics: return "Dynamics";
    case Category::eq: return "EQ";
    case Category::delay: return "Delay";
    case Category::reverb: return "Reverb";
    case Category::modulation: return "Modulation";
    case Category::pitch: return "Pitch";
    case Category::filter: return "Filter";
    case Category::plugin: return "Plugin";
    case Category::utility: break;
  }
  return "Utility";
}

Category categoryFromId(const juce::String& id) {
  static const std::pair<const char*, Category> ids[] = {
      {"amp", Category::amp},         {"drive", Category::drive},
      {"cab", Category::cab},         {"dynamics", Category::dynamics},
      {"eq", Category::eq},           {"delay", Category::delay},
      {"reverb", Category::reverb},   {"modulation", Category::modulation},
      {"pitch", Category::pitch},     {"filter", Category::filter},
      {"plugin", Category::plugin},   {"utility", Category::utility},
  };
  for (const auto& [name, category] : ids)
    if (id == name)
      return category;
  return Category::utility;
}

}  // namespace pedaleira::ui
