#include "GridPage.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kScenesHeight = 56;
// Under this much room the settings panel starts collapsed to its bar.
constexpr int kRoomForSettings = BlockSettingsPanel::kExpandedHeight - 28;

// Whether the player collapsed (true) or expanded (false) the settings panel
// in this session; unset until they do.
std::optional<bool>& sessionCollapsed() {
  static std::optional<bool> collapsed;
  return collapsed;
}
const juce::Colour kCable = colour::faint;

// How far from a point or a cable the mouse may be and still take it.
constexpr float kPointGrab = 10.0f;
constexpr float kCableGrab = 8.0f;

void paintGroupBox(juce::Graphics& g, juce::Rectangle<int> box, const juce::String& title, const juce::String& note) {
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box.toFloat(), size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.toFloat().reduced(0.5f), size::panelRadius, 1.0f);
  auto inner = box.reduced(16, 14);
  g.setFont(type::bodyStrong());
  g.setColour(colour::text);
  g.drawText(title, inner.removeFromTop(20), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(note, inner.removeFromTop(16), juce::Justification::centredLeft, true);
}

}  // namespace

// ####################
// SCENES STRIP
// ####################
void ScenesStrip::setScenes(const std::vector<GridScene>& newScenes) {
  scenes.clear();
  for (const GridScene& scene : newScenes)
    if (scene.inUse && scenes.size() < 8)
      scenes.push_back(scene);
  repaint();
}

juce::Rectangle<float> ScenesStrip::buttonBounds(int index) const {
  // The label takes 64 px; the eight buttons share the rest, 8 px apart.
  const float left = 24.0f + 64.0f + 8.0f;
  const float width = (getWidth() - left - 24.0f - 7.0f * 8.0f) / 8.0f;
  return {left + index * (width + 8.0f), (getHeight() - 36.0f) * 0.5f, width, 36.0f};
}

void ScenesStrip::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setColour(colour::line);
  g.fillRect(0, getHeight() - 1, getWidth(), 1);
  paint::label(g, "Scenes", {24, 0, 64, getHeight()});
  for (size_t i = 0; i < scenes.size(); ++i) {
    const GridScene& scene = scenes[i];
    const auto area = buttonBounds(static_cast<int>(i));
    // Active: iris; a scene with a name of its own: raised; the rest plain.
    const bool lit = static_cast<int>(i) == hovered;
    juce::Colour fill = scene.active ? colour::iris : scene.named ? colour::raised : juce::Colours::transparentBlack;
    if (lit)
      fill = scene.active ? colour::irisHover : colour::control;
    const juce::Colour fg = scene.active ? colour::ground : scene.named || lit ? colour::text : colour::muted;
    g.setColour(fill);
    g.fillRoundedRectangle(area, size::buttonRadius);
    g.setColour(scene.active ? colour::iris : lit ? colour::dim : colour::line);
    g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
    auto inner = area.reduced(10.0f, 0.0f);
    g.setFont(type::mono(13.0f));
    g.setColour(fg);
    g.drawText(scene.id, inner.removeFromLeft(14.0f), juce::Justification::centredLeft, false);
    inner.removeFromLeft(8.0f);
    g.setFont(type::button());
    g.drawText(scene.name, inner, juce::Justification::centredLeft, true);
  }
  if (scenes.size() < 8) {
    const auto area = buttonBounds(static_cast<int>(scenes.size()));
    const bool lit = hovered == static_cast<int>(scenes.size());
    if (lit) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(area, size::buttonRadius);
    }
    paint::dashedOutline(g, area, size::buttonRadius, lit ? colour::dim : colour::line);
    auto inner = area.reduced(10.0f, 0.0f);
    icons::draw(g, icons::path(icons::Icon::plus), inner.removeFromLeft(14.0f).withSizeKeepingCentre(14.0f, 14.0f),
                colour::muted);
    inner.removeFromLeft(8.0f);
    g.setFont(type::button());
    g.setColour(lit ? colour::text : colour::muted);
    g.drawText("Add scene", inner, juce::Justification::centredLeft, true);
  }
}

