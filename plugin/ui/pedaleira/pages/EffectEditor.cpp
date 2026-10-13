#include "EffectEditor.h"

#include "../Paint.h"
#include "../Type.h"
#include "BlockStyle.h"
#include "TempoNotes.h"

#include <algorithm>
#include <cmath>

namespace pedaleira::ui {

namespace {
// The graphic equaliser's band parameters, low to high (effects/eq/GraphicEq10.cpp).
juce::String graphicEqBandId(int band) {
  static const char* const ids[GraphicEqView::kBands] = {"band31", "band63", "band125", "band250", "band500",
                                                         "band1k", "band2k", "band4k",  "band8k",  "band16k"};
  return ids[band];
}
}  // namespace

namespace {

constexpr int kGap = 16;
constexpr int kPresetsWidth = 260;
constexpr int kPresetsGap = 20;
constexpr int kKnobDiameter = 72;
constexpr int kTimeKnobDiameter = 88;
constexpr int kTimeWidth = 500;
// The time knob reads a synced note and its time under it: a wider cell.
constexpr int kTimeKnobCell = 152;
constexpr int kTimeHeight = 250;
constexpr int kTimeHeightCompact = 196;
constexpr int kSectionPadding = 16;
// The label row and the gap under it, inside a section.
constexpr int kSectionHead = 28;
constexpr int kSwitchHeight = 32;
constexpr int kChoiceHeight = 60;
// The tail switch of kinds that ring out (kinds.list, spillover).
const char* const kSpilloverId = "spillover";

// The forms a note value takes (the note values themselves are in TempoNotes).
const juce::StringArray& noteVariants() {
  static const juce::StringArray variants{"Straight", "Dotted", "Triplet"};
  return variants;
}

const KnobParam* findSpec(const std::vector<KnobParam>& specs, const juce::String& id) {
  for (const auto& spec : specs)
    if (spec.id == id)
      return &spec;
  return nullptr;
}

}  // namespace

// ####################
// CHOICE CELL
// ####################
// A choice parameter: a segmented control when it is short, else a button
// that opens the list. Its name sits above.
struct EffectEditor::ChoiceCell : public juce::Component {
  ChoiceCell(const KnobParam& spec, std::function<void(int)> picked) : name(spec.name), choices(spec.choices), onPick(std::move(picked)) {
    setTitle(spec.name);
    const bool short_ = choices.size() <= 4;
    if (short_) {
      segments = std::make_unique<SegmentedControl>(choices);
      segments->setTitle(spec.name);
      segments->onChange = [this](int index) { onPick(index); };
      addAndMakeVisible(*segments);
    } else {
      menu = std::make_unique<PushButton>("");
      menu->setTitle(spec.name);
      menu->onClick = [this] {
        juce::PopupMenu list;
        for (int i = 0; i < choices.size(); ++i)
          list.addItem(choices[i], true, i == current, [this, i] {
            setValue(i);
            onPick(i);
          });
        list.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menu.get()));
      };
      addAndMakeVisible(*menu);
    }
    setValue(juce::roundToInt(spec.value));
    const int controlWidth = short_ ? segments->idealWidth() : 160;
    setSize(juce::jmax(controlWidth, juce::roundToInt(juce::GlyphArrangement::getStringWidth(type::sans(12.0f), name)) + 2),
            kChoiceHeight);
  }

  void setValue(int index) {
    current = juce::jlimit(0, juce::jmax(0, choices.size() - 1), index);
    if (segments != nullptr)
      segments->setSelected(current);
    else
      menu->setButtonText(choices[current]);
  }

  void paint(juce::Graphics& g) override {
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(name, getLocalBounds().removeFromTop(18), juce::Justification::centredLeft, true);
  }

  void resized() override {
    auto control = getLocalBounds().withTrimmedTop(22).withHeight(36);
    if (segments != nullptr)
      segments->setBounds(control.withWidth(segments->idealWidth()));
    else
      menu->setBounds(control.withWidth(160).withHeight(juce::jmin(36, control.getHeight())));
  }

  juce::String name;
  juce::StringArray choices;
  std::function<void(int)> onPick;
  std::unique_ptr<SegmentedControl> segments;
  std::unique_ptr<PushButton> menu;
  int current = 0;
};

