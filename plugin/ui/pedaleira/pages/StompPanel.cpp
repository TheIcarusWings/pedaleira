#include "StompPanel.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

namespace pedaleira::ui {

namespace {

constexpr int kDetailWidth = 500;
constexpr int kSwitchPitch = 96;
constexpr int kMemberHeight = 36;
constexpr int kMemberGap = 4;
constexpr int kMaxMembers = 3;

}  // namespace

// The small x at the end of a block row.
class StompPanel::RemoveButton final : public juce::Button {
public:
  explicit RemoveButton(const juce::String& accessibleName) : juce::Button(accessibleName) {
    setTitle(accessibleName);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

  void paintButton(juce::Graphics& g, bool highlighted, bool) override {
    const auto area = getLocalBounds().toFloat();
    if (highlighted) {
      g.setColour(colour::control);
      g.fillRoundedRectangle(area, 6.0f);
    }
    icons::draw(g, icons::path(icons::Icon::close), area.withSizeKeepingCentre(16.0f, 16.0f),
                highlighted ? colour::text : colour::muted);
    if (hasKeyboardFocus(false)) {
      g.setColour(colour::iris);
      g.drawRoundedRectangle(area.reduced(1.0f), 6.0f, 2.0f);
    }
  }
};

StompPanel::StompPanel(EngineClient& client) : engine(client) {
  setTitle("Stomp assignment");
  for (int i = 0; i < 8; ++i) {
    auto fs = std::make_unique<Footswitch>(FootswitchModel::letterOf(i));
    // A click picks the switch; it does not flip its blocks.
    fs->onPress = [this, i] { selectSwitch(i); };
    addAndMakeVisible(*fs);
    switches[static_cast<size_t>(i)] = std::move(fs);
  }
  label.setTitle("Label");
  label.setFont(type::sans(14.0f, type::Weight::medium));
  label.setColour(juce::TextEditor::backgroundColourId, colour::surface);
  label.setColour(juce::TextEditor::outlineColourId, colour::line);
  label.setColour(juce::TextEditor::focusedOutlineColourId, colour::iris);
  label.setColour(juce::TextEditor::textColourId, colour::text);
  label.setIndents(12, 0);
  label.setJustification(juce::Justification::centredLeft);
  label.setMultiLine(false);
  label.onReturnKey = [this] { commitLabel(); };
  label.onFocusLost = [this] { commitLabel(); };
  addBlock.onClick = [this] { addBlockMenu(); };
  momentary.onToggle = [this](bool on) {
    engine.run("stomp.setOptions",
               object({{"switch", FootswitchModel::letterOf(selected)}, {"momentary", on}}));
    notifyChanged();
  };
  midi.setSwitchEnabled(false);
  modes.onClick = [this] {
    if (onOpenModes)
      onOpenModes();
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&modes, &addBlock, &momentary, &midi, &label})
    addAndMakeVisible(c);
}

StompPanel::~StompPanel() = default;

void StompPanel::notifyChanged() {
  // Called from a button of this panel: the page reads the rig on the next
  // turn, so the button is not deleted while its click is running.
  juce::Component::SafePointer<StompPanel> self(this);
  juce::MessageManager::callAsync([self] {
    if (self != nullptr && self->onChanged)
      self->onChanged();
  });
}

juce::StringArray StompPanel::selectedBlocks() const { return model.stompBlocks(selected); }

void StompPanel::selectSwitch(int index) {
  selected = juce::jlimit(0, 7, index);
  rebuildMembers();
  for (int i = 0; i < 8; ++i)
    switches[static_cast<size_t>(i)]->setSelected(i == selected);
  resized();
  repaint();
  if (onSelectionChanged)
    onSelectionChanged();
}

void StompPanel::refresh(const GridModel& gridModel) {
  grid = gridModel;
  model = FootswitchModel::read(engine);
  for (int i = 0; i < 8; ++i) {
    Footswitch& fs = *switches[static_cast<size_t>(i)];
    SwitchFace face = model.stompFace(i);
    if (!model.stompAssigned(i))
      face.name = "Assign";
    // Without a label, a switch with several blocks is named after the first two.
    const juce::StringArray ids = model.stompBlocks(i);
    if (model.stomps[static_cast<size_t>(i)].label.isEmpty() && ids.size() > 1)
      face.name = model.blocks.at(ids[0]).name + " + " + model.blocks.at(ids[1]).name;
    applyFace(fs, face);
    fs.setSelected(i == selected);
  }
  rebuildMembers();
  resized();
  repaint();
  if (onSelectionChanged)
    onSelectionChanged();
}

void StompPanel::rebuildMembers() {
  members.clear();
  const juce::StringArray ids = model.stompBlocks(selected);
  const int shown = ids.size() > kMaxMembers ? kMaxMembers - 1 : ids.size();
  hiddenMembers = ids.size() - shown;
  const juce::String letter = FootswitchModel::letterOf(selected);
  for (int i = 0; i < shown; ++i) {
    Member member;
    const auto& fact = model.blocks.at(ids[i]);
    member.blockId = ids[i];
    member.name = fact.name;
    member.tint = fact.category;
    member.category = categoryName(fact.category);
    member.remove = std::make_unique<RemoveButton>("Remove " + fact.name + " from footswitch " + letter);
    const juce::String blockId = ids[i];
    member.remove->onClick = [this, blockId] {
      engine.run("stomp.unassign", object({{"switch", FootswitchModel::letterOf(selected)}, {"blockId", blockId}}));
      notifyChanged();
    };
    addAndMakeVisible(*member.remove);
    members.push_back(std::move(member));
  }

  const bool assigned = !ids.isEmpty();
  momentary.setState(assigned && model.stomps[static_cast<size_t>(selected)].momentary);
  momentary.setSwitchEnabled(assigned);
  if (!label.hasKeyboardFocus(true))
    label.setText(model.stomps[static_cast<size_t>(selected)].label, false);
  label.setEnabled(assigned);
  label.setAlpha(assigned ? 1.0f : 0.5f);
}

void StompPanel::commitLabel() {
  if (!model.stompAssigned(selected) || label.getText() == model.stomps[static_cast<size_t>(selected)].label)
    return;
  engine.run("stomp.setOptions", object({{"switch", FootswitchModel::letterOf(selected)}, {"label", label.getText()}}));
  notifyChanged();
}

void StompPanel::addBlockMenu() {
  juce::PopupMenu menu;
  const juce::StringArray onSwitch = model.stompBlocks(selected);
  const int index = selected;
  juce::Component::SafePointer<StompPanel> self(this);
  for (const int r : grid.visibleRows())
    for (size_t s = 0; s < 8; ++s) {
      const GridSlot& slot = grid.rows[static_cast<size_t>(r)].slots[s];
      if (slot.empty)
        continue;
      const juce::String title = slot.name + "   Row " + juce::String(r + 1) + ", slot " + juce::String(s + 1);
      const juce::String blockId = slot.blockId;
      menu.addItem(title, !onSwitch.contains(blockId), false, [self, index, blockId, onSwitch] {
        if (self == nullptr)
          return;
        juce::Array<juce::var> ids;
        for (const auto& id : onSwitch)
          ids.add(id);
        ids.add(blockId);
        const FootswitchModel::Stomp& current = self->model.stomps[static_cast<size_t>(index)];
        // Assigning replaces the switch, so its label and mode are passed again.
        self->engine.run("stomp.assign", object({{"switch", FootswitchModel::letterOf(index)},
                                                 {"blockIds", ids},
                                                 {"label", current.label},
                                                 {"momentary", current.momentary}}));
        self->notifyChanged();
      });
    }
  if (menu.getNumItems() == 0)
    menu.addItem("No blocks in the rig yet", false, false, [] {});
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addBlock));
}