void ScenesStrip::mouseMove(const juce::MouseEvent& event) {
  int now = -1;
  for (size_t i = 0; i <= scenes.size() && i < 8; ++i)
    if (buttonBounds(static_cast<int>(i)).contains(event.position))
      now = static_cast<int>(i);
  if (now != hovered) {
    hovered = now;
    repaint();
  }
  setMouseCursor(now >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void ScenesStrip::mouseExit(const juce::MouseEvent&) {
  if (hovered >= 0) {
    hovered = -1;
    repaint();
  }
}

void ScenesStrip::mouseUp(const juce::MouseEvent& event) {
  for (size_t i = 0; i < scenes.size(); ++i)
    if (buttonBounds(static_cast<int>(i)).contains(event.position)) {
      if (event.mods.isPopupMenu()) {
        if (scenes.size() > 1 && onRemove)
          onRemove(scenes[i].id);
      } else if (onSelect) {
        onSelect(scenes[i].id);
      }
      return;
    }
  if (scenes.size() < 8 && buttonBounds(static_cast<int>(scenes.size())).contains(event.position) && onAdd)
    onAdd();
}

// ####################
// GRID VIEW
// ####################
// Forwards a tile's mouse to the grid, which knows where the tile sits: for
// dragging blocks and for the tile's menu.
struct GridView::TileMouse final : juce::MouseListener {
  TileMouse(GridView& owner, int r, int s) : grid(owner), row(r), slot(s) {}
  void mouseDown(const juce::MouseEvent& event) override { grid.tileMouseDown(row, slot, event); }
  void mouseDrag(const juce::MouseEvent& event) override { grid.tileMouseDrag(row, slot, event); }
  void mouseUp(const juce::MouseEvent& event) override { grid.tileMouseUp(row, slot, event); }
  void mouseDoubleClick(const juce::MouseEvent&) override {
    const GridSlot& block = grid.model.rows[static_cast<size_t>(row)].slots[static_cast<size_t>(slot)];
    if (!block.empty && grid.onOpenBlock)
      grid.onOpenBlock(block.blockId);
  }
  GridView& grid;
  int row, slot;
};

GridView::GridView() {
  setTitle("Grid");
  setWantsKeyboardFocus(true);
  for (int r = 0; r < 4; ++r)
    for (int s = 0; s < 8; ++s) {
      auto tile = std::make_unique<Tile>();
      tile->onClick = [this, r, s] {
        const GridSlot& slot = model.rows[static_cast<size_t>(r)].slots[static_cast<size_t>(s)];
        if (slot.empty) {
          if (onEmptySlot)
            onEmptySlot(r, s);
        } else if (onSelectBlock) {
          onSelectBlock(slot.blockId);
        }
      };
      tile->onPower = [this, r, s] {
        const GridSlot& slot = model.rows[static_cast<size_t>(r)].slots[static_cast<size_t>(s)];
        if (!slot.empty && onToggleBlock)
          onToggleBlock(slot.blockId, !slot.enabled);
      };
      auto mouse = std::make_unique<TileMouse>(*this, r, s);
      tile->addMouseListener(mouse.get(), false);
      tileMice.push_back(std::move(mouse));
      addChildComponent(*tile);
      tiles[static_cast<size_t>(r)][static_cast<size_t>(s)] = std::move(tile);
    }
}

GridView::~GridView() {
  // The listeners go before the tiles they listen to.
  for (int r = 0; r < 4; ++r)
    for (int s = 0; s < 8; ++s)
      tiles[static_cast<size_t>(r)][static_cast<size_t>(s)]->removeMouseListener(
          tileMice[static_cast<size_t>(r * 8 + s)].get());
}

int GridView::visibleIndexOf(int row) const {
  for (size_t i = 0; i < shown.size(); ++i)
    if (shown[i] == row)
      return static_cast<int>(i);
  return -1;
}

void GridView::setModel(const GridModel& newModel, const juce::String& selectedBlock,
                        std::optional<GridPoint> newSelectedPoint) {
  model = newModel;
  shown = model.visibleRows();
  selectedPoint = newSelectedPoint;
  for (int r = 0; r < 4; ++r) {
    const bool visible = visibleIndexOf(r) >= 0;
    for (int s = 0; s < 8; ++s) {
      Tile& tile = *tiles[static_cast<size_t>(r)][static_cast<size_t>(s)];
      tile.setVisible(visible);
      const GridSlot& slot = model.rows[static_cast<size_t>(r)].slots[static_cast<size_t>(s)];
      if (slot.empty) {
        tile.setEmpty();
      } else {
        Tile::State state = Tile::State::active;
        if (slot.missing)
          state = Tile::State::missing;
        else if (slot.loading)
          state = Tile::State::loading;
        else if (!slot.enabled || !slot.active)
          state = Tile::State::bypassed;
        tile.setBlock(slot.name, slot.category, state);
        tile.setFootswitch(slot.footswitch);
      }
      tile.setSelected(!slot.empty && ((slot.blockId == selectedBlock && !selectedPoint) || highlighted.contains(slot.blockId)));
    }
  }
  resized();
  repaint();
}

juce::Rectangle<int> GridView::addRowBounds() const {
  return {kInputX, rowTop(static_cast<int>(shown.size())) + 16, 1320, 44};
}

void GridView::resized() {
  for (int r = 0; r < 4; ++r) {
    const int visible = visibleIndexOf(r);
    for (int s = 0; s < 8; ++s)
      if (visible >= 0)
        tiles[static_cast<size_t>(r)][static_cast<size_t>(s)]->setBounds(kFirstSlotX + s * kSlotPitch, rowTop(visible),
                                                                          kSlotWidth, kSlotHeight);
  }
}

void GridView::setLevels(const std::array<RowLevels, 4>& now) {
  const juce::uint32 time = juce::Time::getMillisecondCounter();
  for (size_t r = 0; r < 4; ++r) {
    for (size_t c = 0; c < 2; ++c) {
      // The peak holds fall about 12 dB a second at ten readings a second.
      holds[r].in[c] = juce::jmax(now[r].in[c], holds[r].in[c] - 1.2f);
      holds[r].out[c] = juce::jmax(now[r].out[c], holds[r].out[c] - 1.2f);
      // A level at full scale shows as a clip for two seconds.
      if (now[r].in[c] >= -0.5f)
        clipUntil[r][0] = time + 2000;
      if (now[r].out[c] >= -0.5f)
        clipUntil[r][1] = time + 2000;
    }
  }
  targets = now;
  if (!motion::enabled()) {
    levels = now;
    timerCallback();
    return;
  }
  if (!isTimerRunning())
    startTimerHz(30);
}

void GridView::timerCallback() {
  // Up at once, down at 24 dB a second (0.8 dB a frame at 30 frames).
  bool settled = true;
  const auto follow = [&settled](float& shown, float target) {
    shown = target >= shown ? target : juce::jmax(target, shown - 0.8f);
    settled = settled && shown == target;
  };
  for (size_t r = 0; r < 4; ++r)
    for (size_t c = 0; c < 2; ++c) {
      follow(levels[r].in[c], targets[r].in[c]);
      follow(levels[r].out[c], targets[r].out[c]);
    }
  if (settled)
    stopTimer();
  const int outputX = kFirstSlotX + 8 * kSlotPitch;
  for (size_t v = 0; v < shown.size(); ++v) {
    repaint(kInputX, rowTop(static_cast<int>(v)), kInputWidth, kSlotHeight);
    repaint(outputX, rowTop(static_cast<int>(v)), kOutputWidth, kSlotHeight);
  }
}

GridView::Hover GridView::hoverAt(juce::Point<float> position) const {
  const auto [row, part] = levelPartAt(position);
  switch (part) {
    case LevelPart::trim: return {row, HoverPart::trim};
    case LevelPart::volume: return {row, HoverPart::volume};
    case LevelPart::mute: return {row, HoverPart::mute};
    case LevelPart::solo: return {row, HoverPart::solo};
    case LevelPart::none: break;
  }
  juce::Rectangle<int> box;
  if (const auto input = rowInputAt(position.toInt(), &box))
    return {*input, HoverPart::input};
  if (const auto output = rowOutputAt(position.toInt(), &box))
    return {*output, HoverPart::output};
  if (shown.size() < 4 && addRowBounds().toFloat().contains(position))
    return {-1, HoverPart::addRow};
  return {};
}

juce::Rectangle<float> GridView::inputMeterArea(int visibleIndex) const {
  return {kInputX + 10.0f, rowTop(visibleIndex) + 54.0f, kInputWidth - 20.0f, 8.0f};
}

juce::Rectangle<float> GridView::outputMeterArea(int visibleIndex) const {
  const float x = kFirstSlotX + 8.0f * kSlotPitch + 10.0f;
  return {x, rowTop(visibleIndex) + 40.0f, kOutputWidth - 20.0f - 72.0f, 8.0f};
}

juce::Rectangle<float> GridView::faderArea(int visibleIndex) const {
  const float x = kFirstSlotX + 8.0f * kSlotPitch + 10.0f;
  return {x, rowTop(visibleIndex) + 52.0f, kOutputWidth - 20.0f - 72.0f, 14.0f};
}

juce::Rectangle<float> GridView::muteSoloBox(int visibleIndex, int which) const {
  const float right = kFirstSlotX + 8.0f * kSlotPitch + kOutputWidth - 10.0f;
  return {right - 54.0f + which * 30.0f, rowTop(visibleIndex) + 10.0f, 24.0f, 24.0f};
}

std::pair<int, GridView::LevelPart> GridView::levelPartAt(juce::Point<float> position) const {
  for (size_t v = 0; v < shown.size(); ++v) {
    const int visible = static_cast<int>(v);
    const GridRow& row = model.rows[static_cast<size_t>(shown[v])];
    if (row.source == "device" && inputMeterArea(visible).expanded(0.0f, 6.0f).contains(position))
      return {row.index, LevelPart::trim};
    if (row.sink != "device")
      continue;
    if (faderArea(visible).expanded(4.0f, 2.0f).contains(position))
      return {row.index, LevelPart::volume};
    if (muteSoloBox(visible, 0).contains(position))
      return {row.index, LevelPart::mute};
    if (muteSoloBox(visible, 1).contains(position))
      return {row.index, LevelPart::solo};
  }
  return {-1, LevelPart::none};
}

void GridView::paintMeterPair(juce::Graphics& g, juce::Rectangle<float> area, const std::array<float, 2>& db,
                              const std::array<float, 2>& hold, bool clipped) const {
  // Left on top, right under it; a mono row shows the same level twice.
  for (int c = 0; c < 2; ++c)
    LevelMeter::paintBar(g, area.withHeight(3.0f).translated(0.0f, c * 5.0f), db[static_cast<size_t>(c)],
                         hold[static_cast<size_t>(c)]);
  if (clipped) {
    g.setColour(colour::clip);
    g.drawRoundedRectangle(area.expanded(2.0f), 3.0f, 1.0f);
  }
}

juce::Point<float> GridView::pointPosition(GridPoint p) const {
  const GridSplit* split = model.splitOf(p.row);
  const int visible = visibleIndexOf(p.row);
  if (split == nullptr || visible < 0)
    return {-100.0f, -100.0f};
  const int boundary = p.mix ? split->mixAt.value_or(8) : split->splitAt;
  return {boundaryX(boundary), rowTop(visible) + kSlotHeight * 0.5f};
}

std::optional<GridPoint> GridView::pointAt(juce::Point<float> position) const {
  for (const GridSplit& split : model.splits)
    for (const bool mix : {false, true}) {
      if (mix && !split.mixAt)
        continue;
      const GridPoint p{split.row, mix};
      if (pointPosition(p).getDistanceFrom(position) <= kPointGrab)
        return p;
    }
  return std::nullopt;
}

std::vector<int> GridView::legalBoundaries(GridPoint p) const {
  // A fork stays before its rejoin, a rejoin after its fork; the engine
  // refuses the rest (a split of the lower row has to stay inside the one
  // above) and says why.
  std::vector<int> legal;
  const GridSplit* split = model.splitOf(p.row);
  if (split == nullptr)
    return legal;
  if (p.mix) {
    for (int b = split->splitAt + 1; b <= 8; ++b)
      if (b != *split->mixAt)
        legal.push_back(b);
  } else {
    const int end = split->mixAt.value_or(8);
    for (int b = 0; b < end; ++b)
      if (b != split->splitAt)
        legal.push_back(b);
  }
  return legal;
}

GridView::Cable GridView::cableAt(juce::Point<float> position) const {
  // The gaps between two slots (boundaries 1 to 7) of a row with its own
  // signal (not a split's lower row: its cable is the split's path).
  for (size_t i = 0; i < shown.size(); ++i) {
    const GridRow& row = model.rows[static_cast<size_t>(shown[i])];
    if (model.splitInto(row.index) != nullptr)
      continue;
    const float centre = rowTop(static_cast<int>(i)) + kSlotHeight * 0.5f;
    if (std::abs(position.y - centre) > kCableGrab)
      continue;
    for (int b = 1; b < 8; ++b)
      if (std::abs(position.x - boundaryX(b)) < (kSlotPitch - kSlotWidth) * 0.5f)
        return {row.index, b};
  }
  return {};
}

std::optional<std::pair<int, int>> GridView::slotAt(juce::Point<int> position) const {
  for (size_t i = 0; i < shown.size(); ++i)
    for (int s = 0; s < 8; ++s)
      if (juce::Rectangle<int>(kFirstSlotX + s * kSlotPitch, rowTop(static_cast<int>(i)), kSlotWidth, kSlotHeight)
              .contains(position))
        return std::make_pair(shown[i], s);
  // The row under the last one shown: a block dropped there splits its row.
  const int below = static_cast<int>(shown.size());
  if (below < 4)
    for (int s = 0; s < 8; ++s)
      if (juce::Rectangle<int>(kFirstSlotX + s * kSlotPitch, rowTop(below), kSlotWidth, kSlotHeight).contains(position))
        for (int r = 0; r < 4; ++r)
          if (visibleIndexOf(r) < 0)
            return std::make_pair(r, s);
  return std::nullopt;
}

void GridView::paint(juce::Graphics& g) {
  g.fillAll(colour::ground);
  g.setColour(colour::line);
  g.fillRect(0, getHeight() - 1, getWidth(), 1);

  const float lineStart = static_cast<float>(kInputX + kInputWidth);
  const float outputX = static_cast<float>(kFirstSlotX + 8 * kSlotPitch);

  for (size_t i = 0; i < shown.size(); ++i) {
    const GridRow& row = model.rows[static_cast<size_t>(shown[i])];
    const int top = rowTop(static_cast<int>(i));
    const float centre = top + kSlotHeight * 0.5f;
    const GridSplit* into = model.splitInto(row.index);

    // The row's cable, under its tiles. A split's lower row has the split's
    // path instead (drawn below).
    if (into == nullptr) {
      g.setColour(kCable);
      g.fillRect(juce::Rectangle<float>(lineStart, centre - 1.0f, outputX - lineStart, 2.0f));
    }

    // The input box.
    const auto input = juce::Rectangle<float>(kInputX, top, kInputWidth, kSlotHeight);
    const bool ownInput = row.source == "device";
    g.setColour(colour::surface);
    g.fillRoundedRectangle(input, size::panelRadius);
    const bool inputLit = hovering.row == row.index && hovering.part == HoverPart::input;
    if (inputLit) {
      g.setColour(colour::raised);
      g.fillRoundedRectangle(input, size::panelRadius);
    }
    if (ownInput) {
      g.setColour(inputLit ? colour::dim : colour::line);
      g.drawRoundedRectangle(input.reduced(0.5f), size::panelRadius, 1.0f);
      // A chevron says the box opens a menu.
      if (inputLit)
        icons::draw(g, icons::path(icons::Icon::chevronDown),
                    juce::Rectangle<float>(12.0f, 12.0f).withPosition(input.getRight() - 20.0f, input.getY() + 30.0f),
                    colour::muted, 2.0f);
    } else {
      paint::dashedOutline(g, input, size::panelRadius, colour::line);
    }
    g.setFont(type::sans(11.0f, type::Weight::semibold).withExtraKerningFactor(0.08f));
    g.setColour(colour::muted);
    g.drawText("ROW " + juce::String(row.index + 1) + (ownInput ? " IN" : ""),
               input.reduced(10.0f).withHeight(14.0f), juce::Justification::centredLeft, true);
    g.setFont(type::sans(13.0f, type::Weight::semibold));
    g.setColour(ownInput ? colour::text : colour::muted);
    g.drawText(row.inputName, input.reduced(10.0f).withTrimmedTop(16.0f).withHeight(22.0f),
               juce::Justification::centredLeft, true);
    // What enters the row, and its trim (drag the meter to change it).
    const juce::uint32 time = juce::Time::getMillisecondCounter();
    const auto& level = levels[static_cast<size_t>(row.index)];
    const auto& hold = holds[static_cast<size_t>(row.index)];
    if ((hovering.row == row.index && hovering.part == HoverPart::trim) ||
        (levelDrag && levelDrag->row == row.index && levelDrag->part == LevelPart::trim)) {
      g.setColour(colour::dim);
      g.drawRoundedRectangle(inputMeterArea(static_cast<int>(i)).expanded(3.0f, 3.0f), 4.0f, 1.0f);
    }
    paintMeterPair(g, inputMeterArea(static_cast<int>(i)), level.in, hold.in,
                   time < clipUntil[static_cast<size_t>(row.index)][0]);
    // The trim shows once it is not 0 dB.
    if (ownInput && std::abs(row.inputTrimDb) > 0.05) {
      g.setFont(type::mono(10.0f));
      g.setColour(colour::iris);
      g.drawText((row.inputTrimDb > 0.05 ? "+" : "") + juce::String(row.inputTrimDb, 1),
                 input.reduced(10.0f).withHeight(14.0f), juce::Justification::centredRight, false);
    }

    // The output box: a device pair with mute, solo and volume, or where
    // the row goes instead.
    const auto output = juce::Rectangle<float>(outputX, top, kOutputWidth, kSlotHeight);
    if (row.sink == "device") {
      paint::panel(g, output);
      auto inner = output.reduced(10.0f);
      const auto nameLine = inner.withHeight(24.0f).withTrimmedRight(64.0f);
      if (hovering.row == row.index && hovering.part == HoverPart::output) {
        g.setColour(colour::control);
        g.fillRoundedRectangle(nameLine.expanded(4.0f, 0.0f), 6.0f);
        icons::draw(g, icons::path(icons::Icon::chevronDown),
                    juce::Rectangle<float>(12.0f, 12.0f).withCentre({nameLine.getRight() - 6.0f, nameLine.getCentreY()}),
                    colour::muted, 2.0f);
      }
      g.setFont(type::sans(13.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(row.outputName, inner.withHeight(24.0f), juce::Justification::centredLeft, true);
      for (int b = 0; b < 2; ++b) {
        const auto box = muteSoloBox(static_cast<int>(i), b);
        const bool on = b == 0 ? row.outputMute : row.outputSolo;
        g.setColour(on ? (b == 0 ? colour::warn : colour::iris) : colour::raised);
        g.fillRoundedRectangle(box, 6.0f);
        const bool boxLit = hovering.row == row.index &&
                            hovering.part == (b == 0 ? HoverPart::mute : HoverPart::solo);
        g.setColour(boxLit ? colour::dim : colour::line);
        g.drawRoundedRectangle(box.reduced(0.5f), 6.0f, 1.0f);
        g.setFont(type::sans(11.0f, type::Weight::bold));
        g.setColour(on ? colour::ground : colour::muted);
        g.drawText(b == 0 ? "M" : "S", box, juce::Justification::centred, false);
      }
      const float textWidth = 64.0f;
      // What the row sends, with its peak in dB.
      const auto meter = outputMeterArea(static_cast<int>(i));
      const bool clipped = time < clipUntil[static_cast<size_t>(row.index)][1];
      paintMeterPair(g, meter, level.out, hold.out, clipped);
      const float peak = juce::jmax(hold.out[0], hold.out[1]);
      g.setFont(type::mono(11.0f));
      g.setColour(clipped ? colour::clip : colour::muted);
      g.drawText(clipped ? juce::String("CLIP") : peak <= -60.0f ? juce::String("-inf") : juce::String(peak, 1),
                 juce::Rectangle<float>(inner.getRight() - textWidth, meter.getCentreY() - 7.0f, textWidth, 14.0f),
                 juce::Justification::centredRight, false);
      // The volume, -60 to +12 dB along the fader (drag it; double-click for 0 dB).
      const auto fader = faderArea(static_cast<int>(i));
      const auto bar = fader.withSizeKeepingCentre(fader.getWidth(), 4.0f);
      g.setColour(colour::line);
      g.fillRoundedRectangle(bar, 2.0f);
      const float filled = juce::jlimit(0.0f, 1.0f, static_cast<float>((row.outputVolumeDb + 60.0) / 72.0));
      g.setColour(colour::muted);
      g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * filled), 2.0f);
      // The unity mark, then the handle.
      g.setColour(colour::line);
      g.fillRect(juce::Rectangle<float>(bar.getX() + bar.getWidth() * (60.0f / 72.0f) - 0.5f, fader.getY() + 2.0f, 1.0f,
                                        fader.getHeight() - 4.0f));
      const bool faderLit = (hovering.row == row.index && hovering.part == HoverPart::volume) ||
                            (levelDrag && levelDrag->row == row.index && levelDrag->part == LevelPart::volume);
      const float handle = faderLit ? 13.0f : 10.0f;
      if (faderLit) {
        g.setColour(colour::text.withAlpha(0.15f));
        g.fillEllipse(juce::Rectangle<float>(handle + 8.0f, handle + 8.0f)
                          .withCentre({bar.getX() + bar.getWidth() * filled, bar.getCentreY()}));
      }
      g.setColour(colour::text);
      g.fillEllipse(juce::Rectangle<float>(handle, handle).withCentre({bar.getX() + bar.getWidth() * filled, bar.getCentreY()}));
      g.setFont(type::mono(11.0f));
      g.setColour(colour::muted);
      g.drawText(row.outputVolumeDb <= -59.95 ? juce::String("Off") : signedDb(row.outputVolumeDb),
                 juce::Rectangle<float>(inner.getRight() - textWidth, fader.getY(), textWidth, fader.getHeight()),
                 juce::Justification::centredRight, false);
    } else {
      paint::dashedOutline(g, output, size::panelRadius, colour::line);
      g.setFont(type::compact());
      g.setColour(colour::muted);
      g.drawText(row.outputName, output.reduced(10.0f), juce::Justification::centredLeft, true);
    }
  }

  // Splits: the lower path leaves the row at splitAt and, with a mix point,
  // comes back at mixAt; behind the tiles.
  for (const GridSplit& split : model.splits) {
    const int upper = visibleIndexOf(split.row);
    const int lower = visibleIndexOf(split.lowerRow);
    if (upper < 0 || lower < 0)
      continue;
    const float y1 = rowTop(upper) + kSlotHeight * 0.5f;
    const float y2 = rowTop(lower) + kSlotHeight * 0.5f;
    const float x1 = boundaryX(split.splitAt);
    const float x2 = split.mixAt ? boundaryX(*split.mixAt) : outputX;
    juce::Path path;
    path.startNewSubPath(x1, y1);
    path.lineTo(x1, y2 - 16.0f);
    path.quadraticTo(x1, y2, x1 + 16.0f, y2);
    if (split.mixAt) {
      path.lineTo(x2 - 16.0f, y2);
      path.quadraticTo(x2, y2, x2, y2 - 16.0f);
      path.lineTo(x2, y1);
    } else {
      path.lineTo(x2, y2);
    }
    g.setColour(kCable);
    g.strokePath(path, juce::PathStrokeType(2.0f));
  }

  // Add a row, while one is left to add.
  if (shown.size() < 4) {
    const auto area = addRowBounds().toFloat();
    const bool lit = hovering.part == HoverPart::addRow;
    if (lit) {
      g.setColour(colour::surface);
      g.fillRoundedRectangle(area, size::panelRadius);
    }
    paint::dashedOutline(g, area, size::panelRadius, lit ? colour::dim : colour::line);
    auto inner = addRowBounds().reduced(16, 0);
    icons::draw(g, icons::path(icons::Icon::plus), inner.removeFromLeft(18).toFloat().withSizeKeepingCentre(18.0f, 18.0f),
                lit ? colour::text : colour::muted);
    inner.removeFromLeft(10);
    g.setFont(type::button());
    g.setColour(lit ? colour::text : colour::muted);
    g.drawText("Add a row", inner, juce::Justification::centredLeft, false);
    g.setFont(type::sans(12.0f));
    g.drawText("For a second input or a separate path. Rows also appear when you split a path.", inner,
               juce::Justification::centredRight, false);
  }
}

void GridView::paintOverChildren(juce::Graphics& g) {
  // The split and mix points sit in the gaps, over the cables.
  for (const GridSplit& split : model.splits)
    for (const bool mix : {false, true}) {
      if (mix && !split.mixAt)
        continue;
      const GridPoint p{split.row, mix};
      if (draggingPoint && *draggingPoint == p)
        continue;
      const auto dot = juce::Rectangle<float>(14.0f, 14.0f).withCentre(pointPosition(p));
      g.setColour(colour::text);
      g.fillEllipse(dot);
      g.setColour(colour::ground);
      g.drawEllipse(dot.reduced(1.5f), 3.0f);
      if (selectedPoint && *selectedPoint == p) {
        g.setColour(colour::iris);
        g.drawEllipse(dot.expanded(3.0f), 2.0f);
      }
    }

  // A point on the move: dashed rings where it may land, the dot under the
  // mouse, and the hint the board shows.
  if (draggingPoint) {
    const float y = pointPosition(*draggingPoint).y;
    for (const int b : legalBoundaries(*draggingPoint)) {
      const auto ring = juce::Rectangle<float>(18.0f, 18.0f).withCentre({boundaryX(b), y});
      g.setColour(colour::ground);
      g.fillEllipse(ring);
      paint::dashedOutline(g, ring, 9.0f, colour::iris, 2.0f);
    }
    const auto dot = juce::Rectangle<float>(14.0f, 14.0f).withCentre({dragPosition.x, y});
    g.setColour(colour::text);
    g.fillEllipse(dot);
    g.setColour(colour::iris);
    g.drawEllipse(dot.expanded(3.0f), 2.0f);
    const juce::String hint = "Drag the dot to a dashed ring";
    const juce::Font hintFont = type::sans(12.0f, type::Weight::semibold);
    const auto tip = juce::Rectangle<float>(juce::GlyphArrangement::getStringWidth(hintFont, hint) + 20.0f, 28.0f)
                         .withCentre({dragPosition.x, y - 34.0f});
    g.setColour(colour::iris);
    g.fillRoundedRectangle(tip, size::buttonRadius);
    g.setFont(hintFont);
    g.setColour(colour::ground);
    g.drawText(hint, tip, juce::Justification::centred, false);
  }

  // The cable's plus, where the mouse is.
  if (hoverCable.row >= 0 && !draggingPoint && !draggingTile) {
    const int visible = visibleIndexOf(hoverCable.row);
    const auto plus = juce::Rectangle<float>(20.0f, 20.0f)
                          .withCentre({boundaryX(hoverCable.boundary), rowTop(visible) + kSlotHeight * 0.5f});
    g.setColour(colour::iris);
    g.fillEllipse(plus);
    icons::draw(g, icons::path(icons::Icon::plus), plus.reduced(4.0f), colour::ground, 2.5f);
  }

  // A block on the move: its ghost follows the mouse; the slot under it is
  // marked, and a drop on the free row below says what it will do.
  if (draggingTile) {
    if (const auto target = slotAt(tileDragPosition)) {
      const int visible = visibleIndexOf(target->first);
      const int top = visible >= 0 ? rowTop(visible) : rowTop(static_cast<int>(shown.size()));
      const auto slot = juce::Rectangle<float>(kFirstSlotX + target->second * kSlotPitch, top, kSlotWidth, kSlotHeight);
      paint::dashedOutline(g, slot.expanded(3.0f), size::panelRadius + 2.0f, colour::iris, 2.0f);
      if (visible < 0) {
        g.setFont(type::sans(12.0f, type::Weight::semibold));
        g.setColour(colour::iris);
        g.drawText("Drop to run in parallel", slot.translated(0.0f, kSlotHeight + 4.0f).withHeight(16.0f).expanded(60.0f, 0.0f),
                   juce::Justification::centred, false);
      }
    }
    const auto& from = model.rows[static_cast<size_t>(draggingTile->first)].slots[static_cast<size_t>(draggingTile->second)];
    const auto ghost = juce::Rectangle<float>(kSlotWidth, kSlotHeight).withCentre(tileDragPosition.toFloat());
    g.setColour(categoryTint(from.category).withAlpha(0.85f));
    g.fillRoundedRectangle(ghost, size::panelRadius);
    g.setFont(type::sans(12.0f, type::Weight::semibold));
    g.setColour(colour::text);
    g.drawText(from.name, ghost.reduced(8.0f), juce::Justification::bottomLeft, true);
  }
}

void GridView::mouseMove(const juce::MouseEvent& event) {
  const Hover now = hoverAt(event.position);
  if (now != hovering) {
    hovering = now;
    repaint();
  }
  if (now.part == HoverPart::trim || now.part == HoverPart::volume) {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    return;
  }
  if (now.part != HoverPart::none) {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    return;
  }
  const bool onPoint = pointAt(event.position).has_value();
  const Cable cable = onPoint ? Cable{} : cableAt(event.position);
  if (cable.row != hoverCable.row || cable.boundary != hoverCable.boundary) {
    hoverCable = cable;
    repaint();
  }
  setMouseCursor(onPoint || cable.row >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void GridView::mouseExit(const juce::MouseEvent&) {
  if (hoverCable.row >= 0 || hovering.part != HoverPart::none) {
    hoverCable = {};
    hovering = {};
    repaint();
  }
}

void GridView::mouseDown(const juce::MouseEvent& event) {
  grabKeyboardFocus();
  if (const auto p = pointAt(event.position)) {
    if (onSelectPoint)
      onSelectPoint(*p);
    draggingPoint = p;
    dragPosition = event.position;
    repaint();
    return;
  }
  const Cable cable = cableAt(event.position);
  if (cable.row >= 0) {
    showCableMenu(cable);
    return;
  }
  const auto [levelRow, part] = levelPartAt(event.position);
  if (part != LevelPart::none) {
    const GridRow& row = model.rows[static_cast<size_t>(levelRow)];
    if (part == LevelPart::mute || part == LevelPart::solo) {
      if (onRowMix)
        onRowMix(levelRow, object({{part == LevelPart::mute ? "mute" : "solo",
                                    !(part == LevelPart::mute ? row.outputMute : row.outputSolo)}}));
      return;
    }
    const bool trim = part == LevelPart::trim;
    if (event.getNumberOfClicks() > 1) {
      // A double-click puts the level back at 0 dB.
      if (trim && onRowTrim)
        onRowTrim(levelRow, 0.0);
      else if (!trim && onRowMix)
        onRowMix(levelRow, object({{"volumeDb", 0.0}}));
      return;
    }
    levelDrag = LevelDrag{levelRow, part, trim ? row.inputTrimDb : row.outputVolumeDb, event.position.x};
    return;
  }
  juce::Rectangle<int> box;
  if (const auto row = rowInputAt(event.getPosition(), &box)) {
    if (onRowInput)
      onRowInput(*row, box);
  } else if (const auto outRow = rowOutputAt(event.getPosition(), &box)) {
    if (onRowOutput)
      onRowOutput(*outRow, box);
  }
}

std::optional<int> GridView::rowInputAt(juce::Point<int> position, juce::Rectangle<int>* box) const {
  for (size_t v = 0; v < shown.size(); ++v) {
    const juce::Rectangle<int> input(kInputX, rowTop(static_cast<int>(v)), kInputWidth, kSlotHeight);
    if (input.contains(position)) {
      *box = input;
      return shown[v];
    }
  }
  return std::nullopt;
}

std::optional<int> GridView::rowOutputAt(juce::Point<int> position, juce::Rectangle<int>* box) const {
  // The name line only: the mute and solo boxes sit at its right end.
  const int outputX = kFirstSlotX + 8 * kSlotPitch;
  for (size_t v = 0; v < shown.size(); ++v) {
    const int row = shown[v];
    const juce::String sink = model.rows[static_cast<size_t>(row)].sink;
    if (sink == "mix")
      continue;
    const int top = rowTop(static_cast<int>(v));
    const juce::Rectangle<int> output(outputX, top, kOutputWidth, kSlotHeight);
    const juce::Rectangle<int> name = sink == "device" ? output.reduced(10).withHeight(24).withTrimmedRight(64) : output;
    if (name.contains(position)) {
      *box = output;
      return row;
    }
  }
  return std::nullopt;
}

void GridView::mouseDrag(const juce::MouseEvent& event) {
  if (levelDrag) {
    // Trim: 48 dB across the meter; volume: 72 dB across the fader. Shift
    // for fine steps.
    const bool trim = levelDrag->part == LevelPart::trim;
    const float width = trim ? kInputWidth - 20.0f : kOutputWidth - 92.0f;
    const double range = trim ? 48.0 : 72.0;
    const double fine = event.mods.isShiftDown() ? 0.1 : 1.0;
    const double moved = (event.position.x - levelDrag->startX) / width * range * fine;
    const double value = std::round(juce::jlimit(trim ? -24.0 : -60.0, trim ? 24.0 : 12.0, levelDrag->startDb + moved) * 10.0) / 10.0;
    const GridRow& row = model.rows[static_cast<size_t>(levelDrag->row)];
    if (std::abs(value - (trim ? row.inputTrimDb : row.outputVolumeDb)) < 0.05)
      return;
    if (trim && onRowTrim)
      onRowTrim(levelDrag->row, value);
    else if (!trim && onRowMix)
      onRowMix(levelDrag->row, object({{"volumeDb", value}}));
    return;
  }
  if (draggingPoint) {
    dragPosition = event.position;
    repaint();
  }
}

void GridView::mouseUp(const juce::MouseEvent& event) {
  if (levelDrag) {
    levelDrag.reset();
    return;
  }
  if (draggingPoint) {
    const GridPoint p = *draggingPoint;
    draggingPoint.reset();
    repaint();
    // Dropped on a ring: the point moves there.
    if (event.getDistanceFromDragStart() > 4)
      for (const int b : legalBoundaries(p))
        if (std::abs(event.position.x - boundaryX(b)) <= kPointGrab && onMovePoint) {
          onMovePoint(p, b);
          return;
        }
    return;
  }
  if (shown.size() < 4 && addRowBounds().contains(event.getPosition()) && onAddRow)
    onAddRow();
}

bool GridView::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::escapeKey) {
    if (draggingPoint || draggingTile) {
      cancelDrags();
      return true;
    }
    if (onClearSelection) {
      onClearSelection();
      return true;
    }
    return false;
  }
  if (key.getModifiers().isAnyModifierKeyDown() && !key.getModifiers().isShiftDown())
    return false;
  int dx = 0, dy = 0;
  if (key == juce::KeyPress::leftKey)
    dx = -1;
  else if (key == juce::KeyPress::rightKey)
    dx = 1;
  else if (key == juce::KeyPress::upKey)
    dy = -1;
  else if (key == juce::KeyPress::downKey)
    dy = 1;
  else
    return false;
  if (onMoveSelection)
    onMoveSelection(dx, dy);
  return true;
}

void GridView::cancelDrags() {
  if (draggingTile && onTileDrag)
    onTileDrag(false, false);
  draggingPoint.reset();
  draggingTile.reset();
  repaint();
}

void GridView::showCableMenu(Cable cable) {
  // The board "Add a split: from the cable": insert a block at that point,
  // or split the path there into the row below.
  juce::PopupMenu menu;
  const GridRow& row = model.rows[static_cast<size_t>(cable.row)];
  const bool slotFree = row.slots[static_cast<size_t>(cable.boundary)].empty;
  menu.addItem(juce::String::fromUTF8("Insert block here\xe2\x80\xa6"), slotFree, false, [this, cable] {
    if (onInsertHere)
      onInsertHere(cable.row, cable.boundary);
  });
  const bool canSplit = cable.row < 3 && model.splitOf(cable.row) == nullptr &&
                        model.rows[static_cast<size_t>(cable.row + 1)].source != "device";
  menu.addItem("Split path here", canSplit, false, [this, cable] {
    if (onSplitHere)
      onSplitHere(cable.row, cable.boundary);
  });
  const juce::Rectangle<int> at(juce::roundToInt(boundaryX(cable.boundary)),
                                rowTop(visibleIndexOf(cable.row)) + kSlotHeight / 2, 1, 1);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(at)));
}

