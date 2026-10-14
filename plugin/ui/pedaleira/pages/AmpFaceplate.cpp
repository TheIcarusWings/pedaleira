#include "AmpFaceplate.h"

#include "../Paint.h"
#include "../Type.h"

#include <algorithm>
#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kBoardPadX = 12;
constexpr int kBoxGap = 12;
constexpr int kBoxPadX = 12;
constexpr int kSwitchSpacing = 24;
// The knobs shrink to this, then the gaps around them do.
constexpr int kMinDiameter = 34;
constexpr int kComfortableDiameter = 44;
constexpr int kMaxDiameter = 64;
constexpr int kLevelDiameter = 30;
// The name, the value and the gap above them, under a knob.
constexpr int kKnobText = 4 + 18 + 18;

template <size_t N>
int indexOf(const char* const (&ids)[N], const juce::String& id, int fallback) {
  for (size_t i = 0; i < N; ++i)
    if (id == ids[i])
      return static_cast<int>(i);
  return fallback;
}

// The text on the dark ink or the light one, whichever reads on this colour.
juce::Colour inkOn(juce::Colour background) {
  return background.getPerceivedBrightness() > 0.55f ? colour::ground : colour::text;
}

// How the faceplate writes a knob's value: the amp's controls are 0 to 100
// percent, read as the 0 to 10 of the panel.
juce::String faceValueText(const KnobParam& param) {
  if (param.unit == "%")
    return juce::String(param.value / 10.0, 1);
  return param.text(param.value);
}

float textWidth(const juce::Font& font, const juce::String& text) {
  return juce::GlyphArrangement::getStringWidth(font, text);
}

}  // namespace

// ####################
// SPEC
// ####################
FaceplateSpec FaceplateSpec::fromVar(const juce::var& v, juce::Colour fallbackAccent) {
  static const char* const nameplates[] = {"left", "right", "top", "bottom"};
  static const char* const caps[] = {"round", "pointer", "chicken_head", "fader"};
  static const char* const strips[] = {"none",   "slash",   "dots",  "wave",  "cells",  "zigzag", "chevron", "bells",
                                       "arcs",   "crosses", "ticks", "flames", "rings", "steps",  "diamonds", "ripple"};
  FaceplateSpec spec;
  spec.name = v["name"].toString();
  spec.family = v["family"].toString();
  spec.tag = v["tag"].toString();
  spec.character = v["character"].toString();
  spec.styled = static_cast<bool>(v["styled"]);
  const juce::String accent = v["accent"].toString();
  spec.accent = accent.length() == 7 && accent[0] == '#' ? juce::Colour::fromString("ff" + accent.substring(1)) : fallbackAccent;
  spec.nameplate = static_cast<FaceNameplate>(indexOf(nameplates, v["nameplate"].toString(), 0));
  spec.cap = static_cast<Knob::Cap>(indexOf(caps, v["knobCap"].toString(), 0));
  spec.strip = static_cast<FaceStrip>(indexOf(strips, v["strip"].toString(), 0));
  if (const auto* names = v["voicings"].getArray())
    for (const auto& name : *names)
      spec.voicings.add(name.toString());
  if (const auto* groups = v["groups"].getArray())
    for (const auto& item : *groups) {
      FaceGroup group;
      group.title = item["title"].toString();
      group.columns = static_cast<int>(item["columns"]);
      if (const auto* controls = item["controls"].getArray())
        for (const auto& control : *controls)
          group.controls.push_back({control["id"].toString(), control["label"].toString()});
      spec.groups.push_back(std::move(group));
    }
  return spec;
}