void StompPanel::resized() {
  const int width = getWidth();
  modes.setBounds(width - 24 - modes.idealWidth(), 16, modes.idealWidth(), 28);

  const int switchHeight = Footswitch::heightFor(Footswitch::Size::regular);
  for (int i = 0; i < 8; ++i)
    switches[static_cast<size_t>(i)]->setBounds(24 + i * kSwitchPitch, 54, kSwitchPitch, switchHeight);
  hintArea = {24, 54 + switchHeight + 14, 8 * kSwitchPitch, 36};

  detailBox = {width - 24 - kDetailWidth, 54, kDetailWidth, getHeight() - 54 - 16};
  auto inner = detailBox.reduced(16, 14);
  auto head = inner.removeFromTop(28);
  addBlock.setBounds(head.removeFromRight(addBlock.idealWidth()).withHeight(28));
  inner.removeFromTop(12);

  // The bottom: two switches on the left, the label on the right.
  auto bottom = inner.removeFromBottom(60);
  momentary.setBounds(bottom.getX(), bottom.getY(), momentary.idealWidth(), 28);
  midi.setBounds(bottom.getX(), bottom.getY() + 32, midi.idealWidth(), 28);
  // "Send MIDI on press" has no engine behind it yet; its note sits after it.
  midiNote = {midi.getRight() + 8, midi.getY(), 124, 28};
  const int labelLeft = midiNote.getRight() + 16;
  labelArea = {labelLeft, bottom.getY(), bottom.getRight() - labelLeft, 60};
  label.setBounds(labelArea.getX(), labelArea.getBottom() - 34, labelArea.getWidth(), 34);

  inner.removeFromBottom(12);
  memberArea = inner;
  int y = memberArea.getY();
  for (auto& member : members) {
    member.remove->setBounds(memberArea.getRight() - 6 - 28, y + (kMemberHeight - 28) / 2, 28, 28);
    y += kMemberHeight + kMemberGap;
  }
}