void GridView::tileMouseDown(int row, int slot, const juce::MouseEvent& event) {
  const GridSlot& s = model.rows[static_cast<size_t>(row)].slots[static_cast<size_t>(slot)];
  if (s.empty || !event.mods.isPopupMenu())
    return;
  juce::PopupMenu menu;
  const juce::String id = s.blockId;
  menu.addItem("Duplicate", [this, id] {
    if (onDuplicate)
      onDuplicate(id);
  });
  menu.addItem("Remove", [this, id] {
    if (onRemove)
      onRemove(id);
  });
  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(tiles[static_cast<size_t>(row)][static_cast<size_t>(slot)].get()));
}

void GridView::tileMouseDrag(int row, int slot, const juce::MouseEvent& event) {
  const GridSlot& s = model.rows[static_cast<size_t>(row)].slots[static_cast<size_t>(slot)];
  if (s.empty || event.mods.isPopupMenu() || event.getDistanceFromDragStart() < 6)
    return;
  draggingTile = std::make_pair(row, slot);
  tileDragPosition = event.getEventRelativeTo(this).getPosition();
  if (onTileDrag)
    onTileDrag(true, !getLocalBounds().contains(tileDragPosition));
  repaint();
}

void GridView::tileMouseUp(int, int, const juce::MouseEvent& event) {
  if (!draggingTile)
    return;
  const auto from = *draggingTile;
  draggingTile.reset();
  repaint();
  if (onTileDrag)
    onTileDrag(false, false);
  const GridSlot& moving = model.rows[static_cast<size_t>(from.first)].slots[static_cast<size_t>(from.second)];
  const juce::Point<int> dropped = event.getEventRelativeTo(this).getPosition();
  // Dropped off the grid (on the remove dock below it, or anywhere else):
  // the block goes. Undo brings it back.
  if (!getLocalBounds().contains(dropped)) {
    if (onRemove)
      onRemove(moving.blockId);
    return;
  }
  const auto target = slotAt(dropped);
  if (!target || *target == from)
    return;
  if (onMoveBlock)
    onMoveBlock(moving.blockId, target->first, target->second);
}