// ####################
// EFFECT EDITOR
// ####################
EffectEditor::EffectEditor(EngineClient& client) : BlockEditorBody(client) {
  setTitle("Effect editor");
  saveAs.setButtonText("Save as" + juce::String::fromUTF8("\xe2\x80\xa6"));
  saveAs.setEnabled(false);
  saveAs.setTooltip(kLaterBuild);
  addAndMakeVisible(saveAs);
}

EffectEditor::~EffectEditor() = default;

void EffectEditor::setStyle(const Style& newStyle) {
  style = newStyle;
  saveAs.setVisible(!style.compact);
  signature.clear();
  resized();
  repaint();
}

int EffectEditor::sectionPadding() const { return style.compact ? 12 : kSectionPadding; }
int EffectEditor::sectionHead() const { return style.compact ? 26 : kSectionHead; }
int EffectEditor::gapPx() const { return style.compact ? 10 : kGap; }
int EffectEditor::knobDiameter() const { return style.compact ? (denseKnobs ? 52 : 60) : kKnobDiameter; }

// Keeps the cached value of a parameter in step with what the player did and
// tells the owner (the panel's picture follows).
void EffectEditor::noteValue(const juce::String& id, double value) {
  for (auto& spec : lastSpecs)
    if (spec.id == id)
      spec.value = value;
  if (onParamsChanged)
    onParamsChanged(lastSpecs);
}

void EffectEditor::setParam(const juce::String& id, double value) {
  noteValue(id, value);
  engine.run("block.setParam", object({{"blockId", context.blockId}, {"param", id}, {"value", value}}));
}

void EffectEditor::readTempo() {
  const juce::var tempo = engine.read("tempo.get");
  if (tempo.hasProperty("bpm"))
    bpm = static_cast<double>(tempo["bpm"]);
}

ParamKnob* EffectEditor::makeKnob(const KnobParam& spec, int diameter) {
  auto knob = std::make_unique<ParamKnob>(engine, context.blockId, spec);
  knob->setDiameter(diameter);
  knob->setSize(ParamKnob::cellSize(diameter).x, ParamKnob::cellSize(diameter).y);
  knob->onAssignmentChanged = [this] {
    if (onChanged)
      onChanged();
  };
  knob->setBypassed(!context.enabled);
  if (!style.accent.isTransparent())
    knob->setAccent(style.accent);
  knob->onValueChanged = [this](const KnobParam& p) { noteValue(p.id, p.value); };
  addAndMakeVisible(*knob);
  ParamKnob* raw = knob.get();
  knobsById[spec.id] = raw;
  owned.push_back(std::move(knob));
  return raw;
}

void EffectEditor::addItem(Section& section, juce::Component* item, bool breakBefore) {
  section.items.push_back(item);
  section.breaks.push_back(breakBefore);
}

void EffectEditor::showBlock(const BlockContext& block) {
  const bool sameBlock = block.blockId == context.blockId;
  context = block;
  const juce::var listed = engine.read("block.listParams", object({{"blockId", block.blockId}}));
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
  readTempo();
  lastSpecs = specs;
  int floats = 0;
  for (const auto& spec : specs)
    if (!spec.common && spec.type == "float")
      ++floats;
  denseKnobs = floats > 12;
  if (!sameBlock || ids != signature) {
    signature = ids;
    rebuild(specs);
  } else {
    updateValues(specs);
  }
  if (onParamsChanged)
    onParamsChanged(lastSpecs);
}