// ####################
// STRIP PATTERNS
// ####################
// The signature strips of the canvas, drawn in a 22 px high band in ink: the
// same shapes, repeated to the width of the area.
void paintFaceStrip(juce::Graphics& g, FaceStrip pattern, juce::Rectangle<float> area, juce::Colour ink) {
  if (pattern == FaceStrip::none || area.isEmpty())
    return;
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(area.expanded(1.0f).toNearestInt());
  g.setColour(ink);
  const float sy = area.getHeight() / 22.0f;
  const float width = area.getWidth();
  const auto P = [&](float x, float y) { return juce::Point<float>(area.getX() + x, area.getY() + y * sy); };
  const juce::PathStrokeType stroke(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
  const auto line = [&](juce::Point<float> a, juce::Point<float> b) {
    juce::Path path;
    path.startNewSubPath(a);
    path.lineTo(b);
    g.strokePath(path, stroke);
  };
  const auto sine = [&](float amplitude, float period, float phase) {
    juce::Path path;
    for (float x = 0.0f; x <= width + 1.0f; x += 2.0f) {
      const auto at = P(x, 11.0f + amplitude * std::sin(juce::MathConstants<float>::twoPi * x / period + phase));
      x == 0.0f ? path.startNewSubPath(at) : path.lineTo(at);
    }
    g.strokePath(path, stroke);
  };
  static const float tickHeights[] = {14, 7, 17, 10, 5, 15, 9, 12};
  static const float flameTops[] = {14, 10, 6, 2, 6, 10};
  static const float stepHeights[] = {6, 12, 18, 10, 16, 8, 14};
  switch (pattern) {
    case FaceStrip::none: break;
    case FaceStrip::slash:
      for (float x = 4.0f; x < width + 18.0f; x += 10.0f)
        line(P(x, 20.0f), P(x + 18.0f, 2.0f));
      break;
    case FaceStrip::dots:
      for (float x = 6.0f; x < width; x += 12.0f)
        for (const float y : {5.0f, 15.0f})
          g.fillEllipse(juce::Rectangle<float>(5.2f, 5.2f * sy).withCentre(P(x, y)));
      break;
    case FaceStrip::wave: sine(8.0f, 66.0f, 0.0f); break;
    case FaceStrip::ripple:
      sine(5.0f, 56.0f, 0.0f);
      sine(5.0f, 58.0f, juce::MathConstants<float>::pi);
      break;
    case FaceStrip::cells:
      for (float x = 2.0f; x + 10.0f <= width; x += 14.0f)
        for (const float y : {1.0f, 11.0f})
          g.drawRoundedRectangle(juce::Rectangle<float>(P(x, y).x, P(x, y).y, 10.0f, 8.0f * sy), 1.5f, 2.0f);
      break;
    case FaceStrip::zigzag: {
      juce::Path path;
      bool down = true;
      for (float x = 0.0f; x <= width + 10.0f; x += 10.0f, down = !down)
        x == 0.0f ? path.startNewSubPath(P(x, 3.0f)) : path.lineTo(P(x, down ? 3.0f : 19.0f));
      g.strokePath(path, stroke);
      break;
    }
    case FaceStrip::chevron:
      for (float x = 2.0f; x < width; x += 12.0f) {
        juce::Path path;
        path.startNewSubPath(P(x, 3.0f));
        path.lineTo(P(x + 7.0f, 11.0f));
        path.lineTo(P(x, 19.0f));
        g.strokePath(path, stroke);
      }
      break;
    case FaceStrip::bells:
      for (float x = 0.0f; x < width; x += 26.0f) {
        juce::Path bell;
        bell.startNewSubPath(P(x + 13.0f, 3.0f));
        bell.cubicTo(P(x + 9.0f, 3.0f), P(x + 7.0f, 6.0f), P(x + 7.0f, 10.0f));
        bell.lineTo(P(x + 7.0f, 14.0f));
        bell.lineTo(P(x + 4.0f, 17.0f));
        bell.lineTo(P(x + 22.0f, 17.0f));
        bell.lineTo(P(x + 19.0f, 14.0f));
        bell.lineTo(P(x + 19.0f, 10.0f));
        bell.cubicTo(P(x + 19.0f, 6.0f), P(x + 17.0f, 3.0f), P(x + 13.0f, 3.0f));
        bell.closeSubPath();
        g.strokePath(bell, stroke);
      }
      break;
    case FaceStrip::arcs:
      // Five nested quarter circles, then again.
      for (float set = 0.0f; set < width; set += 40.0f)
        for (int i = 0; i < 5; ++i) {
          const float r = 18.0f - 2.0f * static_cast<float>(i), x = set + 2.0f + 6.0f * static_cast<float>(i);
          juce::Path arc;
          const auto centre = P(x + r, 20.0f);
          arc.addCentredArc(centre.x, centre.y, r, r * sy, 0.0f, -juce::MathConstants<float>::halfPi, 0.0f, true);
          g.strokePath(arc, stroke);
        }
      break;
    case FaceStrip::crosses:
      for (float x = 8.0f; x < width; x += 16.0f) {
        line(P(x, 3.0f), P(x, 19.0f));
        line(P(x - 5.0f, 11.0f), P(x + 5.0f, 11.0f));
      }
      break;
    case FaceStrip::ticks: {
      int i = 0;
      for (float x = 3.0f; x < width; x += 8.0f, ++i)
        line(P(x, 22.0f), P(x, 22.0f - tickHeights[i % 8]));
      break;
    }
    case FaceStrip::flames: {
      int i = 0;
      for (float x = 0.0f; x < width; x += 16.0f, ++i) {
        juce::Path flame;
        flame.startNewSubPath(P(x + 8.0f, flameTops[i % 6]));
        flame.lineTo(P(x + 15.0f, 20.0f));
        flame.lineTo(P(x + 1.0f, 20.0f));
        flame.closeSubPath();
        g.fillPath(flame);
      }
      break;
    }
    case FaceStrip::rings:
      for (float x = 11.0f; x < width; x += 24.0f)
        g.drawEllipse(juce::Rectangle<float>(18.0f, 18.0f * sy).withCentre(P(x, 11.0f)), 2.0f);
      break;
    case FaceStrip::steps: {
      int i = 0;
      for (float x = 2.0f; x < width; x += 9.0f, ++i) {
        const float h = stepHeights[i % 7];
        g.fillRoundedRectangle(juce::Rectangle<float>(P(x, 22.0f - h).x, P(x, 22.0f - h).y, 5.0f, h * sy), 1.5f);
      }
      break;
    }
    case FaceStrip::diamonds:
      for (float x = 1.0f; x < width; x += 18.0f) {
        juce::Path diamond;
        diamond.startNewSubPath(P(x + 8.0f, 2.0f));
        diamond.lineTo(P(x + 16.0f, 11.0f));
        diamond.lineTo(P(x + 8.0f, 20.0f));
        diamond.lineTo(P(x, 11.0f));
        diamond.closeSubPath();
        g.strokePath(diamond, stroke);
      }
      break;
  }
}

// ####################
// BRIGHT SWITCH
// ####################
FaceBrightSwitch::FaceBrightSwitch(const juce::String& text, juce::Colour accentColour)
    : juce::Button(text), label(text), accent(accentColour) {
  setClickingTogglesState(true);
  setTitle(text);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

int FaceBrightSwitch::idealWidth() const { return 40 + 8 + juce::roundToInt(textWidth(type::sans(13.0f), label)) + 2; }

void FaceBrightSwitch::paintButton(juce::Graphics& g, bool highlighted, bool) {
  const bool on = getToggleState();
  const auto area = getLocalBounds().toFloat();
  const float lift = highlighted ? 0.08f : 0.0f;
  // 40 x 22, radius 11, an 18 px thumb with a 2 px inset: the accent track and a
  // dark thumb on the right when on, a grey track and a muted thumb on the left when off.
  const auto track = juce::Rectangle<float>(40.0f, 22.0f).withCentre({20.0f, area.getCentreY()});
  g.setColour((on ? accent : colour::line).brighter(lift));
  g.fillRoundedRectangle(track, 11.0f);
  const auto thumb = juce::Rectangle<float>(18.0f, 18.0f).withCentre({on ? track.getRight() - 11.0f : track.getX() + 11.0f, area.getCentreY()});
  g.setColour(on ? colour::ground : colour::muted);
  g.fillEllipse(thumb);
  g.setFont(type::sans(13.0f));
  g.setColour(colour::text);
  g.drawText(label, juce::Rectangle<float>(48.0f, area.getY(), area.getRight() - 48.0f, area.getHeight()), juce::Justification::centredLeft,
             false);
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(area.reduced(0.5f), 6.0f, 1.5f);
  }
}

// ####################
// VOICING SWITCH
// ####################
namespace {
const char* const kRoman[] = {"I", "II", "III"};
constexpr int kVoicingLabel = 56;
}  // namespace

FaceVoicingSwitch::FaceVoicingSwitch(const juce::StringArray& voicingNames, juce::Colour) : names(voicingNames) {
  setTitle("Voicing");
  setWantsKeyboardFocus(true);
  setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void FaceVoicingSwitch::setSelected(int index, juce::NotificationType notification) {
  index = juce::jlimit(0, juce::jmax(0, names.size() - 1), index);
  if (index == selected)
    return;
  selected = index;
  repaint();
  if (notification != juce::dontSendNotification && onChange)
    onChange(selected);
}

// The label, then a container with 3 px of padding around segments 34 wide.
int FaceVoicingSwitch::idealWidth() const { return kVoicingLabel + 6 + 34 * names.size() + 2; }

// Where segment `index` is, in this component: 26 high, 34 wide.
juce::Rectangle<float> FaceVoicingSwitch::segment(int index) const {
  return {static_cast<float>(kVoicingLabel) + 3.0f + 34.0f * static_cast<float>(index), getHeight() * 0.5f - 13.0f, 34.0f, 26.0f};
}

int FaceVoicingSwitch::segmentAt(juce::Point<float> point) const {
  for (int i = 0; i < names.size(); ++i)
    if (segment(i).expanded(0.0f, 6.0f).contains(point))
      return i;
  return -1;
}

juce::String FaceVoicingSwitch::getTooltip() { return hovered >= 0 && hovered < names.size() ? names[hovered] : juce::String(); }

void FaceVoicingSwitch::paint(juce::Graphics& g) {
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText("Voicing", 0, 0, kVoicingLabel - 6, getHeight(), juce::Justification::centredLeft, false);
  const int n = names.size();
  if (n == 0)
    return;
  // The container: radius 10, #181B1F, a 1 px #323841 border.
  const auto container = segment(0).getUnion(segment(n - 1)).expanded(3.0f);
  g.setColour(colour::surface);
  g.fillRoundedRectangle(container, 10.0f);
  g.setColour(colour::line);
  g.drawRoundedRectangle(container.reduced(0.5f), 10.0f, 1.0f);
  for (int i = 0; i < n; ++i) {
    const auto at = segment(i);
    const bool on = i == selected;
    if (on) {
      g.setColour(colour::control);
      g.fillRoundedRectangle(at, 7.0f);
    }
    g.setFont(type::mono(13.0f, type::Weight::semibold));
    g.setColour(on ? colour::text : colour::muted.brighter(i == hovered ? 0.2f : 0.0f));
    g.drawText(kRoman[juce::jmin(i, 2)], at, juce::Justification::centred, false);
  }
  if (hasKeyboardFocus(false)) {
    g.setColour(colour::iris);
    g.drawRoundedRectangle(getLocalBounds().toFloat().withTrimmedLeft(static_cast<float>(kVoicingLabel - 4)).reduced(0.5f), 6.0f, 1.5f);
  }
}

void FaceVoicingSwitch::mouseUp(const juce::MouseEvent& event) {
  const int at = segmentAt(event.position);
  if (at >= 0)
    setSelected(at, juce::sendNotification);
}

void FaceVoicingSwitch::mouseMove(const juce::MouseEvent& event) {
  const int at = segmentAt(event.position);
  if (at != hovered) {
    hovered = at;
    repaint();
  }
}

void FaceVoicingSwitch::mouseExit(const juce::MouseEvent&) {
  if (hovered != -1) {
    hovered = -1;
    repaint();
  }
}

bool FaceVoicingSwitch::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::leftKey || key == juce::KeyPress::downKey) {
    setSelected(selected - 1, juce::sendNotification);
    return true;
  }
  if (key == juce::KeyPress::rightKey || key == juce::KeyPress::upKey) {
    setSelected(selected + 1, juce::sendNotification);
    return true;
  }
  return false;
}