// ####################
// SPLIT PANEL
// ####################
namespace {
const juce::StringArray& splitTypes() {
  static const juce::StringArray types{"balance", "ab", "crossover"};
  return types;
}
}  // namespace

SplitPanel::SplitPanel(EngineClient& client) : engine(client) {
  setTitle("Split");
  typeChoice.setTitle("Splitter type");
  typeChoice.onChange = [this](int index) {
    engine.run("split.setType", object({{"row", point.row}, {"type", splitTypes()[index]}}));
    if (onChanged)
      onChanged();
  };
  muteToggle.onClick = [this] { setParam("mix.mute", muteToggle.getToggleState() ? 1.0 : 0.0); };
  secondToggle.onClick = [this] {
    setParam(point.mix ? "mix.invertB" : "split.stereo", secondToggle.getToggleState() ? 1.0 : 0.0);
  };
  remove.onClick = [this] {
    // Removing the mixer keeps the split; the lower path then runs to its
    // own output.
    if (point.mix)
      engine.run("split.move", object({{"row", point.row}, {"rejoin", false}}));
    else
      engine.run("split.remove", object({{"row", point.row}}));
    if (onChanged)
      onChanged();
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&typeChoice, &muteToggle, &secondToggle, &remove})
    addAndMakeVisible(c);
}

