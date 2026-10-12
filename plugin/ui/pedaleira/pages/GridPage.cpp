#include "GridPage.h"

#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"

#include <cmath>

namespace pedaleira::ui {

namespace {

constexpr int kScenesHeight = 56;
constexpr int kGridHeight = 452;
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
  scenes = newScenes;
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
  for (size_t i = 0; i < scenes.size() && i < 8; ++i) {
    const GridScene& scene = scenes[i];
    const auto area = buttonBounds(static_cast<int>(i));
    // Active: iris; a scene with a name of its own: raised; the rest plain.
    const juce::Colour fill = scene.active ? colour::iris : scene.named ? colour::raised : juce::Colours::transparentBlack;
    const juce::Colour fg = scene.active ? colour::ground : scene.named ? colour::text : colour::muted;
    g.setColour(fill);
    g.fillRoundedRectangle(area, size::buttonRadius);
    g.setColour(scene.active ? colour::iris : colour::line);
    g.drawRoundedRectangle(area.reduced(0.5f), size::buttonRadius, 1.0f);
    auto inner = area.reduced(10.0f, 0.0f);
    g.setFont(type::mono(13.0f));
    g.setColour(fg);
    g.drawText(scene.id, inner.removeFromLeft(14.0f), juce::Justification::centredLeft, false);
    inner.removeFromLeft(8.0f);
    g.setFont(type::button());
    g.drawText(scene.name, inner, juce::Justification::centredLeft, true);
  }
}

void ScenesStrip::mouseUp(const juce::MouseEvent& event) {
  for (size_t i = 0; i < scenes.size() && i < 8; ++i)
    if (buttonBounds(static_cast<int>(i)).contains(event.position) && onSelect)
      onSelect(scenes[i].id);
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
    if (ownInput) {
      g.setColour(colour::line);
      g.drawRoundedRectangle(input.reduced(0.5f), size::panelRadius, 1.0f);
    } else {
      paint::dashedOutline(g, input, size::panelRadius, colour::line);
    }
    g.setFont(type::sans(11.0f, type::Weight::semibold).withExtraKerningFactor(0.08f));
    g.setColour(colour::muted);
    g.drawText("ROW " + juce::String(row.index + 1) + (ownInput ? " IN" : ""),
               input.reduced(10.0f).withHeight(14.0f), juce::Justification::centredLeft, true);
    g.setFont(type::sans(13.0f, type::Weight::semibold));
    g.setColour(ownInput ? colour::text : colour::muted);
    g.drawText(row.inputName, input.reduced(10.0f).withTrimmedTop(36.0f), juce::Justification::centredLeft, true);

    // The output box: a device pair with mute, solo and volume, or where
    // the row goes instead.
    const auto output = juce::Rectangle<float>(outputX, top, kOutputWidth, kSlotHeight);
    if (row.sink == "device") {
      paint::panel(g, output);
      auto inner = output.reduced(10.0f);
      g.setFont(type::sans(13.0f, type::Weight::semibold));
      g.setColour(colour::text);
      g.drawText(row.outputName, inner.withHeight(24.0f), juce::Justification::centredLeft, true);
      for (int b = 0; b < 2; ++b) {
        const auto box = juce::Rectangle<float>(inner.getRight() - 54.0f + b * 30.0f, inner.getY(), 24.0f, 24.0f);
        const bool on = b == 0 ? row.outputMute : row.outputSolo;
        g.setColour(on ? (b == 0 ? colour::warn : colour::iris) : colour::raised);
        g.fillRoundedRectangle(box, 6.0f);
        g.setColour(colour::line);
        g.drawRoundedRectangle(box.reduced(0.5f), 6.0f, 1.0f);
        g.setFont(type::sans(11.0f, type::Weight::bold));
        g.setColour(on ? colour::ground : colour::muted);
        g.drawText(b == 0 ? "M" : "S", box, juce::Justification::centred, false);
      }
      const auto barRow = inner.withTrimmedTop(inner.getHeight() - 14.0f);
      const float textWidth = 64.0f;
      const auto bar = barRow.withTrimmedRight(textWidth + 8.0f).withSizeKeepingCentre(barRow.getWidth() - textWidth - 8.0f, 4.0f);
      g.setColour(colour::line);
      g.fillRoundedRectangle(bar, 2.0f);
      // The volume runs -60 to +12 dB along the bar.
      const float filled = juce::jlimit(0.0f, 1.0f, static_cast<float>((row.outputVolumeDb + 60.0) / 72.0));
      g.setColour(colour::text);
      g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * filled), 2.0f);
      g.setFont(type::mono(11.0f));
      g.setColour(colour::muted);
      g.drawText(signedDb(row.outputVolumeDb), barRow.withLeft(barRow.getRight() - textWidth),
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
    paint::dashedOutline(g, area, size::panelRadius, colour::line);
    auto inner = addRowBounds().reduced(16, 0);
    icons::draw(g, icons::path(icons::Icon::plus), inner.removeFromLeft(18).toFloat().withSizeKeepingCentre(18.0f, 18.0f),
                colour::muted);
    inner.removeFromLeft(10);
    g.setFont(type::button());
    g.setColour(colour::muted);
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
  const bool onPoint = pointAt(event.position).has_value();
  const Cable cable = onPoint ? Cable{} : cableAt(event.position);
  if (cable.row != hoverCable.row || cable.boundary != hoverCable.boundary) {
    hoverCable = cable;
    repaint();
  }
  setMouseCursor(onPoint || cable.row >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void GridView::mouseExit(const juce::MouseEvent&) {
  if (hoverCable.row >= 0) {
    hoverCable = {};
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
  if (cable.row >= 0)
    showCableMenu(cable);
}

void GridView::mouseDrag(const juce::MouseEvent& event) {
  if (draggingPoint) {
    dragPosition = event.position;
    repaint();
  }
}

void GridView::mouseUp(const juce::MouseEvent& event) {
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
  if (key == juce::KeyPress::escapeKey && (draggingPoint || draggingTile)) {
    cancelDrags();
    return true;
  }
  return false;
}

void GridView::cancelDrags() {
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
  repaint();
}

void GridView::tileMouseUp(int, int, const juce::MouseEvent& event) {
  if (!draggingTile)
    return;
  const auto from = *draggingTile;
  draggingTile.reset();
  repaint();
  const auto target = slotAt(event.getEventRelativeTo(this).getPosition());
  if (!target || *target == from)
    return;
  const GridSlot& moving = model.rows[static_cast<size_t>(from.first)].slots[static_cast<size_t>(from.second)];
  if (onMoveBlock)
    onMoveBlock(moving.blockId, target->first, target->second);
}

// ####################
// BLOCK PANEL
// ####################
BlockPanel::BlockPanel(EngineClient& client) : engine(client) {
  setTitle("Selected block");
  enabled.onClick = [this] {
    engine.run("block.setEnabled", object({{"blockId", blockId}, {"enabled", enabled.getToggleState()}}));
    if (onChanged)
      onChanged();
  };
  replace.onClick = [this] {
    if (onReplace)
      onReplace(blockId);
  };
  openEditor.onClick = [this] {
    if (onOpenEditor)
      onOpenEditor(blockId);
  };
  assignFootswitch.onClick = [this] {
    juce::PopupMenu menu;
    for (const char* letter : {"A", "B", "C", "D", "E", "F", "G", "H"})
      menu.addItem(juce::String("Footswitch ") + letter, [this, letter] {
        engine.run("stomp.assign", object({{"switch", letter}, {"blockIds", juce::Array<juce::var>{blockId}}}));
        if (onChanged)
          onChanged();
      });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&assignFootswitch));
  };
  for (juce::Component* c : std::initializer_list<juce::Component*>{&enabled, &assignFootswitch, &replace, &openEditor})
    addChildComponent(c);
}

void BlockPanel::showBlock(const GridModel& model, const juce::String& id) {
  blockId = id;
  int row = 0, slot = 0;
  const GridSlot* block = model.find(id, &row, &slot);
  hasBlock = block != nullptr;
  activeScene = model.activeScene;
  knobs.clear();
  params.clear();
  sceneParam = -1;
  sceneValues.clear();
  for (juce::Component* c : std::initializer_list<juce::Component*>{&enabled, &assignFootswitch, &replace, &openEditor})
    c->setVisible(hasBlock);
  if (!hasBlock) {
    repaint();
    return;
  }

  title = block->name;
  category = block->category;
  const juce::String what = block->kindId == "nam" ? juce::String("NAM capture")
                            : block->kindId == "ir" ? juce::String("Impulse response")
                                                    : juce::String("Built-in");
  subtitle = categoryName(category) + " " + type::dot() + " " + what + " " + type::dot() + " Row " +
             juce::String(row + 1) + ", slot " + juce::String(slot + 1);
  enabled.setToggleState(block->enabled, juce::dontSendNotification);

  // The knobs: the model blocks' input, up to four of the kind's own
  // parameters, then output and mix (the board shows six).
  const juce::var listed = engine.read("block.listParams", object({{"blockId", id}}));
  std::vector<KnobParam> common, kind;
  if (const auto* list = listed["params"].getArray())
    for (const auto& item : *list) {
      if (item["type"].toString() != "float")
        continue;
      KnobParam p = KnobParam::fromVar(item);
      p.sceneAssigned = block->sceneParams.contains(p.id);
      (p.common ? common : kind).push_back(p);
    }
  const auto commonParam = [&common](const char* wanted) -> std::optional<KnobParam> {
    for (const auto& p : common)
      if (p.id == wanted)
        return p;
    return std::nullopt;
  };
  if (kind.empty())
    if (auto input = commonParam("inputGain"))
      params.push_back(*input);
  for (size_t i = 0; i < kind.size() && i < 4; ++i)
    params.push_back(kind[i]);
  for (const char* last : {"outputGain", "mix"})
    if (auto p = commonParam(last))
      params.push_back(*p);

  for (size_t i = 0; i < params.size(); ++i) {
    const KnobParam& p = params[i];
    auto knob = std::make_unique<Knob>(p.name);
    knob->setPosition(p.toPosition(p.value));
    knob->setDefaultPosition(p.toPosition(p.defaultValue));
    knob->setValueText(p.text(p.value));
    knob->setAssignment(p.sceneAssigned ? Knob::Assignment::scene : Knob::Assignment::none);
    knob->setBypassed(!block->enabled);
    Knob* raw = knob.get();
    const int index = static_cast<int>(i);
    knob->onChange = [this, raw, index](double position) {
      KnobParam& param = params[static_cast<size_t>(index)];
      param.value = param.fromPosition(position);
      raw->setValueText(param.text(param.value));
      engine.run("block.setParam", object({{"blockId", blockId}, {"param", param.id}, {"value", param.value}}));
      if (param.sceneAssigned)
        showSceneValues(index);
    };
    knob->onContextMenu = [this, index] { openKnobMenu(index); };
    addAndMakeVisible(*knob);
    knobs.push_back(std::move(knob));
    if (p.sceneAssigned && sceneParam < 0)
      showSceneValues(index);
  }
  resized();
  repaint();
}

void BlockPanel::showSceneValues(int paramIndex) {
  sceneParam = paramIndex;
  sceneValues.clear();
  const KnobParam& param = params[static_cast<size_t>(paramIndex)];
  const juce::var scenes = engine.read("scene.get");
  if (const auto* list = scenes["scenes"].getArray())
    for (const auto& scene : *list) {
      double value = param.value;
      if (const auto* values = scene["values"].getArray())
        for (const auto& entry : *values)
          if (entry["blockId"].toString() == blockId && entry["param"].toString() == param.id)
            value = entry["value"];
      sceneValues.emplace_back(scene["id"].toString(), value);
    }
  repaint(sceneBox);
}

void BlockPanel::openKnobMenu(int paramIndex) {
  const KnobParam& param = params[static_cast<size_t>(paramIndex)];
  juce::PopupMenu menu;
  menu.addItem(param.sceneAssigned ? "Same value in every scene" : "Value per scene", [this, paramIndex] {
    const KnobParam& p = params[static_cast<size_t>(paramIndex)];
    engine.run(p.sceneAssigned ? "scene.unassignParam" : "scene.assignParam",
               object({{"blockId", blockId}, {"param", p.id}}));
    if (onChanged)
      onChanged();
  });
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(knobs[static_cast<size_t>(paramIndex)].get()));
}

void BlockPanel::paint(juce::Graphics& g) {
  g.fillAll(colour::surface);
  auto area = getLocalBounds().reduced(24, 20);
  if (!hasBlock) {
    g.setFont(type::body());
    g.setColour(colour::muted);
    g.drawText("Select a block to edit it, or click an empty slot to add one.", area.removeFromTop(40),
               juce::Justification::centredLeft, false);
    return;
  }
  auto head = area.removeFromTop(40);
  const auto icon = head.removeFromLeft(40).toFloat();
  g.setColour(categoryTint(category));
  g.fillRoundedRectangle(icon, size::panelRadius);
  icons::draw(g, icons::path(category), icon.reduced(9.0f), categoryColour(category));
  head.removeFromLeft(12);
  g.setFont(type::heading());
  g.setColour(colour::text);
  g.drawText(title, head.removeFromTop(26), juce::Justification::centredLeft, true);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText(subtitle, head, juce::Justification::centredLeft, true);

  // "On in scene B", beside the switch.
  g.setFont(type::compact());
  g.setColour(colour::text);
  const auto toggleArea = enabled.getBounds();
  g.drawText((enabled.getToggleState() ? "On in scene " : "Off in scene ") + activeScene,
             toggleArea.getRight() + 8, toggleArea.getY(), 110, toggleArea.getHeight(),
             juce::Justification::centredLeft, false);

  // The per-scene values of the knob that has them.
  if (sceneBox.isEmpty())
    return;
  const auto box = sceneBox.toFloat();
  g.setColour(colour::ground);
  g.fillRoundedRectangle(box, size::panelRadius);
  g.setColour(colour::line);
  g.drawRoundedRectangle(box.reduced(0.5f), size::panelRadius, 1.0f);
  auto inner = sceneBox.reduced(16, 14);
  auto heading = inner.removeFromTop(18);
  if (sceneParam < 0) {
    paint::label(g, "Per scene", heading, juce::Justification::centredLeft, colour::iris);
    g.setFont(type::sans(12.0f));
    g.setColour(colour::muted);
    g.drawFittedText("Right-click a knob to give it a value per scene.", inner.withTrimmedTop(8),
                     juce::Justification::topLeft, 2);
    return;
  }
  const KnobParam& param = params[static_cast<size_t>(sceneParam)];
  paint::label(g, param.name + " " + type::dot() + " per scene", heading, juce::Justification::centredLeft, colour::iris);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText("Right-click a knob to assign", heading, juce::Justification::centredRight, false);
  inner.removeFromTop(10);
  auto cells = inner.removeFromTop(32);
  const int cellWidth = (cells.getWidth() - 3 * 8) / 4;
  for (size_t i = 0; i < sceneValues.size() && i < 4; ++i) {
    const auto cell = cells.removeFromLeft(cellWidth).toFloat();
    cells.removeFromLeft(8);
    const bool active = sceneValues[i].first == activeScene;
    g.setColour(active ? colour::iris : colour::raised);
    g.fillRoundedRectangle(cell, size::buttonRadius);
    g.setFont(type::mono(12.0f));
    g.setColour(active ? colour::ground : colour::text);
    g.drawFittedText(sceneValues[i].first + " " + param.text(sceneValues[i].second),
                     cell.reduced(6.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
  }
  inner.removeFromTop(10);
  g.setFont(type::sans(12.0f));
  g.setColour(colour::muted);
  g.drawText("Scenes E to H use the scene A value until edited.", inner.removeFromTop(16),
             juce::Justification::centredLeft, false);
}

void BlockPanel::resized() {
  auto area = getLocalBounds().reduced(24, 20);
  auto head = area.removeFromTop(40);
  int right = head.getRight();
  for (PushButton* button : {&openEditor, &replace, &assignFootswitch}) {
    const int width = button->idealWidth();
    button->setBounds(right - width, head.getCentreY() - 16, width, 32);
    right = button->getX() - 12;
  }
  enabled.setBounds(right - 130, head.getCentreY() - 14, 44, 28);
  area.removeFromTop(20);
  sceneBox = area.removeFromRight(380).withHeight(juce::jmin(area.getHeight(), 130));
  int x = area.getX();
  for (auto& knob : knobs) {
    knob->setBounds(x, area.getY(), 84, 110);
    x += 104;
  }
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
  grid.onSelectPoint = [this](GridPoint p) { selectPoint(p); };
  grid.onEmptySlot = [this](int row, int slot) { openPicker(row, slot); };
  grid.onInsertHere = [this](int row, int slot) { openPicker(row, slot); };
  addChildComponent(library);
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
  grid.onOpenBlock = [this](const juce::String& blockId) { requestEditor(blockId); };
  blockPanel.onOpenEditor = [this](const juce::String& blockId) { requestEditor(blockId); };
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
    selected.clear();
    for (const int r : model.visibleRows())
      for (const auto& slot : model.rows[static_cast<size_t>(r)].slots)
        if (selected.isEmpty() && !slot.empty)
          selected = slot.blockId;
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
  selected = blockId;
  point.reset();
  panel = Panel::selection;
  refreshViews();
}

void GridPage::openPicker(int row, int slot) {
  ensureGrid();
  library.openFor(row, slot);
  library.setVisible(true);
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
  grid.setBounds(area.removeFromTop(kGridHeight));
  blockPanel.setBounds(area);
  splitPanel.setBounds(area);
  scenePanel.setBounds(area);
  stompPanel.setBounds(area);
  editor.setBounds(getLocalBounds());
  library.setBounds(getLocalBounds());
}

}  // namespace pedaleira::ui