std::unique_ptr<juce::AccessibilityHandler> FaceVoicingSwitch::createAccessibilityHandler() {
  return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group);
}

// ####################
// FACEPLATE
// ####################
AmpFaceplate::AmpFaceplate(EngineClient& client) : BlockEditorBody(client) { setTitle("Amp faceplate"); }

AmpFaceplate::~AmpFaceplate() = default;

const KnobParam* AmpFaceplate::find(const juce::String& id) const {
  for (const auto& spec : lastSpecs)
    if (spec.id == id)
      return &spec;
  return nullptr;
}

std::optional<double> AmpFaceplate::shownValue(const juce::String& paramId) const {
  if (const KnobParam* spec = find(paramId))
    return spec->value;
  return std::nullopt;
}

juce::Component* AmpFaceplate::controlFor(const juce::String& paramId) const {
  if (const auto found = knobs.find(paramId); found != knobs.end())
    return found->second;
  if (paramId == "bright")
    return brightSwitch;
  if (paramId == "voicing")
    return voicingSwitch;
  if (paramId == "levelDb")
    return level.get();
  return nullptr;
}

juce::Rectangle<int> AmpFaceplate::groupBounds(int index) const {
  return index >= 0 && index < static_cast<int>(boxes.size()) ? boxes[static_cast<size_t>(index)].bounds : juce::Rectangle<int>();
}

