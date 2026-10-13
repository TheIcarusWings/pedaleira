#include "BlockStyle.h"

#include "../Paint.h"
#include "../Type.h"
#include "TempoNotes.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

struct GroupRule {
  const char* group;
  std::vector<const char*> keywords;
};

bool hasAny(const juce::String& lowerId, const std::vector<const char*>& keywords) {
  for (const char* keyword : keywords)
    if (lowerId.contains(keyword))
      return true;
  return false;
}

const std::vector<GroupRule>& rulesFor(Category category) {
  static const std::vector<GroupRule> delay{{"Feedback", {"feedback", "cross", "spread", "ratio", "pattern", "taps", "diffusion"}},
                                            {"Modulation", {"wow", "flutter", "wobble", "mod"}},
                                            {"Tone", {"tone", "cut", "grit", "saturation", "age"}}};
  static const std::vector<GroupRule> reverb{{"Space", {"decay", "size", "predelay", "diffusion", "spring", "dwell", "interval"}},
                                             {"Modulation", {"modulation", "shimmer"}},
                                             {"Tone", {"damping", "tone", "cut", "bass", "treble"}}};
  static const std::vector<GroupRule> modulation{{"Tone", {"tone", "cut"}},
                                                 {"Shape", {"depth", "width", "voice", "stage", "cent", "manual", "shape", "pan",
                                                            "speed", "horn", "drive", "rise", "feedback", "pattern", "step"}}};
  static const std::vector<GroupRule> dynamics{{"Threshold", {"threshold", "ratio", "knee", "ceiling", "sustain", "peak", "input",
                                                              "range", "hysteresis", "mode", "detector", "colour"}},
                                               {"Tone", {"tone"}},
                                               {"Timing", {"attack", "release", "holdms"}}};
  static const std::vector<GroupRule> drive{{"Tone", {"tone", "bass", "middle", "treble", "presence", "depth", "shelf"}},
                                            {"Drive", {"drive", "gain", "fuzz", "bias", "distortion", "saturation", "sustain",
                                                       "bright", "voicing", "tight", "boost", "sub"}}};
  static const std::vector<GroupRule> filter{{"Timing", {"attack", "release"}},
                                             {"Sweep", {"position", "cent", "low", "range", "direction", "sensitivity", "depth",
                                                        "shape", "resonance", "voicing", "filter"}}};
  static const std::vector<GroupRule> pitch{{"Quality", {"window"}},
                                            {"Interval", {"semitone", "cents", "interval", "degree", "key", "scale", "mode", "oct", "voice",
                                                          "tone"}}};
  static const std::vector<GroupRule> none;
  switch (category) {
    case Category::delay: return delay;
    case Category::reverb: return reverb;
    case Category::modulation: return modulation;
    case Category::dynamics: return dynamics;
    case Category::drive:
    case Category::amp: return drive;
    case Category::filter: return filter;
    case Category::pitch: return pitch;
    default: return none;
  }
}

}  // namespace

juce::String paramGroup(Category category, const KnobParam& param) {
  const juce::String id = param.id.toLowerCase();
  // Equaliser bands, by number or by name.
  if (category == Category::eq) {
    if (id.length() > 1 && id[0] == 'b' && juce::CharacterFunctions::isDigit(id[1]))
      return "Band " + id.substring(1, 2);
    if (id.startsWith("hp") || id.startsWith("lp"))
      return "Filters";
    if (id.startsWith("lowmid"))
      return "Low-mid";
    if (id.startsWith("highmid"))
      return "High-mid";
    if (id.startsWith("low"))
      return "Low";
    if (id.startsWith("high"))
      return "High";
  }
  if (id == "leveldb" || id == "outputdb" || id == "makeupdb" || id == "master" || id == "outputgain" || id == "mix" ||
      (category == Category::dynamics && id == "gaindb"))
    return "Output";
  for (const GroupRule& rule : rulesFor(category))
    if (hasAny(id, rule.keywords))
      return rule.group;
  // A kind the tables do not know: the words it uses.
  if (hasAny(id, {"tone", "treble", "bass", "middle", "presence", "cut", "damp", "shelf"}))
    return "Tone";
  if (hasAny(id, {"attack", "release", "holdms"}))
    return "Timing";
  if (id.contains("feedback"))
    return "Feedback";
  if (hasAny(id, {"depth", "width", "modul", "wow", "flutter"}))
    return "Modulation";
  return "Parameters";
}

