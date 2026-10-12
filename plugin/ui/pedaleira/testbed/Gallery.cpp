#include "Gallery.h"

#include "Paint.h"
#include "Type.h"

namespace pedaleira::ui::testbed {

namespace {
const Category kCategories[] = {Category::amp,    Category::drive,      Category::cab,   Category::dynamics,
                                Category::eq,     Category::delay,      Category::reverb, Category::modulation,
                                Category::pitch,  Category::filter,     Category::utility, Category::plugin};
}  // namespace

Gallery::Gallery() {
  setTitle("Design system");
  for (const Category category : kCategories) {
    auto tile = std::make_unique<Tile>();
    tile->setBlock(categoryName(category), category, Tile::State::active);
    addAndMakeVisible(*tile);
    categoryTiles.push_back(std::move(tile));
  }
  const auto stateTile = [this](const juce::String& name, Category category, Tile::State state, bool selected,
                                const juce::String& footswitch) {
    auto tile = std::make_unique<Tile>();
    if (state == Tile::State::empty)
      tile->setEmpty();
    else
      tile->setBlock(name, category, state);
    tile->setSelected(selected);
    tile->setFootswitch(footswitch);
    addAndMakeVisible(*tile);
    stateTiles.push_back(std::move(tile));
  };
  stateTile("Brit 800", Category::amp, Tile::State::active, false, "");
  stateTile("Brit 800", Category::amp, Tile::State::active, true, "");
  stateTile("Brit 800", Category::amp, Tile::State::bypassed, false, "");
  stateTile("Seventh H.", Category::plugin, Tile::State::missing, false, "");
  stateTile("", Category::utility, Tile::State::empty, false, "");
  stateTile("Brit 800", Category::amp, Tile::State::loading, false, "");
  stateTile("Green 808", Category::drive, Tile::State::active, false, "A");

  const auto knob = [this](const juce::String& name, double position, const juce::String& text,
                           Knob::Assignment assignment, bool bypassed) {
    auto k = std::make_unique<Knob>(name);
    k->setPosition(position);
    k->setValueText(text);
    k->setAssignment(assignment);
    k->setBypassed(bypassed);
    addAndMakeVisible(*k);
    knobs.push_back(std::move(k));
  };
  knob("Gain", 0.7, "7.0", Knob::Assignment::none, false);
  knob("Mix", 0.3, "30%", Knob::Assignment::scene, false);
  knob("Position", 0.6, "60%", Knob::Assignment::expression, false);
  knob("Level", 0.5, "0.0 dB", Knob::Assignment::none, true);

  const auto footswitch = [this](const juce::String& letter, const juce::String& label, Footswitch::Look look,
                                 juce::Colour ring, bool blink) {
    auto f = std::make_unique<Footswitch>(letter);
    f->setLabel(label);
    f->setLook(look, ring);
    f->setBlinkOn(blink);
    addAndMakeVisible(*f);
    switches.push_back(std::move(f));
  };
  footswitch("A", "Drive", Footswitch::Look::off, colour::iris, false);
  footswitch("B", "Drive", Footswitch::Look::stomp, categoryColour(Category::drive), false);
  footswitch("C", "Lead", Footswitch::Look::active, colour::iris, false);
  footswitch("120", "Tap", Footswitch::Look::tap, colour::iris, true);

  segmented.setSelected(1);
  toggleOn.setToggleState(true, juce::dontSendNotification);
  meter.setLevel(-3.2f, -2.4f);
  for (juce::Component* c : std::initializer_list<juce::Component*>{&primary, &secondary, &ghost, &segmented,
                                                                     &toggleOn, &toggleOff, &meter})
    addAndMakeVisible(c);
}

void Gallery::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setFont(type::title());
  g.setColour(colour::text);
  g.drawText("Design system", 40, 24, 600, 34, juce::Justification::centredLeft);
  paint::label(g, "Block categories", {40, 72, 400, 16});
  paint::label(g, "Tile states", {40, 260, 400, 16});
  paint::label(g, "Knobs", {40, 400, 400, 16});
  paint::label(g, "Footswitches", {460, 400, 400, 16});
  paint::label(g, "Buttons and controls", {880, 400, 400, 16});
  paint::label(g, "Type", {40, 600, 400, 16});
  g.setFont(type::display());
  g.drawText("Glass Cleans", 40, 620, 400, 50, juce::Justification::centredLeft);
  g.setFont(type::sans(40.0f, type::Weight::medium, true));
  g.drawText("Glass Cleans", 420, 620, 400, 50, juce::Justification::centredLeft);
  g.setFont(type::heading());
  g.drawText("Brit 800 Crunch " + type::dot() + " NAM capture", 40, 676, 600, 28, juce::Justification::centredLeft);
  g.setFont(type::body());
  g.drawText("Plug the Volt line output 3 into the re-amp box, then into the amp input.", 40, 706, 800, 22,
             juce::Justification::centredLeft);
  paint::label(g, "Scene B " + type::dot() + " Stomp mode", {40, 732, 400, 18}, juce::Justification::centredLeft, colour::text);
  g.setFont(type::value());
  g.setColour(colour::text);
  g.drawText(juce::String::fromUTF8("-12.4 dB \xc2\xb7 120.0 BPM \xc2\xb7 48 kHz / 128 \xc2\xb7 CC 43"), 40, 756, 600, 20,
             juce::Justification::centredLeft);
}

void Gallery::resized() {
  int x = 40;
  for (size_t i = 0; i < categoryTiles.size(); ++i) {
    const int column = static_cast<int>(i % 6), row = static_cast<int>(i / 6);
    categoryTiles[i]->setBounds(40 + column * 116, 96 + row * 84, size::tileWidth, size::tileHeight);
  }
  for (size_t i = 0; i < stateTiles.size(); ++i)
    stateTiles[i]->setBounds(40 + static_cast<int>(i) * 116, 284, size::tileWidth, size::tileHeight);
  x = 40;
  for (auto& k : knobs) {
    k->setBounds(x, 424, 96, 110);
    x += 100;
  }
  x = 460;
  for (auto& f : switches) {
    f->setBounds(x, 424, 92, 120);
    x += 100;
  }
  primary.setBounds(880, 424, primary.idealWidth(), 32);
  secondary.setBounds(primary.getRight() + 12, 424, secondary.idealWidth(), 32);
  ghost.setBounds(secondary.getRight() + 12, 424, ghost.idealWidth(), 32);
  segmented.setBounds(880, 470, segmented.idealWidth(), 36);
  toggleOn.setBounds(880, 520, 44, 28);
  toggleOff.setBounds(932, 520, 44, 28);
  meter.setBounds(880, 562, 360, 10);
}

}  // namespace pedaleira::ui::testbed