juce::String AmpFaceplate::bubbleText() const {
  for (const auto& [id, knob] : knobs)
    if (knob->isHot())
      return knob->getName() + " " + knob->getValueText();
  if (level != nullptr && level->isHot())
    return level->getName() + " " + level->getValueText();
  return {};
}

void AmpFaceplate::noteValue(const juce::String& id, double value) {
  for (auto& spec : lastSpecs)
    if (spec.id == id)
      spec.value = value;
}

void AmpFaceplate::setParam(const juce::String& id, double value) {
  noteValue(id, value);
  engine.run("block.setParam", object({{"blockId", context.blockId}, {"param", id}, {"value", value}}));
}

ParamKnob* AmpFaceplate::makeKnob(const KnobParam& param, int d) {
  auto knob = std::make_unique<ParamKnob>(engine, context.blockId, param);
  knob->setDiameter(d);
  knob->setCap(plate.cap);
  knob->setAccent(plate.accent);
  knob->setTagShown(false);
  knob->setBypassed(!context.enabled);
  knob->valueText = faceValueText;
  knob->update(param);
  knob->onAssignmentChanged = [this] {
    if (onChanged)
      onChanged();
  };
  knob->onValueChanged = [this](const KnobParam& p) { noteValue(p.id, p.value); };
  knob->onHotChanged = [this] { repaint(); };
  addAndMakeVisible(*knob);
  ParamKnob* raw = knob.get();
  owned.push_back(std::move(knob));
  return raw;
}

void AmpFaceplate::showBlock(const BlockContext& block) {
  context = block;
  const juce::var listed = engine.read("block.listParams", object({{"blockId", block.blockId}}));
  juce::var face = engine.read("block.getFaceplate", object({{"blockId", block.blockId}}));
  std::vector<KnobParam> specs;
  juce::String ids = block.blockId;
  if (const auto* list = listed["params"].getArray())
    for (const auto& item : *list) {
      KnobParam p = KnobParam::fromVar(item);
      p.sceneAssigned = block.sceneParams.contains(p.id);
      p.expressionAssigned = block.expressionParams.contains(p.id);
      ids << "|" << p.id;
      specs.push_back(p);
    }
  // What the faceplate depends on, apart from the revision it was read at.
  if (auto* object = face.getDynamicObject()) {
    object->removeProperty("revision");
    object->removeProperty("blockId");
  }
  const juce::String next = juce::JSON::toString(face, true) + ids;
  lastSpecs = specs;
  if (next != signature) {
    signature = next;
    plate = FaceplateSpec::fromVar(face, categoryColour(Category::amp));
    rebuild(specs);
  } else {
    updateValues(specs);
  }
}