void SplitPanel::setParam(const juce::String& id, double value) {
  engine.run("split.setParam", object({{"row", point.row}, {"param", id}, {"value", value}}));
}

juce::String SplitPanel::blocksOf(const GridModel& model, int row, int from, int to) const {
  juce::StringArray names;
  for (int s = juce::jmax(0, from); s < juce::jmin(8, to); ++s) {
    const GridSlot& slot = model.rows[static_cast<size_t>(row)].slots[static_cast<size_t>(s)];
    if (!slot.empty)
      names.add(slot.name);
  }
  return names.isEmpty() ? juce::String("Nothing yet") : names.joinIntoString(" + ");
}

void SplitPanel::addKnob(const juce::String& id, const juce::String& name, int group) {
  for (const KnobParam& spec : specs) {
    if (spec.id != id)
      continue;
    Placed placed;
    placed.param = spec;
    placed.param.name = name;
    placed.group = group;
    placed.knob = std::make_unique<Knob>(name);
    placed.knob->setPosition(spec.toPosition(spec.value));
    placed.knob->setDefaultPosition(spec.toPosition(spec.defaultValue));
    placed.knob->setValueText(spec.text(spec.value));
    Knob* raw = placed.knob.get();
    const KnobParam param = placed.param;
    placed.knob->onChange = [this, raw, param](double position) {
      const double value = param.fromPosition(position);
      raw->setValueText(param.text(value));
      setParam(param.id, value);
    };
    addAndMakeVisible(*placed.knob);
    knobs.push_back(std::move(placed));
    return;
  }
}

void SplitPanel::showPoint(const GridModel& model, GridPoint newPoint) {
  point = newPoint;
  knobs.clear();
  specs.clear();
  const GridSplit* split = model.splitOf(point.row);
  if (split == nullptr)
    return;
  type = split->type;
  const juce::var listed = engine.read("split.listParams", object({{"row", point.row}}));
  if (const auto* list = listed["params"].getArray())
    for (const auto& item : *list)
      specs.push_back(KnobParam::fromVar(item));
  const auto valueOf = [this](const char* id) {
    for (const auto& spec : specs)
      if (spec.id == id)
        return spec.value;
    return 0.0;
  };

  const int boundary = point.mix ? split->mixAt.value_or(8) : split->splitAt;
  juce::String before;
  for (int s = boundary - 1; s >= 0 && before.isEmpty(); --s)
    if (!model.rows[static_cast<size_t>(point.row)].slots[static_cast<size_t>(s)].empty)
      before = model.rows[static_cast<size_t>(point.row)].slots[static_cast<size_t>(s)].name;
  const juce::String where = before.isEmpty() ? juce::String("at the start") : "after " + before;
  muteToggle.setToggleState(valueOf("mix.mute") >= 0.5, juce::dontSendNotification);
  if (point.mix) {
    title = "Mixer";
    subtitle = "Row " + juce::String(split->lowerRow + 1) + " back into row " + juce::String(point.row + 1) + " " +
               type::dot() + " " + where;
    secondLabel = "Invert phase of path B";
    secondToggle.setToggleState(valueOf("mix.invertB") >= 0.5, juce::dontSendNotification);
    remove.setButtonText("Remove mixer");
    typeChoice.setVisible(false);
    pathA = blocksOf(model, point.row, split->splitAt, boundary);
    pathB = blocksOf(model, split->lowerRow, split->splitAt, boundary);
    addKnob("mix.levelA", "Level", 0);
    addKnob("mix.panA", "Pan", 0);
    addKnob("mix.levelB", "Level", 1);
    addKnob("mix.panB", "Pan", 1);
    addKnob("mix.level", "Level", 2);
  } else {
    title = "Splitter";
    subtitle = "Row " + juce::String(point.row + 1) + " into row " + juce::String(split->lowerRow + 1) + " " +
               type::dot() + " " + where;
    secondLabel = "Stereo split (L to A, R to B)";
    secondToggle.setToggleState(valueOf("split.stereo") >= 0.5, juce::dontSendNotification);
    remove.setButtonText("Remove split");
    typeChoice.setVisible(true);
    typeChoice.setSelected(juce::jmax(0, splitTypes().indexOf(type)));
    if (type == "balance") {
      addKnob("split.balance", "Balance", 1);
    } else if (type == "ab") {
      addKnob("split.levelA", "Path A", 1);
      addKnob("split.levelB", "Path B", 1);
    } else {
      addKnob("split.crossoverHz", "Frequency", 1);
    }
  }
  resized();
  repaint();
}

