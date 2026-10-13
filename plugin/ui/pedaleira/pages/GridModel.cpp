#include "GridModel.h"

#include <cstdlib>

namespace pedaleira::ui {

namespace {

juce::String inputNameOf(const juce::var& input, const juce::String& source, int row) {
  if (source == "split")
    return "From split";
  if (source == "rows")
    return "From a row";
  if (input["type"].toString() != "device")
    return "No input";
  const int channel = input["channel"];
  if (static_cast<bool>(input["stereo"]))
    return "Input " + juce::String(channel) + "/" + juce::String(channel + 1);
  juce::ignoreUnused(row);
  return "Input " + juce::String(channel);
}

juce::String outputNameOf(const juce::var& output, const juce::String& sink, int mixRow) {
  if (sink == "mix")
    return "Mixed back into row " + juce::String(mixRow + 1);
  if (sink == "row")
    return "Into row " + juce::String(static_cast<int>(output["toRow"]) + 1);
  if (output["type"].toString() != "device")
    return "Not routed";
  const int pair = output["pair"];
  return "Out " + juce::String(pair * 2 - 1) + "/" + juce::String(pair * 2);
}

}  // namespace

std::vector<int> GridModel::visibleRows() const {
  std::vector<int> shown;
  for (const auto& row : rows)
    if (row.routed || splitInto(row.index) != nullptr)
      shown.push_back(row.index);
  // A fresh grid shows its first row even before it is routed.
  if (shown.empty())
    shown.push_back(0);
  return shown;
}

juce::String GridModel::firstBlock() const {
  for (const int r : visibleRows())
    for (const auto& slot : rows[static_cast<size_t>(r)].slots)
      if (!slot.empty)
        return slot.blockId;
  return {};
}

juce::String GridModel::neighbour(const juce::String& from, int dx, int dy) const {
  int row = 0, slot = 0;
  if (find(from, &row, &slot) == nullptr)
    return firstBlock();
  const auto& cells = rows[static_cast<size_t>(row)].slots;
  if (dx != 0) {
    for (int s = slot + (dx > 0 ? 1 : -1); s >= 0 && s < 8; s += (dx > 0 ? 1 : -1))
      if (!cells[static_cast<size_t>(s)].empty)
        return cells[static_cast<size_t>(s)].blockId;
    return from;
  }
  if (dy != 0) {
    const std::vector<int> shown = visibleRows();
    int at = -1;
    for (size_t i = 0; i < shown.size(); ++i)
      if (shown[i] == row)
        at = static_cast<int>(i);
    for (int i = at + (dy > 0 ? 1 : -1); at >= 0 && i >= 0 && i < static_cast<int>(shown.size()); i += (dy > 0 ? 1 : -1)) {
      const auto& other = rows[static_cast<size_t>(shown[static_cast<size_t>(i)])].slots;
      int best = -1;
      for (int s = 0; s < 8; ++s)
        if (!other[static_cast<size_t>(s)].empty && (best < 0 || std::abs(s - slot) < std::abs(best - slot)))
          best = s;
      if (best >= 0)
        return other[static_cast<size_t>(best)].blockId;
    }
  }
  return from;
}

const GridSplit* GridModel::splitOf(int row) const {
  for (const auto& split : splits)
    if (split.row == row)
      return &split;
  return nullptr;
}

const GridSplit* GridModel::splitInto(int lowerRow) const {
  for (const auto& split : splits)
    if (split.lowerRow == lowerRow)
      return &split;
  return nullptr;
}

const GridSlot* GridModel::find(const juce::String& blockId, int* rowOut, int* slotOut) const {
  if (blockId.isEmpty())
    return nullptr;
  for (const auto& row : rows)
    for (size_t i = 0; i < row.slots.size(); ++i)
      if (row.slots[i].blockId == blockId) {
        if (rowOut != nullptr)
          *rowOut = row.index;
        if (slotOut != nullptr)
          *slotOut = static_cast<int>(i);
        return &row.slots[i];
      }
  return nullptr;
}

GridModel GridModel::read(EngineClient& engine) {
  GridModel model;
  const juce::var state = engine.read("state.get");
  const juce::var grid = state["chain"]["grid"];
  model.gridMode = grid["mode"].toString() == "grid";
  if (!model.gridMode) {
    model.legacyEmpty = true;
    for (const char* lane : {"chain", "chainRight"})
      if (const auto* blocks = state["chain"][lane].getArray())
        for (const auto& block : *blocks)
          if (block["kind"].toString() != "insert")
            model.legacyEmpty = false;
  }

  // Which switch flips which block, for the switches that are stomps in the
  // current mode (in scene or preset mode the letters would say otherwise).
  juce::StringArray stompSwitches;
  if (const auto* switches = state["footswitches"]["switches"].getArray())
    for (const auto& footswitch : *switches)
      if (footswitch["role"].toString() == "stomp")
        stompSwitches.add(footswitch["id"].toString());
  std::map<juce::String, juce::String> footswitchOf;
  if (const auto* stomps = state["stomps"].getArray())
    for (const auto& stomp : *stomps)
      if (const auto* ids = stomp["blockIds"].getArray(); ids != nullptr && stompSwitches.contains(stomp["switch"].toString()))
        for (const auto& id : *ids)
          footswitchOf[id.toString()] = stomp["switch"].toString();

  if (const auto* splits = grid["splits"].getArray())
    for (const auto& split : *splits) {
      GridSplit s;
      s.id = split["id"].toString();
      s.row = split["row"];
      s.lowerRow = split["lowerRow"];
      s.splitAt = split["splitAt"];
      if (!split["mixAt"].isVoid())
        s.mixAt = static_cast<int>(split["mixAt"]);
      s.type = split["type"].toString();
      model.splits.push_back(s);
    }

  if (const auto* rows = grid["rows"].getArray()) {
    for (int r = 0; r < rows->size() && r < 4; ++r) {
      const juce::var row = rows->getReference(r);
      GridRow& out = model.rows[static_cast<size_t>(r)];
      out.index = r;
      out.routed = static_cast<bool>(row["routed"]);
      out.source = row["source"].toString();
      out.sink = row["sink"].toString();
      const GridSplit* into = model.splitInto(r);
      out.inputName = inputNameOf(row["input"], out.source, r);
      out.outputName = outputNameOf(row["output"], out.sink, into != nullptr ? into->row : r);
      out.inputTrimDb = row["input"]["trimDb"];
      out.outputVolumeDb = row["output"]["volumeDb"];
      out.outputMute = static_cast<bool>(row["output"]["mute"]);
      out.outputSolo = static_cast<bool>(row["output"]["solo"]);
      const auto* blocks = row["blocks"].getArray();
      for (int i = 0; i < 8; ++i) {
        GridSlot& slot = out.slots[static_cast<size_t>(i)];
        slot.active = static_cast<bool>(row["active"][i]);
        slot.blockId = row["slots"][i].toString();
        if (blocks == nullptr)
          continue;
        for (const auto& block : *blocks) {
          if (block["blockId"].toString() != slot.blockId)
            continue;
          if (block["kind"].toString() == "insert")
            break;
          slot.empty = false;
          slot.name = block["tone"]["title"].toString();
          slot.kindId = block["kindId"].toString();
          slot.category = categoryFromId(block["category"].toString());
          slot.enabled = static_cast<bool>(block["params"]["enabled"]);
          slot.loading = static_cast<bool>(block["modelLoading"]);
          slot.missing = static_cast<bool>(block["missing"]);
          if (const auto* params = block["sceneParams"].getArray())
            for (const auto& param : *params)
              slot.sceneParams.add(param.toString());
          if (const auto found = footswitchOf.find(slot.blockId); found != footswitchOf.end())
            slot.footswitch = found->second;
          break;
        }
      }
    }
  }

  if (model.legacyEmpty) {
    // What the first edit's conversion makes of a fresh rig: row 1 on
    // input 1 and the first output pair.
    GridRow& first = model.rows[0];
    first.routed = true;
    first.source = "device";
    first.sink = "device";
    first.inputName = "Input 1";
    first.outputName = "Out 1/2";
  }

  const juce::var scenes = state["scenes"];
  model.activeScene = scenes["activeId"].toString();
  model.footswitchMode = state["footswitches"]["mode"].toString();
  if (const auto* list = scenes["scenes"].getArray())
    for (const auto& scene : *list) {
      GridScene s;
      s.id = scene["id"].toString();
      s.name = scene["name"].toString();
      s.active = static_cast<bool>(scene["active"]);
      s.named = s.name != "Scene " + s.id;
      s.inUse = scene["inUse"].isVoid() || static_cast<bool>(scene["inUse"]);
      model.scenes.push_back(s);
    }
  return model;
}

}  // namespace pedaleira::ui