void AmpFaceplate::rebuild(const std::vector<KnobParam>& specs) {
  juce::ignoreUnused(specs);
  owned.clear();
  knobs.clear();
  boxes.clear();
  level.reset();
  brightSwitch = nullptr;
  voicingSwitch = nullptr;

  for (const FaceGroup& group : plate.groups) {
    Box box;
    box.title = group.title;
    box.columns = group.columns;
    for (const FaceControl& control : group.controls) {
      const KnobParam* param = find(control.id);
      if (param == nullptr)
        continue;
      if (control.id == "bright") {
        auto toggle = std::make_unique<FaceBrightSwitch>(control.label.isNotEmpty() ? control.label : param->name, plate.accent);
        toggle->setToggleState(param->value >= 0.5, juce::dontSendNotification);
        FaceBrightSwitch* raw = toggle.get();
        toggle->onClick = [this, raw] { setParam("bright", raw->getToggleState() ? 1.0 : 0.0); };
        addAndMakeVisible(*toggle);
        brightSwitch = raw;
        box.switchItems.push_back(raw);
        owned.push_back(std::move(toggle));
      } else if (control.id == "voicing") {
        // One voicing is nothing to switch between.
        if (plate.voicings.size() < 2)
          continue;
        auto voices = std::make_unique<FaceVoicingSwitch>(plate.voicings, plate.accent);
        voices->setSelected(juce::roundToInt(param->value));
        FaceVoicingSwitch* raw = voices.get();
        voices->onChange = [this](int index) { setParam("voicing", index); };
        addAndMakeVisible(*voices);
        voicingSwitch = raw;
        box.switchItems.push_back(raw);
        owned.push_back(std::move(voices));
      } else if (param->type == "float") {
        KnobParam labelled = *param;
        if (control.label.isNotEmpty())
          labelled.name = control.label;
        ParamKnob* knob = makeKnob(labelled, diameter);
        knobs[control.id] = knob;
        box.knobItems.push_back(knob);
      }
    }
    if (!box.knobItems.empty() || !box.switchItems.empty())
      boxes.push_back(std::move(box));
  }

  // The output level, in the strip: a knob with no text of its own.
  if (const KnobParam* param = find("levelDb")) {
    KnobParam labelled = *param;
    labelled.name = "Level";
    level = std::make_unique<ParamKnob>(engine, context.blockId, labelled);
    level->setDiameter(kLevelDiameter);
    level->setAccent(plate.accent);
    level->setTextShown(false);
    level->setBypassed(!context.enabled);
    level->update(labelled);
    level->onAssignmentChanged = [this] {
      if (onChanged)
        onChanged();
    };
    level->onValueChanged = [this](const KnobParam& p) {
      noteValue(p.id, p.value);
      repaint(strip);
    };
    level->onHotChanged = [this] { repaint(); };
    addAndMakeVisible(*level);
  }
  layoutBoard();
  repaint();
}

void AmpFaceplate::updateValues(const std::vector<KnobParam>& specs) {
  for (const auto& [id, knob] : knobs)
    for (const auto& spec : specs)
      if (spec.id == id) {
        KnobParam labelled = spec;
        labelled.name = knob->getName();
        knob->update(labelled);
        knob->setBypassed(!context.enabled);
      }
  if (level != nullptr)
    for (const auto& spec : specs)
      if (spec.id == "levelDb") {
        KnobParam labelled = spec;
        labelled.name = "Level";
        level->update(labelled);
        level->setBypassed(!context.enabled);
      }
  for (const auto& spec : specs) {
    if (brightSwitch != nullptr && spec.id == "bright")
      brightSwitch->setToggleState(spec.value >= 0.5, juce::dontSendNotification);
    if (voicingSwitch != nullptr && spec.id == "voicing")
      voicingSwitch->setSelected(juce::roundToInt(spec.value));
  }
  const float alpha = context.enabled ? 1.0f : 0.6f;
  for (juce::Component* c : {static_cast<juce::Component*>(brightSwitch), static_cast<juce::Component*>(voicingSwitch)})
    if (c != nullptr)
      c->setAlpha(alpha);
  repaint();
}

void AmpFaceplate::setLive(const BlockLive& next) {
  const auto sag = [](const BlockLive& l) {
    const auto it = l.readings.find("sag");
    return it != l.readings.end() ? it->second : 0.0;
  };
  const bool seen = !next.readings.empty();
  const bool changed = seen != liveSeen || std::abs(sag(next) - sag(live)) > 0.004;
  live = next;
  liveSeen = seen;
  if (changed)
    repaint(strip);
}

// ####################
// LAYOUT
// ####################
int AmpFaceplate::switchWidth(const juce::Component* item) const {
  if (const auto* bright = dynamic_cast<const FaceBrightSwitch*>(item))
    return bright->idealWidth();
  if (const auto* voices = dynamic_cast<const FaceVoicingSwitch*>(item))
    return voices->idealWidth();
  return 60;
}

void AmpFaceplate::resized() { layoutBoard(); }