void SplitPanel::paint(juce::Graphics& g) {
  g.fillAll(colour::surface);
  auto area = getLocalBounds().reduced(24, 20);
  auto head = area.removeFromTop(40);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(title, head.removeFromTop(26), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(subtitle, head, juce::Justification::centredLeft, true);

  g.setFont(type::compact());
  g.setColour(colour::text);
  for (Toggle* toggle : {&muteToggle, &secondToggle}) {
    const auto b = toggle->getBounds();
    g.drawText(toggle == &muteToggle ? juce::String("Mute") : secondLabel, b.getRight() + 6, b.getY(),
               toggle == &muteToggle ? 40 : 200, b.getHeight(), juce::Justification::centredLeft, false);
  }

  if (point.mix) {
    paintGroupBox(g, groups[0], "Path A " + type::dot() + " row " + juce::String(point.row + 1), pathA);
    paintGroupBox(g, groups[1], "Path B " + type::dot() + " row " + juce::String(point.row + 2), pathB);
    paintGroupBox(g, groups[2], "Output", "After the two paths are summed");
  } else {
    paint::label(g, "Type", groups[0].withHeight(16));
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawText("Path A stays on row " + juce::String(point.row + 1) + ", path B goes to row " +
                   juce::String(point.row + 2) + ".",
               groups[0].withTrimmedTop(66).withHeight(16), juce::Justification::centredLeft, false);
    const juce::String heading = type == "balance" ? "Balance" : type == "ab" ? "A/B" : "Crossover";
    const juce::String note = type == "balance" ? "Both paths at unity; one knob fades either"
                              : type == "ab"    ? "Level to each path"
                                                : "Splits by frequency: lows to A, highs to B";
    paintGroupBox(g, groups[1], heading, note);
  }
}

void SplitPanel::resized() {
  auto area = getLocalBounds().reduced(24, 20);
  auto head = area.removeFromTop(40);
  const int removeWidth = remove.idealWidth();
  remove.setBounds(head.getRight() - removeWidth, head.getCentreY() - 16, removeWidth, 32);
  secondToggle.setBounds(remove.getX() - 24 - 250, head.getCentreY() - 14, 44, 28);
  muteToggle.setBounds(secondToggle.getX() - 110, head.getCentreY() - 14, 44, 28);
  area.removeFromTop(20);
  if (point.mix) {
    const int width = (area.getWidth() - 2 * 16) / 3;
    for (int i = 0; i < 3; ++i)
      groups[static_cast<size_t>(i)] =
          area.withX(area.getX() + i * (width + 16)).withWidth(width).withHeight(juce::jmin(area.getHeight(), 200));
  } else {
    groups[0] = area.withWidth(380).withHeight(90);
    typeChoice.setBounds(groups[0].getX(), groups[0].getY() + 22, typeChoice.idealWidth(), 36);
    groups[1] = area.withTrimmedLeft(404).withHeight(juce::jmin(area.getHeight(), 200));
    groups[2] = {};
  }
  // Knobs in their group, under its title.
  std::array<int, 3> next{};
  for (auto& placed : knobs) {
    const auto& box = groups[static_cast<size_t>(placed.group)];
    const int column = next[static_cast<size_t>(placed.group)]++;
    placed.knob->setBounds(box.getX() + 16 + column * 96, box.getY() + 52, 84, 110);
  }
}

// ####################
// PAGE
// ####################
GridPage::GridPage(EngineClient& client)
    : engine(client), blockPanel(client), splitPanel(client), scenePanel(client), stompPanel(client), editor(client),
      library(client) {
  setTitle("Grid editor");
  addAndMakeVisible(scenes);
  addAndMakeVisible(grid);
  addAndMakeVisible(blockPanel);
  addChildComponent(splitPanel);
  addChildComponent(scenePanel);
  addChildComponent(stompPanel);
  addChildComponent(editor);
  scenes.onSelect = [this](const juce::String& scene) {
    engine.run("scene.select", object({{"scene", scene}}));
    // The scene's own panel, with its values and its blocks' states.
    panel = Panel::scene;
    rigChanged();
  };
  scenePanel.onSelectScene = scenes.onSelect;
  scenes.onAdd = [this] {
    ensureGrid();
    const EngineReply reply = engine.run("scene.add");
    // The new scene is where the next edits go: select it.
    if (reply.ok && reply.value["added"].toString().isNotEmpty())
      engine.run("scene.select", object({{"scene", reply.value["added"].toString()}}));
    panel = Panel::scene;
    rigChanged();
  };
  scenes.onRemove = [this](const juce::String& scene) {
    juce::PopupMenu menu;
    menu.addItem("Remove scene " + scene, [this, scene] {
      Dialog::Options options;
      options.title = "Remove scene " + scene + "?";
      options.message = "The scenes after it move down one place. Undo brings it back.";
      options.confirm = "Remove scene";
      juce::Component::SafePointer<GridPage> self(this);
      prompt.show(options, [self, scene](bool confirmed, const juce::String&) {
        if (self == nullptr || !confirmed)
          return;
        self->runRouting("scene.remove", object({{"scene", scene}}), "remove the scene");
      });
    });
    menu.showMenuAsync(juce::PopupMenu::Options());
  };
  scenePanel.onChanged = [this] { rigChanged(); };
  scenePanel.onOpenModes = [this] {
    if (onOpenModes)
      onOpenModes();
  };
  stompPanel.onChanged = [this] { rigChanged(); };
  stompPanel.onSelectionChanged = [this] {
    if (panel != Panel::stomp)
      return;
    grid.setHighlighted(stompPanel.selectedBlocks());
    grid.setModel(model, {}, std::nullopt);
  };
  stompPanel.onOpenModes = scenePanel.onOpenModes;
  grid.onSelectBlock = [this](const juce::String& blockId) { selectBlock(blockId); };
  grid.onMoveSelection = [this](int dx, int dy) { moveSelection(dx, dy); };
  grid.onClearSelection = [this] { clearSelection(); };
  grid.onSelectPoint = [this](GridPoint p) { selectPoint(p); };
  grid.onEmptySlot = [this](int row, int slot) { openPicker(row, slot); };
  grid.onInsertHere = [this](int row, int slot) { openPicker(row, slot); };
  addChildComponent(library);
  addChildComponent(prompt);
  grid.onRowInput = [this](int row, juce::Rectangle<int> at) { showRowInputMenu(row, at); };
  grid.onToggleBlock = [this](const juce::String& blockId, bool enabled) {
    engine.run("block.setEnabled", object({{"blockId", blockId}, {"enabled", enabled}}));
    rigChanged();
  };
  grid.onRowOutput = [this](int row, juce::Rectangle<int> at) { showRowOutputMenu(row, at); };
  // Trim and volume go to the engine as they are dragged (the history keeps
  // one step per gesture); the model is read again so the boxes follow.
  grid.onRowTrim = [this](int row, double trimDb) {
    engine.run("row.setInputStage", object({{"row", row}, {"trimDb", trimDb}}));
    rigChanged();
  };
  grid.onRowMix = [this](int row, const juce::var& mix) {
    juce::DynamicObject::Ptr args = new juce::DynamicObject();
    args->setProperty("row", row);
    if (const auto* fields = mix.getDynamicObject())
      for (const auto& field : fields->getProperties())
        args->setProperty(field.name, field.value);
    engine.run("row.setMix", juce::var(args.get()));
    rigChanged();
  };
  library.onClose = [this] { library.setVisible(false); };
  library.onAdded = [this](const juce::String& blockId) {
    library.setVisible(false);
    rigChanged();
    selectBlock(blockId);
    // A block replaced from its editor: the editor follows the new one.
    if (editorShown)
      openEditor(blockId);
  };
  grid.onSplitHere = [this](int row, int boundary) {
    ensureGrid();
    // An empty parallel path one slot long; the player moves its points.
    engine.run("split.add", object({{"row", row}, {"splitAt", boundary}, {"mixAt", juce::jmin(8, boundary + 1)}}));
    rigChanged();
    selectPoint({row, false});
  };
  grid.onMovePoint = [this](GridPoint p, int boundary) {
    engine.run("split.move", p.mix ? object({{"row", p.row}, {"mixAt", boundary}})
                                   : object({{"row", p.row}, {"splitAt", boundary}}));
    rigChanged();
  };
  grid.onMoveBlock = [this](const juce::String& blockId, int row, int slot) {
    ensureGrid();
    // Dropped on a row that is not shown yet, under a row with no split:
    // that row becomes a parallel path around the slot.
    const bool newRow = !model.rows[static_cast<size_t>(row)].routed && model.splitInto(row) == nullptr;
    if (newRow && row > 0 && model.splitOf(row - 1) == nullptr)
      engine.run("split.add", object({{"row", row - 1}, {"splitAt", slot}, {"mixAt", juce::jmin(8, slot + 1)}}));
    engine.run("grid.moveBlock", object({{"blockId", blockId}, {"row", row}, {"slot", slot}, {"mode", "swap"}}));
    rigChanged();
  };
  grid.onDuplicate = [this](const juce::String& blockId) {
    engine.run("chain.duplicateBlock", object({{"blockId", blockId}}));
    rigChanged();
  };
  grid.onRemove = [this](const juce::String& blockId) {
    engine.run("chain.removeBlock", object({{"blockId", blockId}}));
    rigChanged();
  };
  grid.onTileDrag = [this](bool dragging, bool offGrid) {
    removeDock.setVisible(dragging);
    removeDock.setArmed(offGrid);
    if (dragging)
      removeDock.toFront(false);
  };
  addChildComponent(removeDock);
  grid.onAddRow = [this] {
    ensureGrid();
    // The first row that does not run gets the next input and the first
    // output pair; the player changes either from the row's boxes.
    for (const auto& row : model.rows) {
      if (row.routed || model.splitInto(row.index) != nullptr)
        continue;
      engine.run("row.setInput", object({{"row", row.index}, {"source", "device"}, {"channel", row.index + 1}}));
      engine.run("row.setOutput", object({{"row", row.index}, {"target", "device"}, {"pair", 1}}));
      break;
    }
    rigChanged();
  };
  // Double-click: the settings are already on the page, so it selects and
  // makes sure they are showing. A hosted plugin has its own window.
  grid.onOpenBlock = [this](const juce::String& blockId) {
    const GridSlot* block = model.find(blockId);
    if (block != nullptr && block->kindId == "plugin" && onOpenPluginWindow) {
      requestEditor(blockId);
      return;
    }
    selectBlock(blockId);
    if (blockPanel.isCollapsed())
      setSettingsCollapsed(false);
  };
  blockPanel.onOpenEditor = [this](const juce::String& blockId) { requestEditor(blockId); };
  blockPanel.onCollapsedChanged = [this](bool collapsed) { setSettingsCollapsed(collapsed); };
  blockPanel.onReplace = [this](const juce::String& blockId) {
    int row = 0, slot = 0;
    if (model.find(blockId, &row, &slot) != nullptr)
      openPicker(row, slot);
  };
  blockPanel.onChanged = [this] { rigChanged(); };
  editor.onBack = [this] { closeEditor(); };
  editor.onOpenBlock = [this](const juce::String& blockId) { openEditor(blockId); };
  editor.onChanged = [this] { rigChanged(); };
  editor.onReplace = [this](int row, int slot, int source) {
    openPicker(row, slot);
    // The library opens on the kind of file being replaced.
    if (source != BlockPicker::all)
      library.setFilter({}, source, {});
  };
  splitPanel.onChanged = [this] { rigChanged(); };
  rigChanged();
}

void GridPage::ensureGrid() {
  if (model.gridMode)
    return;
  engine.run("grid.convert", object({{"to", "grid"}}));
  model = GridModel::read(engine);
}

// ####################
// ROW INPUTS AND OUTPUTS
// ####################
void GridPage::runRouting(const char* command, const juce::var& args, const juce::String& what) {
  const EngineReply reply = engine.run(command, args);
  rigChanged();
  if (reply.ok)
    return;
  Dialog::Options options;
  options.title = "Could not " + what;
  options.message = reply.errorMessage;
  options.cancelable = false;
  prompt.show(options, [](bool, const juce::String&) {});
}

void GridPage::showMeters(const juce::var& meters) {
  if (!grid.isShowing())
    return;
  blockPanel.showMeters(meters);
  std::array<GridView::RowLevels, 4> levels;
  if (const auto* rows = meters["rows"].getArray())
    for (const auto& row : *rows) {
      const int index = row["row"];
      if (index < 0 || index >= 4)
        continue;
      for (int c = 0; c < 2; ++c) {
        levels[static_cast<size_t>(index)].in[static_cast<size_t>(c)] = static_cast<float>(row["in"][c]);
        levels[static_cast<size_t>(index)].out[static_cast<size_t>(c)] = static_cast<float>(row["out"][c]);
      }
    }
  grid.setLevels(levels);
}

void GridPage::showRowInputMenu(int row, juce::Rectangle<int> at) {
  const GridRow& shownRow = model.rows[static_cast<size_t>(row)];
  const juce::var gridState = engine.read("grid.get");
  juce::var current;
  if (const auto* rows = gridState["rows"].getArray())
    if (row < rows->size())
      current = (*rows)[row]["input"];
  const bool none = current["type"].toString() != "device";
  const int channel = current["channel"];
  const bool stereo = static_cast<bool>(current["stereo"]);
  const bool summed = current["fold"].toString() == "sum";

  // The device's inputs by name, or eight plain ones.
  juce::StringArray names;
  if (deviceState != nullptr)
    if (const auto* channels = deviceState()["inputChannels"].getArray())
      for (const auto& c : *channels)
        names.add(c["name"].toString().isNotEmpty() ? c["name"].toString() : "Input " + juce::String(names.size() + 1));
  if (names.isEmpty())
    for (int i = 1; i <= 8; ++i)
      names.add("Input " + juce::String(i));

  juce::PopupMenu menu;
  menu.addSectionHeader("Row " + juce::String(row + 1) + " input");
  if (shownRow.source == "split" || shownRow.source == "rows") {
    menu.addItem(shownRow.source == "split" ? "Fed by the split above" : "Fed by another row", false, false, [] {});
  } else {
    menu.addItem("No input", true, none, [this, row] {
      runRouting("row.setInput", object({{"row", row}, {"source", "none"}}), "change the input");
    });
    menu.addSeparator();
    for (int i = 0; i < names.size(); ++i) {
      const int ch = i + 1;
      menu.addItem(names[i], true, !none && !stereo && channel == ch, [this, row, ch] {
        runRouting("row.setInput", object({{"row", row}, {"source", "device"}, {"channel", ch}}), "change the input");
      });
    }
    if (names.size() >= 2) {
      juce::PopupMenu pairs;
      for (int ch = 1; ch < names.size(); ch += 2)
        pairs.addItem("In " + juce::String(ch) + "/" + juce::String(ch + 1), true, !none && stereo && channel == ch,
                      [this, row, ch, summed] {
                        runRouting("row.setInput",
                                   object({{"row", row}, {"source", "device"}, {"channel", ch}, {"stereo", true},
                                           {"fold", summed ? "sum" : "none"}}),
                                   "change the input");
                      });
      menu.addSubMenu("Stereo pair", pairs);
    }
    if (!none && stereo)
      menu.addItem("Sum the pair to mono", true, summed, [this, row, channel, summed] {
        runRouting("row.setInput",
                   object({{"row", row}, {"source", "device"}, {"channel", channel}, {"stereo", true},
                           {"fold", summed ? "none" : "sum"}}),
                   "change the input");
      });
  }
  if (shownRow.source == "device") {
    // The row's own share of the input stage.
    menu.addSeparator();
    const bool gate = static_cast<bool>(current["gate"]);
    menu.addItem("Noise gate", true, gate, [this, row, gate] {
      runRouting("row.setInputStage", object({{"row", row}, {"gate", !gate}}), "change the noise gate");
    });
    const double trim = current["trimDb"];
    menu.addItem("Input trim " + signedDb(trim) + (std::abs(trim) > 0.05 ? ": reset to 0 dB" : " (drag the meter)"),
                 std::abs(trim) > 0.05, false, [this, row] {
                   runRouting("row.setInputStage", object({{"row", row}, {"trimDb", 0.0}}), "change the trim");
                 });
  }
  menu.addSeparator();
  int running = 0;
  for (const auto& r : model.rows)
    if (r.routed || model.splitInto(r.index) != nullptr)
      ++running;
  const bool lowerOfSplit = shownRow.source == "split";
  menu.addItem(lowerOfSplit ? "Remove row (remove the split instead)" : "Remove row", !lowerOfSplit && running > 1, false,
               [this, row] { removeRow(row); });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(grid.localAreaToGlobal(at)));
}