void EffectEditor::rebuild(const std::vector<KnobParam>& specs) {
  sections.clear();
  timeGroups.clear();
  graphicEq.reset();
  knobsById.clear();
  choicesById.clear();
  switchesById.clear();
  owned.clear();

  // Tempo-synced times and rates, with the switch and note value that go
  // with each.
  juce::StringArray consumed;
  std::vector<Section> built;
  for (const auto& spec : specs) {
    juce::String syncId, divisionId;
    if (spec.common || spec.type != "float" || !spec.tempoSync || !tempoNeighbours(spec.id, syncId, divisionId))
      continue;
    const KnobParam* sync = findSpec(specs, syncId);
    const KnobParam* division = findSpec(specs, divisionId);
    if (sync == nullptr || division == nullptr || division->choices.size() != noteBases().size() * 3)
      continue;
    auto group = std::make_unique<TimeGroup>();
    group->time = spec;
    group->sync = *sync;
    group->division = *division;
    TimeGroup* raw = group.get();
    group->knob = std::make_unique<ParamKnob>(engine, context.blockId, spec);
    group->knob->setDiameter(kTimeKnobDiameter);
    group->knob->setSize(kTimeKnobCell, ParamKnob::cellSize(kTimeKnobDiameter).y);
    group->knob->valueText = [this, raw](const KnobParam& p) { return timeText(*raw, p); };
    group->knob->onAssignmentChanged = [this] {
      if (onChanged)
        onChanged();
    };
    // Turning the time while it follows the tempo means the player wants a
    // time of their own: the sync switches off.
    if (!style.accent.isTransparent())
      group->knob->setAccent(style.accent);
    group->knob->onValueChanged = [this, raw](const KnobParam& p) {
      noteValue(p.id, p.value);
      raw->time.value = p.value;
      if (raw->sync.value >= 0.5) {
        raw->sync.value = 0.0;
        setParam(raw->sync.id, 0.0);
        updateTime(*raw);
      }
    };
    group->knob->update(spec);
    addAndMakeVisible(*group->knob);
    knobsById[spec.id] = group->knob.get();

    group->syncRow = std::make_unique<SwitchRow>("Tempo sync");
    group->syncRow->onToggle = [this, raw](bool on) {
      raw->sync.value = on ? 1.0 : 0.0;
      setParam(raw->sync.id, raw->sync.value);
      updateTime(*raw);
    };
    addAndMakeVisible(*group->syncRow);
    group->note = std::make_unique<SegmentedControl>(noteBases());
    group->note->setTitle("Note value");
    group->variant = std::make_unique<SegmentedControl>(noteVariants());
    group->variant->setTitle("Note length");
    const auto pickNote = [this, raw] {
      const int division = raw->note->getSelected() * 3 + raw->variant->getSelected();
      raw->division.value = division;
      setParam(raw->division.id, division);
      updateTime(*raw);
    };
    group->note->onChange = [pickNote](int) { pickNote(); };
    group->variant->onChange = [pickNote](int) { pickNote(); };
    addAndMakeVisible(*group->note);
    addAndMakeVisible(*group->variant);
    group->tap = std::make_unique<PushButton>("Tap");
    group->tap->onClick = [this, raw] {
      engine.run("tempo.tap");
      readTempo();
      updateTime(*raw);
    };
    addAndMakeVisible(*group->tap);
    updateTime(*group);

    Section section;
    section.title = spec.name;
    section.time = raw;
    built.push_back(section);
    consumed.add(spec.id);
    consumed.add(syncId);
    consumed.add(divisionId);
    timeGroups.push_back(std::move(group));
  }

  // The graphic equaliser: its bands as sliders over the curve they make.
  if (context.kindId == "eq.graphic10") {
    graphicEq = std::make_unique<GraphicEqView>();
    graphicEq->setSize(640, style.compact ? 190 : 260);
    graphicEq->onChange = [this](int band, double db) { setParam(graphicEqBandId(band), db); };
    addAndMakeVisible(*graphicEq);
    Section bands;
    bands.title = "Bands";
    bands.items.push_back(graphicEq.get());
    bands.breaks.push_back(false);
    built.push_back(bands);
    for (int b = 0; b < GraphicEqView::kBands; ++b)
      consumed.add(graphicEqBandId(b));
  }

  // The other parameters of the kind, in the sections its category groups
  // them in (BlockStyle): in each, the knobs, then the choices, then the
  // switches. The output section also takes the common output and mix knobs
  // and the tail switch.
  struct Pending {
    juce::String title;
    std::vector<const KnobParam*> knobs, choices, switches;
  };
  std::vector<Pending> pending;
  const auto groupNamed = [&pending](const juce::String& title) -> Pending& {
    for (auto& group : pending)
      if (group.title == title)
        return group;
    pending.push_back({title, {}, {}, {}});
    return pending.back();
  };
  const KnobParam* spillover = nullptr;
  bool anyKnob = false;
  for (const auto& spec : specs) {
    if (spec.common || consumed.contains(spec.id))
      continue;
    if (spec.id == kSpilloverId) {
      spillover = &spec;
      continue;
    }
    Pending& group = groupNamed(paramGroup(context.category, spec));
    if (spec.type == "float") {
      anyKnob = true;
      group.knobs.push_back(&spec);
    } else if (spec.type == "choice") {
      group.choices.push_back(&spec);
    } else if (spec.type == "boolean") {
      group.switches.push_back(&spec);
    }
  }
  if (!anyKnob)
    if (const KnobParam* input = findSpec(specs, "inputGain"))
      groupNamed("Parameters").knobs.push_back(input);
  Pending& outputGroup = groupNamed("Output");
  for (const char* id : {"outputGain", "mix"})
    if (const KnobParam* spec = findSpec(specs, id))
      outputGroup.knobs.push_back(spec);

  // In a section named after what it belongs to ("Band 3"), the parameters
  // do not repeat it: "Frequency", not "Band 3 frequency".
  const auto shortName = [](const juce::String& title, const juce::String& name) {
    if (!name.startsWithIgnoreCase(title + " "))
      return name;
    const juce::String rest = name.substring(title.length() + 1);
    return rest.substring(0, 1).toUpperCase() + rest.substring(1);
  };
  const auto makeSwitch = [this](const KnobParam& spec, const juce::String& label) {
    auto row = std::make_unique<SwitchRow>(label);
    row->setState(spec.value >= 0.5);
    row->onToggle = [this, id = spec.id](bool on) { setParam(id, on ? 1.0 : 0.0); };
    row->setSize(row->idealWidth(), kSwitchHeight);
    addAndMakeVisible(*row);
    switchesById[spec.id] = row.get();
    SwitchRow* raw = row.get();
    owned.push_back(std::move(row));
    return raw;
  };
  std::stable_sort(pending.begin(), pending.end(),
                   [](const Pending& x, const Pending& y) { return groupRank(x.title) < groupRank(y.title); });
  for (auto& group : pending) {
    Section section;
    section.title = group.title;
    for (const KnobParam* spec : group.knobs) {
      KnobParam named = *spec;
      named.name = shortName(group.title, named.name);
      addItem(section, makeKnob(named, knobDiameter()));
    }
    bool first = true;
    for (const KnobParam* spec : group.choices) {
      KnobParam named = *spec;
      named.name = shortName(group.title, named.name);
      auto cell = std::make_unique<ChoiceCell>(named, [this, id = spec->id](int index) { setParam(id, index); });
      addAndMakeVisible(*cell);
      choicesById[spec->id] = cell.get();
      addItem(section, cell.get(), first && !group.knobs.empty());
      first = false;
      owned.push_back(std::move(cell));
    }
    first = true;
    for (const KnobParam* spec : group.switches) {
      addItem(section, makeSwitch(*spec, shortName(group.title, spec->name)), first && (!group.knobs.empty() || !group.choices.empty()));
      first = false;
    }
    if (group.title == "Output" && spillover != nullptr)
      addItem(section, makeSwitch(*spillover, "Tails spill over on bypass"), true);
    if (!section.items.empty())
      built.push_back(std::move(section));
  }
  // A tail switch with no output section to sit in.
  if (spillover != nullptr && switchesById.find(spillover->id) == switchesById.end()) {
    Section section;
    section.title = "Output";
    addItem(section, makeSwitch(*spillover, "Tails spill over on bypass"));
    built.push_back(std::move(section));
  }

  sections = std::move(built);
  updateValues(specs);
  resized();
  repaint();
}