void AmpFaceplate::layoutBoard() {
  auto area = getLocalBounds();
  strip = area.removeFromBottom(kStripHeight);
  board = area;
  const bool banner = plate.nameplate == FaceNameplate::top || plate.nameplate == FaceNameplate::bottom;
  // How big the knobs can be with a set of gaps: the room under the title and
  // above the switches, shared by the rows of knobs of the box that has the
  // fewest. The normal gaps unless they leave the knobs small.
  const bool fader = plate.cap == Knob::Cap::fader;
  const auto fit = [&](const Metrics& m) {
    const int boardRoom = board.getHeight() - 2 * m.padY - (banner ? m.banner + m.bannerGap : 0);
    int best = kMaxDiameter;
    for (const Box& box : boxes) {
      if (box.knobItems.empty())
        continue;
      const int cols = box.columns > 0 ? box.columns : static_cast<int>(box.knobItems.size());
      const int rows = (static_cast<int>(box.knobItems.size()) + cols - 1) / cols;
      const int room = boardRoom - m.boxTop - m.title - m.titleGap - m.boxBottom - (box.switchItems.empty() ? 0 : m.switchRow + m.switchGap);
      const int perRow = room / rows - kKnobText;
      best = juce::jmin(best, fader ? juce::roundToInt(static_cast<float>(perRow) / 1.7f) : perRow);
    }
    return best;
  };
  // The cards are flat and padded 10 / 12 / 12: top, sides, bottom.
  const Metrics normal{10, 48, 8, 10, 16, 6, 12, 32, 6};
  const Metrics tight{5, 40, 6, 8, 14, 3, 6, 28, 4};
  compact = fit(normal) < kComfortableDiameter && fit(tight) > fit(normal);
  metrics = compact ? tight : normal;
  auto inner = board.reduced(kBoardPadX, metrics.padY);
  plateBounds = {};
  const int sideWidth = juce::jlimit(170, 250, juce::roundToInt(static_cast<float>(getWidth()) * 0.18f));
  switch (plate.nameplate) {
    case FaceNameplate::left:
      plateBounds = inner.removeFromLeft(sideWidth);
      inner.removeFromLeft(kBoxGap);
      break;
    case FaceNameplate::right:
      plateBounds = inner.removeFromRight(sideWidth);
      inner.removeFromRight(kBoxGap);
      break;
    case FaceNameplate::top:
      plateBounds = inner.removeFromTop(metrics.banner);
      inner.removeFromTop(metrics.bannerGap);
      break;
    case FaceNameplate::bottom:
      plateBounds = inner.removeFromBottom(metrics.banner);
      inner.removeFromBottom(metrics.bannerGap);
      break;
  }

  // The level knob in the strip.
  if (level != nullptr) {
    const int x = 20 + juce::roundToInt(textWidth(type::label(), "OUTPUT")) + 14;
    level->setBounds(juce::Rectangle<int>(34, 34).withPosition(x, strip.getCentreY() - 17));
    levelLabel = {level->getRight() + 8, strip.getY(), 150, strip.getHeight()};
  }
  if (boxes.empty() || inner.getWidth() < 40 || inner.getHeight() < 40)
    return;

  const int count = static_cast<int>(boxes.size());
  diameter = juce::jlimit(kMinDiameter, kMaxDiameter, fit(metrics));

  // How wide each box needs to be, and the knobs shrink until they all fit.
  std::vector<int> need(boxes.size());
  const int room = inner.getWidth() - kBoxGap * (count - 1);
  for (;;) {
    int total = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
      const Box& box = boxes[i];
      const int cols = box.columns > 0 ? box.columns : static_cast<int>(box.knobItems.size());
      int switches = 0;
      for (const juce::Component* item : box.switchItems)
        switches += switchWidth(item) + (switches > 0 ? kSwitchSpacing : 0);
      need[i] = juce::jmax(cols * (diameter + 26), switches, 90) + 2 * kBoxPadX;
      total += need[i];
    }
    if (total <= room || diameter <= kMinDiameter)
      break;
    diameter -= 2;
  }
  int needTotal = 0;
  for (const int n : need)
    needTotal += n;

  int x = inner.getX();
  for (size_t i = 0; i < boxes.size(); ++i) {
    Box& box = boxes[i];
    const bool lastBox = i + 1 == boxes.size();
    const int width = lastBox ? inner.getRight() - x : juce::roundToInt(static_cast<float>(room) * static_cast<float>(need[i]) / static_cast<float>(needTotal));
    box.bounds = {x, inner.getY(), width, inner.getHeight()};
    x += width + kBoxGap;

    auto content = box.bounds.reduced(kBoxPadX, 0).withTrimmedTop(metrics.boxTop + metrics.title + metrics.titleGap).withTrimmedBottom(metrics.boxBottom);
    if (!box.switchItems.empty()) {
      auto row = content.removeFromBottom(metrics.switchRow);
      content.removeFromBottom(metrics.switchGap);
      int sx = row.getX();
      for (juce::Component* item : box.switchItems) {
        const int w = switchWidth(item);
        item->setBounds(sx, row.getY(), w, metrics.switchRow);
        sx += w + kSwitchSpacing;
      }
    }
    if (box.knobItems.empty())
      continue;
    const int cols = box.columns > 0 ? box.columns : static_cast<int>(box.knobItems.size());
    const int rows = (static_cast<int>(box.knobItems.size()) + cols - 1) / cols;
    const int cellWidth = content.getWidth() / cols;
    const int cellHeight = Knob::cellHeight(plate.cap, diameter, false);
    // The knobs sit compact near the top of the box (the switches stay at the
    // bottom), however tall the panel is; they do not float in the middle.
    const int top = content.getY() + 6;
    for (size_t k = 0; k < box.knobItems.size(); ++k) {
      ParamKnob* knob = box.knobItems[k];
      knob->setDiameter(diameter);
      knob->setBounds(content.getX() + static_cast<int>(k % static_cast<size_t>(cols)) * cellWidth,
                      top + static_cast<int>(k / static_cast<size_t>(cols)) * cellHeight, cellWidth, cellHeight);
    }
  }
}

