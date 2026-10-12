#include "EngineCommands.h"

// Processor.h first: it pulls in the JUCE module headers, which must come
// before anything that drags in <windows.h> (see EngineCommands.cpp).
#include "Processor.h"

#include "GridTopology.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

// ####################
// GRID
// ####################
// The grid's commands: grid.* for the rig
// as a whole, row.* for one row's input, output, format, mix and input
// stage, split.* for the fork of a row into the row below it. All of them
// are thin calls into ProcessorGrid.cpp. A row.* or split.* command that
// changes something (and grid.moveBlock) on a legacy rig converts it to a
// grid first, as one undo step together with the edit.
//
// Rows and slots are numbered from 0, as the grid's own state lists them;
// input channels and output pairs from 1, as an audio interface labels them.

namespace {

using P = CommandParam;
using T = CommandParamType;

CommandResult fail(const char* code, const juce::String& message) {
  return CommandResult::failure(code, message);
}

juce::var withProperty(const juce::var& base, const char* name, const juce::var& value) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  if (const auto* source = base.getDynamicObject())
    for (const auto& property : source->getProperties())
      obj->setProperty(property.name, property.value);
  obj->setProperty(name, value);
  return juce::var(obj.get());
}

P rowParam() {
  return P::required("row", T::integer, "The row, 0 (top) to 3.").range(0, kGridRows - 1);
}

}  // namespace