void EffectEditor::updateValues(const std::vector<KnobParam>& specs) {
  const bool dimmed = !context.enabled;
  if (graphicEq != nullptr) {
    graphicEq->setBypassed(dimmed);
    for (int b = 0; b < GraphicEqView::kBands; ++b)
      if (const KnobParam* band = findSpec(specs, graphicEqBandId(b)))
        graphicEq->setGain(b, band->value);
  }
  for (const auto& spec : specs) {
    if (const auto knob = knobsById.find(spec.id); knob != knobsById.end()) {
      knob->second->update(spec);
      knob->second->setBypassed(dimmed);
    }
    if (const auto choice = choicesById.find(spec.id); choice != choicesById.end())
      choice->second->setValue(juce::roundToInt(spec.value));
    if (const auto row = switchesById.find(spec.id); row != switchesById.end())
      row->second->setState(spec.value >= 0.5);
  }
  for (auto& group : timeGroups) {
    if (const KnobParam* sync = findSpec(specs, group->sync.id))
      group->sync = *sync;
    if (const KnobParam* division = findSpec(specs, group->division.id))
      group->division = *division;
    if (const KnobParam* time = findSpec(specs, group->time.id)) {
      group->time = *time;
    }
    updateTime(*group);
  }
}

juce::String EffectEditor::timeText(const TimeGroup& group, const KnobParam& spec) const {
  if (group.sync.value < 0.5)
    return spec.text(spec.value);
  const int division = juce::roundToInt(group.division.value);
  const double ms = noteMs(division, bpm);
  const juce::String amount =
      spec.unit == "Hz" ? juce::String(1000.0 / ms, 2) + " Hz" : juce::String(juce::roundToInt(ms)) + " ms";
  return noteName(division) + " " + type::dot() + " " + amount;
}