void GridPage::showRowOutputMenu(int row, juce::Rectangle<int> at) {
  const juce::var gridState = engine.read("grid.get");
  juce::var current;
  if (const auto* rows = gridState["rows"].getArray())
    if (row < rows->size())
      current = (*rows)[row]["output"];
  const juce::String type = current["type"].toString();
  const int pair = current["pair"];
  const int toRow = current.hasProperty("toRow") ? static_cast<int>(current["toRow"]) : -1;

  juce::StringArray pairs;
  if (deviceState != nullptr)
    if (const auto* list = deviceState()["outputPairs"].getArray())
      for (const auto& p : *list)
        pairs.add(p.toString());
  if (pairs.isEmpty())
    for (int p = 1; p <= 4; ++p)
      pairs.add("Out " + juce::String(p * 2 - 1) + "/" + juce::String(p * 2));

  juce::PopupMenu menu;
  menu.addSectionHeader("Row " + juce::String(row + 1) + " output");
  menu.addItem("Not routed", true, type != "device" && type != "row", [this, row] {
    runRouting("row.setOutput", object({{"row", row}, {"target", "none"}}), "change the output");
  });
  menu.addSeparator();
  for (int p = 0; p < juce::jmin(4, pairs.size()); ++p) {
    const int number = p + 1;
    menu.addItem(pairs[p], true, type == "device" && pair == number, [this, row, number] {
      runRouting("row.setOutput", object({{"row", row}, {"target", "device"}, {"pair", number}}), "change the output");
    });
  }
  menu.addSeparator();
  for (const auto& other : model.rows) {
    if (other.index == row || !(other.routed || model.splitInto(other.index) != nullptr))
      continue;
    const int target = other.index;
    menu.addItem("Into row " + juce::String(target + 1), true, type == "row" && toRow == target, [this, row, target] {
      runRouting("row.setOutput", object({{"row", row}, {"target", "row"}, {"toRow", target}}), "change the output");
    });
  }
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(grid.localAreaToGlobal(at)));
}