// ####################
// PAINT
// ####################
juce::Colour AmpFaceplate::boardColour() const { return juce::Colour(0xff121417).interpolatedWith(plate.accent, 0.06f); }

void AmpFaceplate::paint(juce::Graphics& g) {
  if (plate.groups.empty())
    return;
  g.setColour(boardColour());
  g.fillRoundedRectangle(board.toFloat(), size::panelRadius);
  for (const Box& box : boxes) {
    // A flat card: #101214, radius 10, no border and no rule.
    g.setColour(colour::ground);
    g.fillRoundedRectangle(box.bounds.toFloat(), size::panelRadius);
    paint::label(g, box.title,
                 juce::Rectangle<int>(box.bounds.getX() + kBoxPadX, box.bounds.getY() + metrics.boxTop,
                                      box.bounds.getWidth() - 2 * kBoxPadX, metrics.title),
                 juce::Justification::centredLeft, plate.accent);
  }
  paintNameplate(g);
  paintStrip(g);
}

void AmpFaceplate::paintNameplate(juce::Graphics& g) {
  if (plateBounds.isEmpty())
    return;
  const auto card = plateBounds.toFloat();
  g.setColour(plate.accent);
  g.fillRoundedRectangle(card, size::panelRadius);
  const juce::Colour ink = inkOn(plate.accent);
  const auto chip = [&](juce::Rectangle<float> at) {
    g.setColour(colour::ground);
    g.fillRoundedRectangle(at, 11.0f);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(plate.accent);
    g.drawText(plate.tag, at, juce::Justification::centred, false);
  };
  const float chipWidth = plate.tag.isEmpty() ? 0.0f : textWidth(type::sans(12.0f, type::Weight::semibold), plate.tag) + 20.0f;

  if (plate.nameplate == FaceNameplate::top || plate.nameplate == FaceNameplate::bottom) {
    // A banner: the strip, the name, the tag, the character line, the strip.
    const float stripH = 22.0f;
    auto area = card.reduced(20.0f, 0.0f);
    const float leftStrip = std::min(120.0f, area.getWidth() * 0.12f);
    paintFaceStrip(g, plate.strip, area.removeFromLeft(leftStrip).withSizeKeepingCentre(leftStrip, stripH), ink.withAlpha(0.7f));
    area.removeFromLeft(20.0f);
    const juce::Font nameFont = type::sans(28.0f, type::Weight::bold);
    const float nameWidth = textWidth(nameFont, plate.name);
    g.setFont(nameFont);
    g.setColour(ink);
    g.drawText(plate.name, area.removeFromLeft(nameWidth + 4.0f), juce::Justification::centredLeft, false);
    area.removeFromLeft(14.0f);
    if (chipWidth > 0.0f) {
      chip(area.removeFromLeft(chipWidth).withSizeKeepingCentre(chipWidth, 22.0f));
      area.removeFromLeft(14.0f);
    }
    const float characterWidth = textWidth(type::sans(13.0f, type::Weight::medium), plate.character);
    const float rightStrip = juce::jlimit(0.0f, 260.0f, area.getWidth() - characterWidth - 24.0f);
    g.setFont(type::sans(13.0f, type::Weight::medium));
    g.setColour(ink.withAlpha(0.9f));
    g.drawText(plate.character, area.withTrimmedRight(rightStrip > 0.0f ? rightStrip + 20.0f : 0.0f), juce::Justification::centredLeft,
               true);
    if (rightStrip > 60.0f)
      paintFaceStrip(g, plate.strip, area.removeFromRight(rightStrip).withSizeKeepingCentre(rightStrip, stripH), ink.withAlpha(0.45f));
    return;
  }

  // A card: a strip at the top and the bottom, the tag, the name and the
  // character line between them, low on the card.
  const float pad = 16.0f;
  const float stripH = 22.0f;
  const float inside = card.getWidth() - 2.0f * pad;
  paintFaceStrip(g, plate.strip, juce::Rectangle<float>(card.getX() + pad, card.getY() + 14.0f, std::min(120.0f, inside), stripH), ink.withAlpha(0.7f));
  const auto bottomStrip = juce::Rectangle<float>(card.getX() + pad, card.getBottom() - 14.0f - stripH, std::min(226.0f, inside), stripH);
  paintFaceStrip(g, plate.strip, bottomStrip, ink.withAlpha(0.45f));
  float y = bottomStrip.getY() - 8.0f - 34.0f - 4.0f - 32.0f - 4.0f - 22.0f;
  if (chipWidth > 0.0f)
    chip(juce::Rectangle<float>(card.getX() + pad, y, chipWidth, 22.0f));
  y += 22.0f + 4.0f;
  // The name at 30 px, smaller if it would not fit.
  float nameSize = 30.0f;
  while (nameSize > 18.0f && textWidth(type::sans(nameSize, type::Weight::bold), plate.name) > inside)
    nameSize -= 1.0f;
  g.setFont(type::sans(nameSize, type::Weight::bold));
  g.setColour(ink);
  g.drawText(plate.name, juce::Rectangle<float>(card.getX() + pad, y, inside, 32.0f), juce::Justification::centredLeft, false);
  y += 32.0f + 4.0f;
  g.setFont(type::sans(13.0f, type::Weight::medium));
  g.setColour(ink.withAlpha(0.9f));
  g.drawFittedText(plate.character, juce::Rectangle<float>(card.getX() + pad, y, inside, 34.0f).toNearestInt(), juce::Justification::topLeft, 2,
                   1.0f);
}