void EffectEditor::updateTime(TimeGroup& group) {
  const bool synced = group.sync.value >= 0.5;
  const int division = juce::jlimit(0, noteBases().size() * 3 - 1, juce::roundToInt(group.division.value));
  group.syncRow->setState(synced);
  group.syncRow->setDetail(juce::String(bpm, 1) + " BPM");
  group.note->setSelected(division / 3);
  group.variant->setSelected(division % 3);
  group.note->setEnabled(synced);
  group.variant->setEnabled(synced);
  group.knob->update(group.time);
  group.knob->setBypassed(!context.enabled);
  repaint();
}

// ####################
// LAYOUT
// ####################
int EffectEditor::sectionHeight(const Section& section, int width) const {
  if (section.time != nullptr)
    return style.compact ? kTimeHeightCompact : kTimeHeight;
  const int inner = width - 2 * sectionPadding();
  int total = 0, line = 0, lineHeight = 0;
  for (size_t i = 0; i < section.items.size(); ++i) {
    const int w = section.items[i]->getWidth(), h = section.items[i]->getHeight();
    if (line > 0 && (section.breaks[i] || line + gapPx() + w > inner)) {
      total += lineHeight + gapPx();
      line = 0;
      lineHeight = 0;
    }
    line += (line > 0 ? gapPx() : 0) + w;
    lineHeight = juce::jmax(lineHeight, h);
  }
  return 2 * sectionPadding() + sectionHead() + total + lineHeight;
}

void EffectEditor::placeItems(Section& section) {
  auto area = section.bounds.reduced(sectionPadding());
  area.removeFromTop(sectionHead());
  if (section.time != nullptr) {
    TimeGroup& group = *section.time;
    group.knob->setTopLeftPosition(area.getX(), area.getY() + (style.compact ? 0 : 8));
    const int x = area.getX() + kTimeKnobCell + 20;
    int y = area.getY();
    if (style.compact) {
      group.syncRow->setBounds(x, y, group.syncRow->idealWidth(), 28);
      group.tap->setBounds(x + group.syncRow->idealWidth() + 16, y - 1, 72, 30);
      y += 28 + 10 + 18;
      group.note->setBounds(x, y, group.note->idealWidth(), 36);
      group.variant->setBounds(group.note->getRight() + 12, y, group.variant->idealWidth(), 36);
      return;
    }
    group.syncRow->setBounds(x, y, group.syncRow->idealWidth(), 28);
    y += 28 + 14 + 16 + 6;
    group.note->setBounds(x, y, group.note->idealWidth(), 36);
    y += 36 + 8;
    group.variant->setBounds(x, y, group.variant->idealWidth(), 36);
    y += 36 + 14;
    group.tap->setBounds(x, y, 72, 32);
    return;
  }
  const int inner = area.getWidth();
  int x = 0, y = 0, lineHeight = 0;
  for (size_t i = 0; i < section.items.size(); ++i) {
    juce::Component* item = section.items[i];
    if (x > 0 && (section.breaks[i] || x + gapPx() + item->getWidth() > inner)) {
      y += lineHeight + gapPx();
      x = 0;
      lineHeight = 0;
    }
    if (x > 0)
      x += gapPx();
    item->setTopLeftPosition(area.getX() + x, area.getY() + y);
    x += item->getWidth();
    lineHeight = juce::jmax(lineHeight, item->getHeight());
  }
}