void GridPage::removeRow(int row) {
  Dialog::Options options;
  options.title = "Remove row " + juce::String(row + 1) + "?";
  options.message = "Its blocks are removed and the row stops running. Undo brings them back.";
  options.confirm = "Remove row";
  juce::Component::SafePointer<GridPage> self(this);
  prompt.show(options, [self, row](bool confirmed, const juce::String&) {
    if (self == nullptr || !confirmed)
      return;
    GridPage& page = *self;
    // A split that starts on this row goes first: its lower path is part of it.
    if (page.model.splitOf(row) != nullptr)
      page.engine.run("split.remove", object({{"row", row}}));
    for (const auto& slot : page.model.rows[static_cast<size_t>(row)].slots)
      if (!slot.empty)
        page.engine.run("chain.removeBlock", object({{"blockId", slot.blockId}}));
    // Rows that fed this one go to the first output pair instead.
    if (const auto* rows = page.engine.read("grid.get")["rows"].getArray())
      for (int other = 0; other < rows->size(); ++other) {
        const juce::var output = (*rows)[other]["output"];
        if (other != row && output["type"].toString() == "row" && static_cast<int>(output["toRow"]) == row)
          page.engine.run("row.setOutput", object({{"row", other}, {"target", "device"}, {"pair", 1}}));
      }
    page.engine.run("row.setOutput", object({{"row", row}, {"target", "none"}}));
    page.engine.run("row.setInput", object({{"row", row}, {"source", "none"}}));
    page.rigChanged();
  });
}

void RemoveDock::setArmed(bool shouldBeArmed) {
  if (armed == shouldBeArmed)
    return;
  armed = shouldBeArmed;
  repaint();
}

void RemoveDock::paint(juce::Graphics& g) {
  g.fillAll(colour::ground.withAlpha(0.88f));
  const auto dock = getLocalBounds().toFloat().reduced(24.0f, 20.0f);
  g.setColour(armed ? colour::clip.withAlpha(0.18f) : colour::surface);
  g.fillRoundedRectangle(dock, size::panelRadius);
  paint::dashedOutline(g, dock, size::panelRadius, armed ? colour::clip : colour::line, armed ? 2.0f : 1.0f);
  const juce::String text = armed ? "Release to remove the block" : "Drop here to remove";
  g.setFont(type::button());
  const float width = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 30.0f;
  auto line = dock.withSizeKeepingCentre(width, 24.0f);
  icons::draw(g, icons::path(icons::Icon::trash), line.removeFromLeft(20.0f), armed ? colour::clip : colour::muted);
  line.removeFromLeft(10.0f);
  g.setColour(armed ? colour::clip : colour::muted);
  g.drawText(text, line, juce::Justification::centredLeft, false);
}

void GridPage::newPreset(bool askFirst) {
  const auto start = [self = juce::Component::SafePointer<GridPage>(this)] {
    if (self == nullptr)
      return;
    self->closeEditor();
    self->engine.run("chain.reset");
    self->rigChanged();
  };
  if (!askFirst) {
    start();
    return;
  }
  Dialog::Options options;
  options.title = "Start a new preset?";
  options.message = "The changes to this preset are not saved. Undo brings them back.";
  options.confirm = "New preset";
  prompt.show(options, [start](bool confirmed, const juce::String&) {
    if (confirmed)
      start();
  });
}

void GridPage::rigChanged() {
  model = GridModel::read(engine);
  if (!model.gridMode && !model.legacyEmpty) {
    // The editor works on the grid. A legacy rig with blocks (an older
    // preset) is converted once, which keeps every block where it was; an
    // empty one is converted at the first edit, so opening the app does
    // not leave the rig unsaved.
    ensureGrid();
  }
  if (point) {
    const GridSplit* split = model.splitOf(point->row);
    if (split == nullptr || (point->mix && !split->mixAt))
      point.reset();
  }
  // A block removed from under its editor (an agent, an undo) closes it.
  if (editorShown && model.find(editor.blockId()) == nullptr)
    setEditorShown(false);
  if (model.find(selected) == nullptr) {
    // The page opens on the first block; after Escape it stays empty until
    // the player picks one.
    selected = selectionCleared ? juce::String() : model.firstBlock();
  }
  scenes.setScenes(model.scenes);
  // A change of footswitch mode brings its panel: Scene mode the scene, Stomp
  // mode the assignments, the others the selection. The mode the app opens
  // in does not: the page opens on the selected block.
  if (model.footswitchMode != seenMode) {
    const bool first = seenMode.isEmpty();
    seenMode = model.footswitchMode;
    if (!first)
      panel = seenMode == "scene" ? Panel::scene : seenMode == "stomp" ? Panel::stomp : Panel::selection;
  }
  refreshViews();
}

void GridPage::refreshViews() {
  // A row added or removed changes how much of the page the grid takes.
  if (juce::jlimit(1, 4, static_cast<int>(model.visibleRows().size())) != shownRows)
    resized();
  if (editorShown) {
    grid.setModel(model, selected, point);
    editor.refresh(model);
    return;
  }
  const bool selection = panel == Panel::selection;
  // The stomp panel outlines its switch's blocks; the others the selection.
  if (panel == Panel::stomp) {
    stompPanel.refresh(model);
    grid.setHighlighted(stompPanel.selectedBlocks());
    grid.setModel(model, {}, std::nullopt);
  } else {
    grid.setHighlighted({});
    grid.setModel(model, selected, point);
  }
  blockPanel.setVisible(selection && !point);
  splitPanel.setVisible(selection && point.has_value());
  scenePanel.setVisible(panel == Panel::scene);
  stompPanel.setVisible(panel == Panel::stomp);
  if (selection && point)
    splitPanel.showPoint(model, *point);
  else if (selection)
    blockPanel.showBlock(model, selected);
  else if (panel == Panel::scene)
    scenePanel.refresh(model);
}

void GridPage::showPanel(Panel which) {
  panel = which;
  refreshViews();
}

void GridPage::requestEditor(const juce::String& blockId) {
  const GridSlot* block = model.find(blockId);
  if (block == nullptr)
    return;
  if (block->kindId == "plugin" && onOpenPluginWindow) {
    int row = 0, slot = 0;
    model.find(blockId, &row, &slot);
    const juce::String title = block->name + " " + type::dot() + " Row " + juce::String(row + 1) +
                               ", slot " + juce::String(slot + 1);
    if (onOpenPluginWindow(blockId, title))
      return;
  }
  openEditor(blockId);
}

void GridPage::openEditor(const juce::String& blockId) {
  if (model.find(blockId) == nullptr)
    return;
  selected = blockId;
  point.reset();
  editor.open(model, blockId);
  setEditorShown(true);
  motion::fadeIn(editor);
  editor.grabKeyboardFocus();
}

void GridPage::closeEditor() { setEditorShown(false); }

void GridPage::setEditorShown(bool shown) {
  editorShown = shown;
  editor.setVisible(shown);
  scenes.setVisible(!shown);
  grid.setVisible(!shown);
  if (shown) {
    blockPanel.setVisible(false);
    splitPanel.setVisible(false);
    scenePanel.setVisible(false);
    stompPanel.setVisible(false);
  } else {
    refreshViews();
  }
}

void GridPage::selectBlock(const juce::String& blockId) {
  // Another block: its panel fades in over the last one's.
  selected = blockId;
  selectionCleared = false;
  point.reset();
  panel = Panel::selection;
  refreshViews();
}

void GridPage::moveSelection(int dx, int dy) {
  if (editorShown || panel != Panel::selection)
    return;
  const juce::String next = model.neighbour(point ? juce::String() : selected, dx, dy);
  if (next.isNotEmpty() && (next != selected || point))
    selectBlock(next);
}

void GridPage::clearSelection() {
  if (editorShown)
    return;
  selected.clear();
  point.reset();
  selectionCleared = true;
  panel = Panel::selection;
  refreshViews();
}

void GridPage::setSettingsCollapsed(bool collapsed) {
  sessionCollapsed() = collapsed;
  resized();
}

void GridPage::openPicker(int row, int slot) {
  ensureGrid();
  library.openFor(row, slot);
  library.setVisible(true);
  motion::fadeIn(library);
  library.toFront(true);
}

void GridPage::selectPoint(GridPoint p) {
  point = p;
  panel = Panel::selection;
  refreshViews();
}

void GridPage::paint(juce::Graphics& g) { g.fillAll(colour::ground); }

void GridPage::resized() {
  auto area = getLocalBounds();
  scenes.setBounds(area.removeFromTop(kScenesHeight));
  shownRows = juce::jlimit(1, 4, static_cast<int>(model.visibleRows().size()));
  grid.setBounds(area.removeFromTop(GridView::heightForRows(shownRows)));
  // The settings: docked in the room under the grid, a slim bar when there is
  // little room (or the player collapsed it), or over the bottom of the grid
  // when they expanded it in a small window.
  const bool tight = area.getHeight() < kRoomForSettings;
  const bool collapsed = sessionCollapsed().value_or(tight);
  blockPanel.setCollapsed(collapsed);
  if (collapsed) {
    blockPanel.setBounds(area.withHeight(BlockSettingsPanel::kBarHeight));
  } else if (tight) {
    blockPanel.setBounds(getLocalBounds().removeFromBottom(BlockSettingsPanel::kExpandedHeight));
    blockPanel.toFront(false);
  } else {
    blockPanel.setBounds(area);
  }
  splitPanel.setBounds(area);
  scenePanel.setBounds(area);
  stompPanel.setBounds(area);
  removeDock.setBounds(area);
  editor.setBounds(getLocalBounds());
  library.setBounds(getLocalBounds());
  prompt.setBounds(getLocalBounds());
}

}  // namespace pedaleira::ui