int groupRank(const juce::String& group) {
  if (group.startsWith("Band ")) {
    return 10 + group.getTrailingIntValue();
  }
  static const std::map<juce::String, int> ranks{
      {"Filters", 5}, {"Low", 10}, {"Low-mid", 11}, {"High-mid", 12}, {"High", 13}, {"Threshold", 10}, {"Space", 10}, {"Drive", 10},
      {"Interval", 10}, {"Sweep", 10}, {"Shape", 12}, {"Feedback", 20}, {"Timing", 22}, {"Quality", 24}, {"Tone", 30},
      {"Modulation", 40}, {"Parameters", 60}, {"Output", 100}};
  const auto found = ranks.find(group);
  return found != ranks.end() ? found->second : 50;
}

// ####################
// LIVE
// ####################
BlockLive BlockLive::fromVar(const juce::var& meters) {
  BlockLive live;
  const auto level = [](const juce::var& v) {
    if (const auto* list = v.getArray()) {
      float best = -100.0f;
      for (const auto& item : *list)
        best = juce::jmax(best, static_cast<float>(static_cast<double>(item)));
      return best;
    }
    return v.isVoid() ? -100.0f : static_cast<float>(static_cast<double>(v));
  };
  live.inDb = level(meters["in"]);
  live.outDb = level(meters["out"]);
  if (const auto* readings = meters["readings"].getDynamicObject())
    for (const auto& property : readings->getProperties())
      live.readings[property.name.toString()] = static_cast<double>(property.value);
  return live;
}

// ####################
// BASE
// ####################
void BlockVisual::setAccent(juce::Colour newAccent) {
  if (accent == newAccent)
    return;
  accent = newAccent;
  repaint();
}

void BlockVisual::setParams(const std::vector<KnobParam>& newParams, bool blockEnabled) {
  bool same = blockEnabled == enabled && newParams.size() == params.size();
  for (size_t i = 0; same && i < params.size(); ++i)
    same = params[i].id == newParams[i].id && params[i].value == newParams[i].value;
  params = newParams;
  enabled = blockEnabled;
  if (!same)
    repaint();
}

bool BlockVisual::liveMatters(const BlockLive&, const BlockLive&) const { return false; }

void BlockVisual::setLive(const BlockLive& newLive) {
  const bool matters = liveMatters(live, newLive);
  live = newLive;
  if (matters)
    repaint();
}

void BlockVisual::setTempo(double newBpm) {
  if (newBpm == bpm)
    return;
  bpm = newBpm;
  repaint();
}

const KnobParam* BlockVisual::find(std::initializer_list<const char*> ids) const {
  for (const char* id : ids)
    for (const auto& p : params)
      if (p.id == id)
        return &p;
  return nullptr;
}

double BlockVisual::value(std::initializer_list<const char*> ids, double fallback) const {
  const KnobParam* p = find(ids);
  return p != nullptr ? p->value : fallback;
}

double BlockVisual::position(std::initializer_list<const char*> ids, double fallback) const {
  const KnobParam* p = find(ids);
  return p != nullptr ? juce::jlimit(0.0, 1.0, p->toPosition(p->value)) : fallback;
}

bool BlockVisual::flag(std::initializer_list<const char*> ids, bool fallback) const {
  const KnobParam* p = find(ids);
  return p != nullptr ? p->value >= 0.5 : fallback;
}

int BlockVisual::choice(std::initializer_list<const char*> ids, int fallback) const {
  const KnobParam* p = find(ids);
  return p != nullptr ? juce::roundToInt(p->value) : fallback;
}

double BlockVisual::timeMs() const {
  const KnobParam* time = find({"timeMs"});
  if (time == nullptr)
    return 0.0;
  if (flag({"sync"}, false)) {
    const int division = choice({"division"}, 0);
    return noteMs(division, bpm);
  }
  return time->value;
}

double BlockVisual::rateHz() const {
  const KnobParam* rate = find({"rateHz"});
  if (rate == nullptr)
    return 0.0;
  if (flag({"sync"}, false))
    return 1000.0 / juce::jmax(1.0, noteMs(choice({"division"}, 0), bpm));
  return rate->value;
}

void BlockVisual::paint(juce::Graphics& g) {
  auto box = getLocalBounds().toFloat();
  if (!framed()) {
    draw(g, box);
    return;
  }
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box, size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.reduced(0.5f), size::panelRadius, 1.0f);
  auto inner = box.reduced(12.0f, 10.0f);
  paint::label(g, heading(), inner.removeFromTop(14.0f).toNearestInt(), juce::Justification::centredLeft, accent);
  inner.removeFromTop(6.0f);
  const float alpha = enabled ? 1.0f : 0.4f;
  g.beginTransparencyLayer(alpha);
  draw(g, inner);
  g.endTransparencyLayer();
}

}  // namespace pedaleira::ui