int EffectEditor::naturalWidth(const Section& section) const {
  if (section.time != nullptr) {
    if (style.compact)
      return 2 * sectionPadding() + kTimeKnobCell + 20 + section.time->note->idealWidth() + 12 + section.time->variant->idealWidth();
    return kTimeWidth;
  }
  // As wide as the first line of items (up to the first forced break).
  int widest = 0, firstLine = 0;
  bool inFirstLine = true;
  for (size_t i = 0; i < section.items.size(); ++i) {
    widest = juce::jmax(widest, section.items[i]->getWidth());
    if (i > 0 && section.breaks[i])
      inFirstLine = false;
    if (inFirstLine)
      firstLine += section.items[i]->getWidth() + (firstLine > 0 ? gapPx() : 0);
  }
  return 2 * sectionPadding() + juce::jmax(widest, firstLine);
}

void EffectEditor::layoutSections() {
  const int total = getWidth() - (style.compact ? 0 : kPresetsWidth + kPresetsGap);
  std::vector<Section*> pending;
  for (auto& section : sections)
    pending.push_back(&section);
  int y = 0;
  while (!pending.empty()) {
    // The first section left starts the line; the later ones join it when
    // they fit whole, so a wide section never squeezes a narrow one.
    int x = 0, lineHeight = 0;
    std::vector<Section*> line;
    for (auto at = pending.begin(); at != pending.end();) {
      Section& section = **at;
      const int natural = naturalWidth(section);
      if (!line.empty() && x + natural > total) {
        ++at;
        continue;
      }
      const int width = juce::jmin(natural, total);
      section.bounds = {x, y, width, sectionHeight(section, width)};
      lineHeight = juce::jmax(lineHeight, section.bounds.getHeight());
      line.push_back(&section);
      x += width + gapPx();
      at = pending.erase(at);
    }
    for (Section* section : line)
      section->bounds.setHeight(lineHeight);
    y += lineHeight + gapPx();
  }
  contentHeight = juce::jmax(0, y - gapPx());
  for (auto& section : sections)
    placeItems(section);
}

void EffectEditor::resized() {
  const int total = getWidth() - (style.compact ? 0 : kPresetsWidth + kPresetsGap);
  if (style.compact) {
    presetsBounds = noteBounds = {};
  } else {
    presetsBounds = {total + kPresetsGap, 0, kPresetsWidth, getHeight()};
    noteBounds = {0, getHeight() - 20, total, 20};
    saveAs.setBounds(presetsBounds.getX() + 16, presetsBounds.getBottom() - 16 - 36, presetsBounds.getWidth() - 32, 36);
  }
  layoutSections();
}

void EffectEditor::paint(juce::Graphics& g) {
  for (const auto& section : sections) {
    paintSection(g, section.bounds, section.title, {}, style.accent, sectionPadding());
    if (section.time == nullptr)
      continue;
    const TimeGroup& group = *section.time;
    const int x = group.note->getX();
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("Note value", x, group.note->getY() - (style.compact ? 18 : 22), 200, 16, juce::Justification::centredLeft, false);
    g.drawText("Taps set the rig tempo.", group.tap->getRight() + 12, group.tap->getY(),
               section.bounds.getRight() - 16 - group.tap->getRight() - 12, group.tap->getHeight(),
               juce::Justification::centredLeft, true);
  }

  if (style.compact)
    return;
  // The effect presets column.
  const auto inner = paintSection(g, presetsBounds, "Effect presets");
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawFittedText(juce::String(kLaterBuild) + ". Named settings for this effect will be listed here.",
                   inner.withHeight(48), juce::Justification::topLeft, 3);

  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText("This editor is generated from the effect's parameter description. Right-click any knob to assign it.",
             noteBounds, juce::Justification::centredLeft, true);
}

}  // namespace pedaleira::ui