void AmpFaceplate::paintStrip(juce::Graphics& g) {
  g.setColour(colour::line);
  g.fillRect(strip.getX(), strip.getY(), strip.getWidth(), 1);
  const int centreY = strip.getCentreY();
  paint::label(g, "Output", juce::Rectangle<int>(20, centreY - 7, 64, 14));
  if (level != nullptr) {
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(colour::text);
    g.drawText("Level", levelLabel.getX(), levelLabel.getY(), 40, levelLabel.getHeight(), juce::Justification::centredLeft, false);
    g.setFont(type::mono(12.0f));
    g.setColour(colour::muted);
    g.drawText(level->getValueText(), levelLabel.getX() + 42, levelLabel.getY(), 80, levelLabel.getHeight(), juce::Justification::centredLeft,
               false);
  }

  // The sag, from the right: Live, the percentage, the bar, its label.
  const auto sagIt = live.readings.find("sag");
  const double sag = sagIt != live.readings.end() ? juce::jlimit(0.0, 1.0, sagIt->second) : 0.0;
  int right = strip.getRight() - 20;
  const juce::Colour liveColour = liveSeen ? colour::ok : colour::faint;
  g.setFont(type::sans(11.0f));
  g.setColour(liveColour);
  const int liveWidth = juce::roundToInt(textWidth(type::sans(11.0f), "Live")) + 14;
  g.drawText("Live", right - liveWidth + 14, centreY - 8, liveWidth + 6, 16, juce::Justification::centredLeft, false);
  g.fillEllipse(static_cast<float>(right - liveWidth), static_cast<float>(centreY) - 3.5f, 7.0f, 7.0f);
  right -= liveWidth + 12;
  g.setFont(type::mono(12.0f));
  g.setColour(colour::text);
  g.drawText(juce::String(juce::roundToInt(sag * 100.0)) + "%", right - 36, centreY - 8, 36, 16, juce::Justification::centredRight, false);
  right -= 36 + 12;
  const auto bar = juce::Rectangle<float>(static_cast<float>(right - 140), static_cast<float>(centreY) - 3.0f, 140.0f, 6.0f);
  g.setColour(juce::Colour(0xff0b0c0e));
  g.fillRoundedRectangle(bar, 1.0f);
  {
    // Dashes of green up to the sag.
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(bar.withWidth(bar.getWidth() * static_cast<float>(sag)).toNearestInt());
    g.setColour(colour::ok);
    for (float x = bar.getX(); x < bar.getRight(); x += 7.0f)
      g.fillRect(juce::Rectangle<float>(x, bar.getY(), 5.0f, bar.getHeight()));
  }
  paint::label(g, "Sag", juce::Rectangle<int>(right - 140 - 12 - 32, centreY - 7, 32, 14), juce::Justification::centredRight);

  // What to do, between the level and the sag.
  const int from = levelLabel.getX() + 140, to = right - 140 - 12 - 32 - 24;
  if (to - from > 200) {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("The faceplate is the control surface. Hover or drag a knob to read its value.", from, strip.getY(), to - from, strip.getHeight(),
               juce::Justification::centred, true);
  }
}

void AmpFaceplate::paintOverChildren(juce::Graphics& g) {
  // The value of the knob under the mouse, in a bubble over it.
  const ParamKnob* hot = nullptr;
  for (const auto& [id, knob] : knobs)
    if (knob->isHot() && (hot == nullptr || knob->isMouseButtonDown()))
      hot = knob;
  if (hot == nullptr && level != nullptr && level->isHot())
    hot = level.get();
  if (hot == nullptr)
    return;
  const juce::String text = hot->getName() + " " + hot->getValueText();
  const float width = textWidth(type::mono(12.0f), text) + 20.0f;
  auto bubble = juce::Rectangle<float>(width, 24.0f);
  bubble.setCentre(static_cast<float>(hot->getBounds().getCentreX()), static_cast<float>(hot->getY()) - 4.0f);
  if (hot == level.get())
    bubble.setCentre(static_cast<float>(hot->getBounds().getCentreX()) + width * 0.5f, static_cast<float>(hot->getY()) - 6.0f);
  bubble = bubble.withX(juce::jlimit(2.0f, static_cast<float>(getWidth()) - width - 2.0f, bubble.getX()));
  if (bubble.getY() < 2.0f)
    bubble.setY(2.0f);
  g.setColour(colour::drop.withAlpha(0.5f));
  g.fillRoundedRectangle(bubble.translated(0.0f, 4.0f), 6.0f);
  g.setColour(colour::ground);
  g.fillRoundedRectangle(bubble, 6.0f);
  g.setColour(colour::line);
  g.drawRoundedRectangle(bubble.reduced(0.5f), 6.0f, 1.0f);
  g.setFont(type::mono(12.0f));
  g.setColour(colour::text);
  g.drawText(text, bubble, juce::Justification::centred, false);
}

}  // namespace pedaleira::ui