void StompPanel::paint(juce::Graphics& g) {
  g.fillAll(colour::surface);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText("Stomp assignment", 24, 16, 400, 26, juce::Justification::centredLeft, true);

  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawFittedText("Select a footswitch to see its blocks. Add block puts one on it, or select a block on the grid "
                   "and press Assign footswitch.",
                   hintArea, juce::Justification::topLeft, 2);

  // The selected switch.
  g.setColour(colour::ground);
  g.fillRoundedRectangle(detailBox.toFloat(), size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(detailBox.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
  g.setFont(type::sans(16.0f, type::Weight::semibold));
  g.setColour(colour::text);
  g.drawText(juce::String("Footswitch ") + FootswitchModel::letterOf(selected), detailBox.getX() + 16,
             detailBox.getY() + 14, 240, 28, juce::Justification::centredLeft, true);

  int y = memberArea.getY();
  for (const Member& member : members) {
    const juce::Rectangle<int> row(memberArea.getX(), y, memberArea.getWidth(), kMemberHeight);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(row.toFloat(), size::buttonRadius);
    const auto icon = juce::Rectangle<float>(24.0f, 24.0f).withCentre({row.getX() + 18.0f, row.getCentreY() * 1.0f});
    g.setColour(categoryTint(member.tint));
    g.fillRoundedRectangle(icon, 6.0f);
    icons::draw(g, icons::path(member.tint), icon.reduced(4.0f), categoryColour(member.tint));
    g.setFont(type::sans(13.0f, type::Weight::semibold));
    g.setColour(colour::text);
    const int nameWidth = juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), member.name)) + 2;
    const int textLeft = row.getX() + 6 + 24 + 10;
    g.drawText(member.name, textLeft, row.getY(), juce::jmin(nameWidth, row.getWidth() - 100), kMemberHeight,
               juce::Justification::centredLeft, true);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText(member.category, textLeft + juce::jmin(nameWidth, row.getWidth() - 100) + 8, row.getY(), 120,
               kMemberHeight, juce::Justification::centredLeft, true);
    y += kMemberHeight + kMemberGap;
  }
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  if (members.empty())
    g.drawText("Nothing on this switch yet.", memberArea.getX(), y, memberArea.getWidth(), kMemberHeight,
               juce::Justification::centredLeft, true);
  else if (hiddenMembers > 0)
    g.drawText("and " + juce::String(hiddenMembers) + " more", memberArea.getX() + 6, y, memberArea.getWidth(), 24,
               juce::Justification::centredLeft, true);

  paint::label(g, "Label", labelArea.withHeight(14));
  g.setFont(type::sans(12.0f));
  g.setColour(colour::faint);
  g.drawText(kLaterBuild, midiNote, juce::Justification::centredLeft, true);
}

}  // namespace pedaleira::ui