void EngineCommands::registerGridCommands() {
  // What every row edit answers with: the grid as grid.get gives it, the
  // chain revision, and the one row that was edited.
  const auto rowResult = [this](int row) {
    const juce::var state = processor.getGridState();
    juce::var result = withProperty(state, "revision",
                                    static_cast<int>(processor.getCurrentChainRevision()));
    if (const auto* rows = state["rows"].getArray(); rows != nullptr && row < rows->size())
      result = withProperty(result, "row", rows->getReference(row));
    return CommandResult::success(result);
  };
  const auto rowFailure = [](const juce::Result& result) {
    return fail(CommandError::failed, result.getErrorMessage());
  };

  commands.add({
      "grid.get",
      "Returns the rig's routing: mode (legacy or grid), legacyProjectable (whether a grid "
      "could be written as a legacy rig), and in grid mode rows (each with index, format, "
      "input, output, slots as the eight block ids of the row with empty slots included, "
      "active per slot (a block that is run: in a routed row and, for the lower row of a "
      "split, inside the split's range), activeSlots as [first, end), routed, source (device, "
      "split, rows or none), sink (device, row, mix or none), runsAs (the format the row runs "
      "in), latencySamples and padSamples at the chain rate, cpu, inputMissing and "
      "outputFallback), splits (each with id, row, lowerRow, splitAt, mixAt or null, type, "
      "active, lowerActive, params by id, alignment as the samples paths a and b wait at the "
      "mixer, and meters), feeds (each from, to, active and padSamples), groups (the rows "
      "that are processed together: tied by a split or a feed), latencySamples (as reported "
      "to the host), chainLatencySamples, cpu (total and critical, the plan's estimate as "
      "fractions of one core), imageStage (none in grid mode), legacyControls (per legacy "
      "image control, rows when rows 0 and 1 carry it and held when it rests at neutral and "
      "does nothing) and io (the device's input channels and output pairs, each with "
      "whether it is available; a channel or pair a row asks for that the device lacks is "
      "listed as unavailable).",
      {},
      false,
      CommandPermission::read,
      [this](const juce::var&) {
        return CommandResult::success(
            withProperty(processor.getGridState(), "revision",
                         static_cast<int>(processor.getCurrentChainRevision())));
      },
  });

  commands.add({
      "grid.convert",
      "Converts the rig between the legacy routing (one chain, or two lanes with an optional "
      "branch) and the grid, as one undo step. To grid: lane position i becomes slot i of "
      "row 0 or 1, and lossy lists what does not carry over. A branch becomes a split of row "
      "0 into row 1 that does not rejoin (A/B, both at 0 dB), with the trunk as row 0. To "
      "legacy: only a grid with the shape of a legacy rig converts (one stereo or dual-mono "
      "row, two mono rows, or two mono rows joined by such a split, on inputs 1 and 2 and "
      "output pair 1); otherwise it fails with the reasons.",
      {P::required("to", T::string, "The routing to convert to.").oneOf({"grid", "legacy"})},
      true,
      CommandPermission::edit,
      [this](const juce::var& args) {
        const juce::var converted = processor.convertRouting(args["to"].toString() == "grid");
        if (!static_cast<bool>(converted["ok"])) {
          juce::StringArray reasons;
          if (const auto* list = converted["reasons"].getArray())
            for (const auto& reason : *list)
              reasons.add(reason.toString());
          return fail(CommandError::failed, "cannot convert to " + args["to"].toString() + ": " +
                                                reasons.joinIntoString("; "));
        }
        juce::var result = withProperty(processor.getGridState(), "lossy", converted["lossy"]);
        result = withProperty(result, "revision",
                              static_cast<int>(processor.getCurrentChainRevision()));
        return CommandResult::success(result);
      },
  });

  commands.add({
      "grid.moveBlock",
      "Moves a block to a slot of the grid. swap exchanges it with whatever is there (a block "
      "or an empty slot); insert puts it there and pushes the blocks from that slot on toward "
      "the row's nearest empty slot, and fails when the row is full. A NAM block that lands in "
      "a row of another format (mono or stereo against dual mono) is rebuilt for it.",
      {P::required("blockId", T::string, "The block to move."), rowParam(),
       P::required("slot", T::integer, "The slot in that row, 0 to 7.").range(0, kGridCols - 1),
       P::optional("mode", T::string, "What happens to a block already there.")
           .oneOf({"swap", "insert"})
           .withDefault("swap")},
      true,
      CommandPermission::edit,
      [this, rowFailure](const juce::var& args) {
        const juce::String blockId = args["blockId"].toString();
        const juce::Result moved =
            processor.moveGridBlock(blockId.toStdString(), args["row"], args["slot"],
                                    args["mode"].toString() == "insert");
        if (moved.failed())
          return moved.getErrorMessage().startsWith("no block")
                     ? fail(CommandError::notFound, moved.getErrorMessage())
                     : rowFailure(moved);
        return layoutResult(withProperty({}, "blockId", blockId));
      },
  });

  commands.add({
      "row.setInput",
      "Sets where a row's signal comes from: none (the row is not fed and does not run; its "
      "blocks are kept) or a device channel, or the pair starting at it. A pair can be summed "
      "to mono. An input the device does not have is silence and is reported as inputMissing; "
      "the setting is kept for a device that has it.",
      {rowParam(),
       P::required("source", T::string, "The kind of input.").oneOf({"none", "device"}),
       P::optional("channel", T::integer, "The device's input channel, counted from 1.")
           .range(1, kMaxIoChannels)
           .withDefault(1),
       P::optional("stereo", T::boolean, "Take the pair: channel and the one after it.")
           .withDefault(false),
       P::optional("fold", T::string,
                   "For a pair: none keeps left and right apart, sum puts half of their sum on "
                   "both.")
           .oneOf({"none", "sum"})
           .withDefault("none")},
      true,
      CommandPermission::edit,
      [this, rowResult, rowFailure](const juce::var& args) {
        const int row = args["row"];
        const bool device = args["source"].toString() == "device";
        const bool stereo = static_cast<bool>(args["stereo"]);
        const int channel = static_cast<int>(args["channel"]) - 1;
        if (device && stereo && channel + 1 >= kMaxIoChannels)
          return fail(CommandError::invalidArgs,
                      "row.setInput: a pair starts at channel 1 to " +
                          juce::String(kMaxIoChannels - 1));
        const juce::Result set = processor.setRowInput(
            row, device ? RowInput::Type::Device : RowInput::Type::None, channel, stereo,
            args["fold"].toString() == "sum" ? RowInput::Fold::SumToMono : RowInput::Fold::None);
        return set.failed() ? rowFailure(set) : rowResult(row);
      },
  });

  commands.add({
      "row.setOutput",
      "Sets where a row's signal goes: none (the row does not run), a pair of device outputs, "
      "or another row (target row with toRow), whose input it is then summed into at the "
      "row's volume, with the rows that feed one row kept in step. A fed row has no input of "
      "its own and is not the lower row of a split, and feeds form no loop; otherwise the "
      "command fails with the reason and nothing changes. A pair the device does not have "
      "falls back to pair 1 and is reported as outputFallback; the setting is kept.",
      {rowParam(),
       P::required("target", T::string, "The kind of output.").oneOf({"none", "device", "row"}),
       P::optional("pair", T::integer, "The output pair, counted from 1 (pair 2 is Out 3/4).")
           .range(1, kMaxIoPairs)
           .withDefault(1),
       P::optional("toRow", T::integer, "For target row: the row to feed.").range(0, kGridRows - 1)},
      true,
      CommandPermission::edit,
      [this, rowResult, rowFailure](const juce::var& args) {
        const int row = args["row"];
        const juce::String target = args["target"].toString();
        if (target == "row" && args["toRow"].isVoid())
          return fail(CommandError::invalidArgs, "row.setOutput: target row needs 'toRow'");
        const juce::Result set = processor.setRowOutput(
            row, target == "device" ? RowOutput::Type::Device
                     : target == "row" ? RowOutput::Type::Row : RowOutput::Type::None,
            static_cast<int>(args["pair"]) - 1,
            target == "row" ? static_cast<int>(args["toRow"]) : -1);
        return set.failed() ? rowFailure(set) : rowResult(row);
      },
  });

  commands.add({
      "row.setFormat",
      "Sets what a row's buffer carries: mono (one channel, panned onto its output), stereo "
      "(a stereo pair) or dual (two independent mono signals through the same blocks, each "
      "amp running once per channel). Amps in the row rebuild when the change moves them "
      "between one and two voices.",
      {rowParam(),
       P::required("format", T::string, "The row format.").oneOf({"mono", "stereo", "dual"})},
      true,
      CommandPermission::edit,
      [this, rowResult, rowFailure](const juce::var& args) {
        const int row = args["row"];
        const juce::String format = args["format"].toString();
        const juce::Result set = processor.setRowFormat(
            row, format == "mono" ? RowFormat::Mono
                     : format == "dual" ? RowFormat::DualMono : RowFormat::Stereo);
        return set.failed() ? rowFailure(set) : rowResult(row);
      },
  });

  commands.add({
      "row.setMix",
      "Sets a row's send to its output: volume, pan, mute, solo and polarity; fields left out "
      "stay as they are. Pan is constant power on a mono row (hard left or right is unity) "
      "and a balance on a stereo or dual-mono row (unity at the centre, the far side falling "
      "to silence). Solo silences the outputs of the rows that are neither soloed nor fed by "
      "a soloed row. A row that feeds another row goes in at its volume, mute and polarity; "
      "its pan and solo wait for a device output. Changes glide, without a gap in the audio.",
      {rowParam(),
       P::optional("volumeDb", T::number, "Volume in dB; the bottom of the range is off.")
           .range(kRowVolumeMinDb, kRowVolumeMaxDb),
       P::optional("pan", T::number, "0 is left, 0.5 the centre, 1 right.").range(0, 1),
       P::optional("mute", T::boolean, "Silence the row's send."),
       P::optional("solo", T::boolean, "Hear only the soloed rows."),
       P::optional("invert", T::boolean, "Flip the row's polarity.")},
      true,
      CommandPermission::edit,
      [this, rowResult, rowFailure](const juce::var& args) {
        const int row = args["row"];
        TONE3000Processor::RowMixEdit edit;
        if (!args["volumeDb"].isVoid())
          edit.volumeDb = static_cast<float>(static_cast<double>(args["volumeDb"]));
        if (!args["pan"].isVoid())
          edit.pan = static_cast<float>(static_cast<double>(args["pan"]));
        if (!args["mute"].isVoid())
          edit.mute = static_cast<bool>(args["mute"]);
        if (!args["solo"].isVoid())
          edit.solo = static_cast<bool>(args["solo"]);
        if (!args["invert"].isVoid())
          edit.invert = static_cast<bool>(args["invert"]);
        const juce::Result set = processor.setRowMix(row, edit);
        return set.failed() ? rowFailure(set) : rowResult(row);
      },
  });

  commands.add({
      "row.setInputStage",
      "Sets a row's share of the input stage; fields left out stay as they are. trimDb is the "
      "row's own level on top of the rig's input level (applied as the signal enters the "
      "row, after the gate). gate switches the noise gate for the row's input (the gate's "
      "settings are the rig's). pitch asks for the pitch shifter: one input runs it, the "
      "first that asks, and the others are delayed to stay in step.",
      {rowParam(),
       P::optional("trimDb", T::number, "Input trim in dB.").range(kRowTrimMinDb, kRowTrimMaxDb),
       P::optional("gate", T::boolean, "Run the noise gate on this row's input."),
       P::optional("pitch", T::boolean, "Run the pitch shifter on this row's input.")},
      true,
      CommandPermission::edit,
      [this, rowResult, rowFailure](const juce::var& args) {
        const int row = args["row"];
        TONE3000Processor::RowInputStageEdit edit;
        if (!args["trimDb"].isVoid())
          edit.trimDb = static_cast<float>(static_cast<double>(args["trimDb"]));
        if (!args["gate"].isVoid())
          edit.gate = static_cast<bool>(args["gate"]);
        if (!args["pitch"].isVoid())
          edit.pitch = static_cast<bool>(args["pitch"]);
        const juce::Result set = processor.setRowInputStage(row, edit);
        return set.failed() ? rowFailure(set) : rowResult(row);
      },
  });

  // ---- Splits ---------------------------------------------------------------
  const auto splitTypeOf = [](const juce::String& name) {
    SplitterType type = SplitterType::Balance;
    splitterTypeFromName(name, type);
    return type;
  };
  // What a split edit answers with: the grid, the revision and the one split.
  const auto splitResult = [this](int row) {
    const juce::var state = processor.getGridState();
    juce::var result = withProperty(state, "revision",
                                    static_cast<int>(processor.getCurrentChainRevision()));
    if (const auto* splits = state["splits"].getArray())
      for (const auto& split : *splits)
        if (static_cast<int>(split["row"]) == row && !static_cast<bool>(split["inert"]))
          result = withProperty(result, "split", split);
    return CommandResult::success(result);
  };
  const auto splitFailure = [](const juce::Result& result) {
    return fail(result.getErrorMessage().contains("has no split") ? CommandError::notFound
                                                                  : CommandError::failed,
                result.getErrorMessage());
  };
  const auto upperRowParam = [] {
    return P::required("row", T::integer,
                       "The row that is split (the upper one), 0 to 2; the row below it is the "
                       "lower path.")
        .range(0, kGridRows - 2);
  };

  commands.add({
      "split.add",
      "Splits a row into the row directly below it. The signal forks at splitAt, a boundary "
      "between slots (0 is before the first slot, 8 after the last): path A stays in the row, "
      "path B runs through the lower row's slots from there. With mixAt the two paths are "
      "mixed back into the row at that later boundary, aligned to the sample; without it the "
      "lower row runs to its own output or feed. The lower row must have no input of its "
      "own; its slots outside the split's range are kept and not run. One split per row. A "
      "split of a lower row has to lie inside the split above it, which makes a three-way "
      "parallel path. The lower row takes the row's format. A shape the rules forbid fails "
      "with the reason and changes nothing.",
      {upperRowParam(),
       P::required("splitAt", T::integer, "The boundary the signal forks at, 0 to 8.")
           .range(0, kGridCols),
       P::optional("mixAt", T::integer,
                   "The boundary the paths rejoin at, above splitAt and up to 8. Left out: no "
                   "rejoin.")
           .range(1, kGridCols),
       P::optional("type", T::string,
                   "The splitter: balance (both paths at unity, one knob fades either), ab (a "
                   "level per path) or crossover (lows to A, highs to B).")
           .oneOf({"balance", "ab", "crossover"})
           .withDefault("balance")},
      true,
      CommandPermission::edit,
      [this, splitResult, splitTypeOf](const juce::var& args) {
        const int row = args["row"];
        const juce::Result added = processor.addSplit(
            row, args["splitAt"], args["mixAt"].isVoid() ? -1 : static_cast<int>(args["mixAt"]),
            splitTypeOf(args["type"].toString()));
        return added.failed() ? fail(CommandError::failed, added.getErrorMessage())
                              : splitResult(row);
      },
  });

  commands.add({
      "split.move",
      "Moves a row's split: the fork (splitAt), the rejoin (mixAt), or both; fields left out "
      "stay. rejoin false takes the rejoin away, so the lower row runs to its own output. A "
      "position the rules forbid fails with the reason and changes nothing.",
      {upperRowParam(),
       P::optional("splitAt", T::integer, "The boundary the signal forks at, 0 to 8.")
           .range(0, kGridCols),
       P::optional("mixAt", T::integer, "The boundary the paths rejoin at, up to 8.")
           .range(1, kGridCols),
       P::optional("rejoin", T::boolean, "false: no rejoin (mixAt is then not allowed).")},
      true,
      CommandPermission::edit,
      [this, splitResult, splitFailure](const juce::var& args) {
        const int row = args["row"];
        const bool dropRejoin = !args["rejoin"].isVoid() && !static_cast<bool>(args["rejoin"]);
        if (dropRejoin && !args["mixAt"].isVoid())
          return fail(CommandError::invalidArgs, "split.move: give mixAt or rejoin false, not both");
        std::optional<int> splitAt, mixAt;
        if (!args["splitAt"].isVoid())
          splitAt = static_cast<int>(args["splitAt"]);
        if (!args["mixAt"].isVoid())
          mixAt = static_cast<int>(args["mixAt"]);
        if (dropRejoin)
          mixAt = -1;
        const juce::Result moved = processor.moveSplit(row, splitAt, mixAt);
        return moved.failed() ? splitFailure(moved) : splitResult(row);
      },
  });

  commands.add({
      "split.remove",
      "Takes a row's split out. The lower row is then fed by nothing and does not run; its "
      "blocks are kept.",
      {upperRowParam()},
      true,
      CommandPermission::edit,
      [this, splitFailure](const juce::var& args) {
        const juce::Result removed = processor.removeSplit(args["row"]);
        if (removed.failed())
          return splitFailure(removed);
        return CommandResult::success(
            withProperty(processor.getGridState(), "revision",
                         static_cast<int>(processor.getCurrentChainRevision())));
      },
  });

  commands.add({
      "split.setType",
      "Changes what a row's splitter does: balance, ab or crossover. The parameters of the "
      "other types keep their values.",
      {upperRowParam(),
       P::required("type", T::string, "The splitter type.").oneOf({"balance", "ab", "crossover"})},
      true,
      CommandPermission::edit,
      [this, splitResult, splitFailure, splitTypeOf](const juce::var& args) {
        const int row = args["row"];
        const juce::Result set = processor.setSplitType(row, splitTypeOf(args["type"].toString()));
        return set.failed() ? splitFailure(set) : splitResult(row);
      },
  });

  commands.add({
      "split.setParam",
      "Sets one parameter of a row's split, in its real unit (split.listParams lists them): "
      "the splitter's (split.balance, split.levelA, split.levelB, split.crossoverHz, "
      "split.swap, split.stereo) or the mixer's (mix.levelA, mix.panA, mix.levelB, mix.panB, "
      "mix.invertB, mix.level, mix.mute). The value is clamped to the parameter's range and "
      "the change glides, without a gap in the audio.",
      {upperRowParam(), P::required("param", T::string, "The parameter id."),
       P::required("value", T::number, "The value in the parameter's unit; 0 or 1 for a switch.")},
      true,
      CommandPermission::edit,
      [this, splitResult](const juce::var& args) {
        const int row = args["row"];
        const juce::String param = args["param"].toString();
        const juce::Result set =
            processor.setSplitParam(row, param, static_cast<double>(args["value"]));
        if (set.failed())
          return fail(set.getErrorMessage().contains("has no split") ? CommandError::notFound
                          : set.getErrorMessage().contains("no parameter")
                                ? CommandError::invalidArgs
                                : CommandError::failed,
                      "split.setParam: " + set.getErrorMessage());
        return splitResult(row);
      },
  });

  commands.add({
      "split.listParams",
      "Lists the parameters of a row's split with their ranges, units, defaults and current "
      "values, and the split's id and type.",
      {upperRowParam()},
      false,
      CommandPermission::read,
      [this](const juce::var& args) {
        const int row = args["row"];
        const juce::var params = processor.getSplitParams(row);
        if (params.isVoid())
          return fail(CommandError::notFound, "row " + juce::String(row) + " has no split");
        return CommandResult::success(params);
      },
  });
}
