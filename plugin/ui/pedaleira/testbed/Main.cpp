// PedaleiraUiTestbed: renders the Pedaleira UI over a real headless engine.
// A scenario is a list of engine commands
// applied to a fresh rig, then a page; the testbed writes one PNG per
// scenario. Runs without a display (the snapshot is drawn in software), so
// it works on Linux under CI as well as on the Mac.
//
//   PedaleiraUiTestbed --capture <folder> [--scale 1|2] [scenario...]
//   PedaleiraUiTestbed --list
#include "AgentHub.h"
#include "CommandServer.h"
#include "EngineClient.h"
#include "EngineCommands.h"
#include "LibraryPaths.h"
#include "PluginScanner.h"
#include "Processor.h"
#include "SetlistLibrary.h"
#include "CaptureScenes.h"
#include "FakeDevice.h"
#include "Gallery.h"
#include "pages/CapturePage.h"
#include "pages/ComingSoon.h"
#include "pages/FootswitchModesPage.h"
#include "pages/GigView.h"
#include "pages/GridPage.h"
#include "pages/ImportPage.h"
#include "pages/LibraryPage.h"
#include "pages/SettingsPage.h"
#include "shell/AppShell.h"

#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using pedaleira::ui::Page;

struct Scenario {
  const char* name;
  const char* description;
  Page page;
  // Engine commands applied first, as PedaleiraRender --run takes them.
  const char* script;
  // The gallery replaces the grid page.
  bool gallery = false;
  // The block the grid page selects ("$slot:row:slot"), else its first.
  const char* select = "";
  // Or a split point: "split:<row>" or "mix:<row>".
  const char* point = "";
  // Or the block library: "picker:<row>:<slot>[:<category>]".
  // Or a block's editor: "editor:<block, as $slot:row:slot>"; "replace:<source>"
  // then presses what asks for another file (Swap capture). Several, with ";".
  const char* open = "";
  // Plays a second of a tone through the rig first, so the meters have a level.
  bool audio = false;
  // The tone's pitch, for the tuner.
  double toneHz = 220.0;
  // The panel under the grid: "scene", "stomp" or "stomp:<switch index>".
  const char* panel = "";
  // A view over the page: "modes" (the footswitch modes overview) or "gig"
  // (the Gig View, which opens with the tuner listening).
  const char* view = "";
  // Clicks, several with ";": "gig:<switch letter>" presses and releases that
  // switch of the Gig View.
  const char* drive = "";
  // The Library page, several with ";": "setlist:<name>", "view:all",
  // "view:favourites" or "view:recent", "search:<text>", "sort:name", "sort:tempo"
  // or "sort:recent", "slot:<n>" (a slot of the shown setlist), "row:<n>",
  // "action:<label>" (a button of the selected row), "type:<text>" into the
  // dialog that is up and "confirm" it; "newsetlist", "renamesetlist:<name>",
  // "deletesetlist:<name>" (each opens its dialog), "reorder:<name>:<index>",
  // "moveto:<slot>" and "copyto:<setlist name>" for the selected preset,
  // "click:<x>,<y>" (a press at a point of the page).
  const char* library = "";
  // The importer, several with ";": "kind:qc" or "kind:pedaleira",
  // "files:<path>" (a file or folder; $fixtures and $tmp resolve), "unselect:<n>",
  // "step:choose", "step:map", "run:<n>" (import n presets, stay on the run),
  // "runall" (all of them, to the report), "outcome:<n>", "click:<x>,<y>" (a
  // press at a point of the page), "markdown" (prints the exported report).
  const char* importer = "";
  // The Settings page, several with ";": "part:audio" (midi, expression, plugins,
  // agents, remote, about), "search:<text>" in the plugin list, "scanning" (a scan
  // in progress), "unassign:<index>" (an expression assignment), "addfolder:<path>" and
  // "removefolder:<path>" (plugin folders), "tick" (what the page's timer does: levels, scans), "scroll:<px>",
  // "buffer:<samples>" (the audio device is asked), "server:on" or
  // "server:off", "agent:<name>" (a client connects to the command server) and
  // "permission:<key>:on" or ":off".
  const char* settings = "";
  // The audio device behind it: "full" (an interface with eight inputs), "busy" (two
  // drivers, dropouts, a driver panel, a refused change, the microphone off) or "none"
  // (the process has no device of its own).
  const char* device = "full";
  // Starts with the plugin cache of the board and a scan helper that exists (it is
  // never run); without, the scan is unavailable and the list empty.
  bool plugins = false;
  // The capture wizard, steps separated by ";" (CaptureScenes.h): the engine is
  // brought into a state with commands, a simulated device and a stand-in trainer.
  const char* capture = "";
};

// Joins command scripts (JSON arrays) into one.
const char* joined(std::initializer_list<const char*> parts) {
  static std::deque<std::string> kept;
  std::string out = "[";
  bool first = true;
  for (const char* part : parts) {
    std::string text = part;
    text = text.substr(text.find('[') + 1);
    text = text.substr(0, text.rfind(']'));
    if (text.find('{') == std::string::npos)
      continue;
    out += (first ? "" : ",") + text;
    first = false;
  }
  out += "]";
  kept.push_back(out);
  return kept.back().c_str();
}

// The board's full preset, the base of the split scenarios.
const char* kFullRig = R"([{"command":"grid.convert","args":{"to":"grid"}},
    {"command":"chain.addBlock","args":{"kind":"dynamics.gate","row":0,"slot":0}},
    {"command":"chain.addBlock","args":{"kind":"modulation.chorus","row":0,"slot":1}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$files/a2-amp-test.nam"],"row":0,"slot":3}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$files/cab-ir-test.wav"],"row":0,"slot":4}},
    {"command":"chain.addBlock","args":{"kind":"delay.tape","row":0,"slot":6}},
    {"command":"chain.addBlock","args":{"kind":"reverb.plate","row":0,"slot":7}},
    {"command":"split.add","args":{"row":0,"splitAt":2,"mixAt":6,"type":"balance"}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$files/a2-amp-cab-test.nam"],"row":1,"slot":3}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$files/cab-ir-test-2.wav"],"row":1,"slot":4}},
    {"command":"split.add","args":{"row":1,"splitAt":3,"mixAt":5,"type":"ab"}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$files/a2-amp-test.nam"],"row":2,"slot":3}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$files/cab-ir-test.wav"],"row":2,"slot":4}},
    {"command":"row.setInput","args":{"row":3,"source":"device","channel":2}},
    {"command":"row.setOutput","args":{"row":3,"target":"device","pair":2}},
    {"command":"chain.addBlock","args":{"kind":"dynamics.compressor","row":3,"slot":0}},
    {"command":"chain.addBlock","args":{"kind":"eq.graphic10","row":3,"slot":1}},
    {"command":"state.waitUntilLoaded","args":{}},
    {"command":"split.setParam","args":{"row":1,"param":"mix.levelB","value":-1.5}},
    {"command":"split.setParam","args":{"row":1,"param":"mix.panB","value":0.35}}])";

// The board's row for the block editors: a gate, a bypassed compressor, a
// capture and a cab from the library, a tape echo and a plate, with scene B
// active and the echo's feedback and mix given a value per scene. The
// capture's EQ is shaped like the board's.
const char* kEditorRig = R"([{"command":"grid.convert","args":{"to":"grid"}},
    {"command":"chain.addBlock","args":{"kind":"dynamics.gate","row":0,"slot":0}},
    {"command":"chain.addBlock","args":{"kind":"dynamics.compressor","row":0,"slot":1}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Amps/Brit 800 Crunch.nam"],"row":0,"slot":2}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Cabs/4x12 V30.wav"],"row":0,"slot":3}},
    {"command":"chain.addBlock","args":{"kind":"delay.tape","row":0,"slot":4}},
    {"command":"chain.addBlock","args":{"kind":"reverb.plate","row":0,"slot":5}},
    {"command":"state.waitUntilLoaded","args":{}},
    {"command":"scene.rename","args":{"scene":"A","name":"Clean"}},
    {"command":"scene.rename","args":{"scene":"B","name":"Crunch"}},
    {"command":"scene.rename","args":{"scene":"C","name":"Lead"}},
    {"command":"scene.rename","args":{"scene":"D","name":"Ambient"}},
    {"command":"scene.select","args":{"scene":"B"}},
    {"command":"block.setEnabled","args":{"blockId":"$slot:0:1","enabled":false}},
    {"command":"scene.assignParam","args":{"blockId":"$slot:0:4","param":"feedback"}},
    {"command":"scene.assignParam","args":{"blockId":"$slot:0:4","param":"mix"}},
    {"command":"scene.assignParam","args":{"blockId":"$slot:0:2","param":"outputGain"}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"sync","value":1}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"division","value":10}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"feedback","value":35}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"mix","value":0.28}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:2","param":"inputGain","value":0.55}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:2","param":"outputGain","value":0.62}},
    {"command":"block.setEqBand","args":{"blockId":"$slot:0:2","band":0,"type":"lowcut","freqHz":80,"q":0.71}},
    {"command":"block.setEqBand","args":{"blockId":"$slot:0:2","band":1,"freqHz":120,"gainDb":-1.5}},
    {"command":"block.setEqBand","args":{"blockId":"$slot:0:2","band":2,"freqHz":400,"gainDb":-2.0}},
    {"command":"block.setEqBand","args":{"blockId":"$slot:0:2","band":3,"freqHz":1200,"gainDb":2.0,"q":1.1}},
    {"command":"block.setEqBand","args":{"blockId":"$slot:0:2","band":4,"freqHz":3500,"gainDb":0.5}},
    {"command":"block.setEqBand","args":{"blockId":"$slot:0:2","band":5,"type":"highcut","freqHz":9000,"q":0.71}}])";

// The boards' rig for scenes, stomps and the Gig View: the editor's row, a
// second path with the clean amp and its cab, scene C with its own values,
// five stomp switches, and a setlist of sixteen presets with Glass Cleans in
// slot 12.
const char* kStageRig = joined({kEditorRig, R"([
    {"command":"split.add","args":{"row":0,"splitAt":2,"mixAt":4,"type":"ab"}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Amps/Twin Clean.nam"],"row":1,"slot":2}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Cabs/2x12 Blue.wav"],"row":1,"slot":3}},
    {"command":"state.waitUntilLoaded","args":{}},
    {"command":"scene.assignParam","args":{"blockId":"$slot:0:5","param":"decayS"}},
    {"command":"scene.setParamValue","args":{"scene":"A","blockId":"$slot:0:2","param":"outputGain","value":0.375}},
    {"command":"scene.setParamValue","args":{"scene":"B","blockId":"$slot:0:2","param":"outputGain","value":0.4375}},
    {"command":"scene.setParamValue","args":{"scene":"C","blockId":"$slot:0:2","param":"outputGain","value":0.5}},
    {"command":"scene.setParamValue","args":{"scene":"D","blockId":"$slot:0:2","param":"outputGain","value":0.375}},
    {"command":"scene.setParamValue","args":{"scene":"A","blockId":"$slot:0:4","param":"mix","value":0.0}},
    {"command":"scene.setParamValue","args":{"scene":"B","blockId":"$slot:0:4","param":"mix","value":0.12}},
    {"command":"scene.setParamValue","args":{"scene":"C","blockId":"$slot:0:4","param":"mix","value":0.28}},
    {"command":"scene.setParamValue","args":{"scene":"D","blockId":"$slot:0:4","param":"mix","value":0.45}},
    {"command":"scene.setParamValue","args":{"scene":"A","blockId":"$slot:0:4","param":"feedback","value":20}},
    {"command":"scene.setParamValue","args":{"scene":"B","blockId":"$slot:0:4","param":"feedback","value":20}},
    {"command":"scene.setParamValue","args":{"scene":"C","blockId":"$slot:0:4","param":"feedback","value":35}},
    {"command":"scene.setParamValue","args":{"scene":"D","blockId":"$slot:0:4","param":"feedback","value":60}},
    {"command":"scene.setParamValue","args":{"scene":"A","blockId":"$slot:0:5","param":"decayS","value":1.2}},
    {"command":"scene.setParamValue","args":{"scene":"B","blockId":"$slot:0:5","param":"decayS","value":1.2}},
    {"command":"scene.setParamValue","args":{"scene":"C","blockId":"$slot:0:5","param":"decayS","value":1.8}},
    {"command":"scene.setParamValue","args":{"scene":"D","blockId":"$slot:0:5","param":"decayS","value":6.0}},
    {"command":"stomp.assign","args":{"switch":"A","blockIds":["$slot:0:0"]}},
    {"command":"stomp.assign","args":{"switch":"B","blockIds":["$slot:0:1"]}},
    {"command":"stomp.assign","args":{"switch":"C","blockIds":["$slot:0:4"]}},
    {"command":"stomp.assign","args":{"switch":"D","blockIds":["$slot:0:5"]}},
    {"command":"stomp.assign","args":{"switch":"E","blockIds":["$slot:1:2","$slot:1:3"],"label":"Clean amp"}},
    {"command":"setlist.create","args":{"name":"Friday Covers","select":true}},
    {"command":"preset.saveToSlot","args":{"slot":1,"name":"Clean Sparkle"}},
    {"command":"preset.saveToSlot","args":{"slot":2,"name":"Chime"}},
    {"command":"preset.saveToSlot","args":{"slot":3,"name":"Country Twang"}},
    {"command":"preset.saveToSlot","args":{"slot":4,"name":"Blues Breaker"}},
    {"command":"preset.saveToSlot","args":{"slot":5,"name":"Motown Bass Amp"}},
    {"command":"preset.saveToSlot","args":{"slot":6,"name":"Dry Room"}},
    {"command":"preset.saveToSlot","args":{"slot":7,"name":"Spring Fling"}},
    {"command":"preset.saveToSlot","args":{"slot":8,"name":"Lo-Fi Radio"}},
    {"command":"preset.saveToSlot","args":{"slot":9,"name":"Surf Reverb"}},
    {"command":"preset.saveToSlot","args":{"slot":10,"name":"Jazz Box"}},
    {"command":"preset.saveToSlot","args":{"slot":11,"name":"Slow Burn"}},
    {"command":"preset.saveToSlot","args":{"slot":13,"name":"Brit Crunch"}},
    {"command":"preset.saveToSlot","args":{"slot":14,"name":"Edge of Breakup"}},
    {"command":"preset.saveToSlot","args":{"slot":15,"name":"Wall of Fuzz"}},
    {"command":"preset.saveToSlot","args":{"slot":16,"name":"Ambient Swell"}},
    {"command":"scene.select","args":{"scene":"C"}},
    {"command":"block.setEnabled","args":{"blockId":"$slot:1:2","enabled":false}},
    {"command":"block.setEnabled","args":{"blockId":"$slot:1:3","enabled":false}},
    {"command":"preset.saveToSlot","args":{"slot":12,"name":"Glass Cleans"}},
    {"command":"block.setEnabled","args":{"blockId":"$slot:0:0","enabled":false}}])"});

// Friday Covers as the Library board shows it: the stage rig's sixteen presets
// with slots 6 and 10 empty, a few favourites, three loaded in turn, and the
// loaded preset edited since it was saved.
const char* kLibraryRig = joined({kStageRig, R"([
    {"command":"preset.deleteSlot","args":{"slot":6}},
    {"command":"preset.deleteSlot","args":{"slot":10}},
    {"command":"setlist.create","args":{"name":"Studio"}},
    {"command":"setlist.create","args":{"name":"Originals"}},
    {"command":"favourite.set","args":{"id":"$preset:Clean Sparkle","favourite":true}},
    {"command":"favourite.set","args":{"id":"$preset:Edge of Breakup","favourite":true}},
    {"command":"favourite.set","args":{"id":"$preset:Slow Burn","favourite":true}},
    {"command":"favourite.set","args":{"id":"$preset:Glass Cleans","favourite":true}},
    {"command":"preset.loadSlot","args":{"slot":14}},
    {"command":"preset.loadSlot","args":{"slot":2}},
    {"command":"preset.loadSlot","args":{"slot":12}},
    {"command":"state.waitUntilLoaded","args":{}},
    {"command":"block.setEnabled","args":{"blockId":"$slot:0:1","enabled":false}}])"});

// Three presets exported as files, for the Pedaleira presets source.
const char* kExportRig = R"([{"command":"chain.addBlock","args":{"kind":"dynamics.compressor"}},
    {"command":"preset.exportJson","args":{"path":"$tmp/export/Edge Chime.json","name":"Edge Chime"}},
    {"command":"chain.addBlock","args":{"kind":"delay.tape"}},
    {"command":"preset.exportJson","args":{"path":"$tmp/export/Tape Room.json","name":"Tape Room"}},
    {"command":"chain.addBlock","args":{"kind":"reverb.plate"}},
    {"command":"preset.exportJson","args":{"path":"$tmp/export/Plate Wash.json","name":"Plate Wash"}}])";

// The Settings scenarios' rig: the full preset (inputs 1 and 2, output pairs 1 and 2) with the
// interface calibrated to the board's 12.5 dBu.
const char* kSettingsRig = joined({kFullRig, R"([
    {"command":"param.set","args":{"id":"calibrateInput","value":1}},
    {"command":"param.set","args":{"id":"inputCalibrationLevel","value":12.5}},
    {"command":"param.set","args":{"id":"gateThreshold","value":-62}}])"});

// The expression board's assignments: the echo's mix and feedback, the amp's output and the reverb's decay.
const char* kPedalRig = joined({kEditorRig, R"([
    {"command":"expression.assignParam","args":{"blockId":"$slot:0:4","param":"mix"}},
    {"command":"expression.assignParam","args":{"blockId":"$slot:0:4","param":"feedback"}},
    {"command":"expression.assignParam","args":{"blockId":"$slot:0:2","param":"outputGain"}},
    {"command":"expression.assignParam","args":{"blockId":"$slot:0:5","param":"decayS"}}])"});

// Tags on part of the library and three guitars, two of them recorded.
const char* kAgentRig = R"([
    {"command":"assets.tag","args":{"file":"$library/Amps/Brit 800 Crunch.nam","add":["crunch","brit"]}},
    {"command":"assets.tag","args":{"file":"$library/Cabs/4x12 V30.wav","add":["4x12","v30"]}},
    {"command":"guitar.create","args":{"name":"Strat","pickupType":"single-coil","positions":["bridge","middle","neck"]}},
    {"command":"guitar.create","args":{"name":"Les Paul","pickupType":"humbucker","positions":["bridge","neck"]}},
    {"command":"guitar.create","args":{"name":"Tele","pickupType":"single-coil","positions":["bridge","middle","neck"]}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Strat","position":"bridge","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Strat","position":"middle","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Strat","position":"neck","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Les Paul","position":"bridge","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Les Paul","position":"neck","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Tele","position":"bridge","take":"soft","path":"$files/cab-ir-test.wav"}}])";

// Three mappings on top of the defaults, on channel 2.
const char* kMidiRig = R"([
    {"command":"midi.setCc","args":{"targetId":"gateThreshold","cc":20}},
    {"command":"midi.setCc","args":{"targetId":"block1Power","cc":21}},
    {"command":"midi.setCc","args":{"targetId":"presetNext","cc":22}},
    {"command":"midi.setChannelFilter","args":{"channel":2}}])";

const std::vector<Scenario>& scenarios() {
  static const std::vector<Scenario> list = {
      {"gallery", "Every widget in every state, beside the board Design system", Page::grid, "[]", true},
      {"shell-new", "A fresh rig: the shell with nothing saved yet", Page::grid, "[]"},
      {"shell-unsaved", "An edited rig in stomp mode: the unsaved name and Save", Page::grid,
       R"([{"command":"preset.save","args":{"name":"Glass Cleans"}},
           {"command":"chain.addBlock","args":{"kind":"delay.simple"}},
           {"command":"footswitch.setMode","args":{"mode":"stomp"}}])"},
      {"shell-saved", "A saved preset in scene mode", Page::library,
       R"([{"command":"chain.addBlock","args":{"kind":"dynamics.compressor"}},
           {"command":"preset.save","args":{"name":"Edge Chime"}},
           {"command":"footswitch.setMode","args":{"mode":"scene"}}])"},
      {"shell-capture", "The capture page: Save becomes a secondary button", Page::capture, "[]"},
      {"grid-empty", "Grid editor, a fresh rig: one empty row and Add a row", Page::grid, "[]"},
      {"grid-full", "Grid editor, the board's full preset: three rows, a split, scenes, stomps", Page::grid,
       R"([{"command":"grid.convert","args":{"to":"grid"}},
           {"command":"chain.addBlock","args":{"kind":"dynamics.gate","row":0,"slot":0}},
           {"command":"chain.addBlock","args":{"kind":"modulation.chorus","row":0,"slot":1}},
           {"command":"chain.addToneFromFiles","args":{"paths":["$files/a2-amp-test.nam"],"row":0,"slot":2}},
           {"command":"chain.addToneFromFiles","args":{"paths":["$files/cab-ir-test.wav"],"row":0,"slot":3}},
           {"command":"chain.addBlock","args":{"kind":"delay.tape","row":0,"slot":4}},
           {"command":"chain.addBlock","args":{"kind":"reverb.plate","row":0,"slot":5}},
           {"command":"split.add","args":{"row":0,"splitAt":2,"mixAt":4,"type":"balance"}},
           {"command":"chain.addToneFromFiles","args":{"paths":["$files/a2-amp-cab-test.nam"],"row":1,"slot":2}},
           {"command":"chain.addToneFromFiles","args":{"paths":["$files/cab-ir-test-2.wav"],"row":1,"slot":3}},
           {"command":"row.setInput","args":{"row":2,"source":"device","channel":2}},
           {"command":"row.setOutput","args":{"row":2,"target":"device","pair":2}},
           {"command":"chain.addBlock","args":{"kind":"dynamics.compressor","row":2,"slot":0}},
           {"command":"chain.addBlock","args":{"kind":"eq.graphic10","row":2,"slot":1}},
           {"command":"state.waitUntilLoaded","args":{}},
           {"command":"row.setMix","args":{"row":0,"volumeDb":-2.0}},
           {"command":"row.setMix","args":{"row":2,"volumeDb":-6.0}},
           {"command":"scene.rename","args":{"scene":"A","name":"Clean"}},
           {"command":"scene.rename","args":{"scene":"B","name":"Crunch"}},
           {"command":"scene.rename","args":{"scene":"C","name":"Lead"}},
           {"command":"scene.rename","args":{"scene":"D","name":"Ambient"}},
           {"command":"scene.select","args":{"scene":"B"}},
           {"command":"block.setEnabled","args":{"blockId":"$slot:0:1","enabled":false}},
           {"command":"stomp.assign","args":{"switch":"A","blockIds":["$slot:0:1"]}},
           {"command":"stomp.assign","args":{"switch":"C","blockIds":["$slot:0:4"]}},
           {"command":"stomp.assign","args":{"switch":"D","blockIds":["$slot:0:5"]}},
           {"command":"scene.assignParam","args":{"blockId":"$slot:0:2","param":"outputGain"}},
           {"command":"preset.save","args":{"name":"Glass Cleans"}},
           {"command":"block.setParam","args":{"blockId":"$slot:0:2","param":"outputGain","value":0.44}}])",
       false, "$slot:0:2"},
      {"picker", "The block library, adding to row 1, slot 7", Page::grid, kFullRig, false, "", "", "picker:0:6"},
      {"picker-delay", "The block library filtered to delays", Page::grid, kFullRig, false, "", "", "picker:0:6:delay"},
      {"editor-effect", "A tape echo's editor: time with tempo sync, its other parameters, output", Page::grid,
       kEditorRig, false, "", "", "editor:$slot:0:4"},
      {"editor-reverb", "A plate reverb's editor: no tempo, one parameters section and the output", Page::grid,
       kEditorRig, false, "", "", "editor:$slot:0:5"},
      {"editor-nam", "A NAM capture's editor: capture info, levels, block EQ and meters", Page::grid, kEditorRig,
       false, "", "", "editor:$slot:0:2", true},
      {"editor-swap", "Swap capture: the library over the editor, on the NAM captures", Page::grid, kEditorRig, false,
       "", "", "editor:$slot:0:2;replace:2"},
      {"editor-ir", "An IR's editor: file, its folder's IRs, levels", Page::grid, kEditorRig, false, "", "",
       "editor:$slot:0:3"},
      {"grid-threeway", "Three amps in parallel: row 1 splits into row 2, row 2 into row 3", Page::grid, kFullRig},
      {"grid-splitter", "The splitter of row 1 selected", Page::grid, kFullRig, false, "", "split:0"},
      {"grid-mixer", "The mixer of row 2 selected", Page::grid, kFullRig, false, "", "mix:1"},
      {"scene-panel", "Scenes: scene C active, its name and buttons, values per scene, bypass states", Page::grid,
       joined({kStageRig, R"([{"command":"footswitch.setMode","args":{"mode":"scene"}}])"}), false, "", "", "", false, 220.0,
       "scene"},
      {"scene-panel-fresh", "Scenes on a fresh rig: no values per scene, no blocks", Page::grid,
       R"([{"command":"footswitch.setMode","args":{"mode":"scene"}}])", false, "", "", "", false, 220.0, "scene"},
      {"stomp-assign", "Stomp assignment: five switches, E selected with two blocks, label and momentary", Page::grid,
       joined({kStageRig, R"([{"command":"footswitch.setMode","args":{"mode":"stomp"}},
                              {"command":"stomp.setOptions","args":{"switch":"E","momentary":true}}])"}),
       false, "", "", "", false, 220.0, "stomp:4"},
      {"stomp-none", "Stomp assignment on a rig with no switch assigned", Page::grid,
       R"([{"command":"chain.addBlock","args":{"kind":"dynamics.gate"}},
           {"command":"footswitch.setMode","args":{"mode":"stomp"}}])",
       false, "", "", "", false, 220.0, "stomp"},
      {"modes", "Footswitch modes, Scene current", Page::grid,
       joined({kStageRig, R"([{"command":"footswitch.setMode","args":{"mode":"scene"}}])"}), false, "", "", "", false,
       220.0, "", "modes"},
      {"modes-hybrid", "Footswitch modes, Hybrid current with a scene row over a stomp row", Page::grid,
       joined({kStageRig, R"([{"command":"footswitch.setHybridRows","args":{"top":"scene","bottom":"stomp"}},
                              {"command":"footswitch.setMode","args":{"mode":"hybrid"}}])"}),
       false, "", "", "", false, 220.0, "", "modes"},
      {"gig-view", "Gig View in Hybrid: scenes over stomps, a +3 cent A on the tuner, scene B pressed by click", Page::grid,
       joined({kStageRig, R"([{"command":"tuner.setEnabled","args":{"enabled":true}},
                              {"command":"footswitch.setHybridRows","args":{"top":"scene","bottom":"stomp"}},
                              {"command":"footswitch.setMode","args":{"mode":"hybrid"}},
                              {"command":"tempo.set","args":{"bpm":120}}])"}),
       false, "", "", "", true, 440.76, "", "gig", "gig:B"},
      {"gig-flat", "Gig View in Preset mode, a note 16 cents flat on the tuner", Page::grid,
       joined({kStageRig, R"([{"command":"tuner.setEnabled","args":{"enabled":true}},
                              {"command":"footswitch.setMode","args":{"mode":"preset"}},
                              {"command":"footswitch.press","args":{"switch":"bankUp"}},
                              {"command":"footswitch.release","args":{"switch":"bankUp"}}])"}),
       false, "", "", "", true, 436.0, "", "gig"},
      {"gig-fresh", "Gig View on a fresh rig, nothing played", Page::grid, "[]", false, "", "", "", false, 220.0, "", "gig"},
      {.name = "library", .description = "Library: Friday Covers with gaps, the loaded unsaved preset, favourites, details",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Friday Covers;slot:12"},
      {.name = "library-actions", .description = "Library: a preset selected, its action bar under the row",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Friday Covers;slot:4"},
      {.name = "library-empty", .description = "Library: an empty slot selected, Save current rig here",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Friday Covers;slot:6"},
      {.name = "library-delete", .description = "Library: Delete asks first, in a dialog over the page",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Friday Covers;slot:4;action:Delete"},
      {.name = "library-rename", .description = "Library: Rename takes the name in a dialog",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Friday Covers;slot:3;action:Rename"},
      {.name = "library-edited", .description = "Library after edits: renamed, duplicated, deleted, saved into a gap, moved, copied",
       .page = Page::library, .script = kLibraryRig,
       .library = "setlist:Friday Covers;slot:3;action:Rename;type:Country Rock;confirm;slot:4;action:Duplicate;"
                  "slot:8;action:Delete;confirm;slot:8;action:Save current rig here;type:New Take;confirm;copyto:Studio;moveto:2;"
                  "renamesetlist:Originals;type:Originals 2025;confirm;reorder:Originals 2025:0;newsetlist;type:Weekend;confirm;"
                  "setlist:Friday Covers;slot:2"},
      {.name = "library-favourites", .description = "Library: the Favourites view across setlists",
       .page = Page::library, .script = kLibraryRig, .library = "view:favourites"},
      {.name = "library-clicks", .description = "Library by mouse: the Studio card, then Favourites, then the star of the second row",
       .page = Page::library, .script = kLibraryRig, .library = "click:128,306;click:78,106;click:976,146"},
      {.name = "library-recent", .description = "Library: the Recent view, the last loaded first",
       .page = Page::library, .script = kLibraryRig, .library = "view:recent"},
      {.name = "library-search", .description = "Library: a search for \"ing\", sorted by name, no empty rows",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Friday Covers;search:ing;sort:name;row:0"},
      {.name = "library-all", .description = "Library: All presets across the user's setlists",
       .page = Page::library, .script = kLibraryRig, .library = "view:all;row:2"},
      {.name = "library-factory", .description = "Library: the read-only factory setlist",
       .page = Page::library, .script = kLibraryRig, .library = "setlist:Factory;row:0"},
      {.name = "library-fresh", .description = "Library: a fresh library, nothing saved", .page = Page::library, .script = "[]"},
      {.name = "import-source", .description = "Import, step 1: the four sources, Quad Cortex and how to make the dump",
       .page = Page::import, .script = "[]"},
      {.name = "import-source-files", .description = "Import, step 1: dump files chosen, one of them not a preset",
       .page = Page::import, .script = "[]", .importer = "kind:qc;files:$fixtures/quadcortex"},
      {.name = "import-source-click", .description = "Import, step 1: a press on the Pedaleira presets card selects it",
       .page = Page::import, .script = "[]", .importer = "click:1188,286"},
      {.name = "import-source-pedaleira", .description = "Import, step 1: Pedaleira presets selected",
       .page = Page::import, .script = "[]", .importer = "kind:pedaleira"},
      {.name = "import-choose", .description = "Import, step 2: choose presets, the setlist they go to",
       .page = Page::import, .script = kStageRig, .importer = "kind:qc;files:$fixtures/quadcortex;step:choose;click:427,372"},
      {.name = "import-map", .description = "Import, step 3: the mapping table read against the chosen presets",
       .page = Page::import, .script = kStageRig, .importer = "kind:qc;files:$fixtures/quadcortex;step:map"},
      {.name = "import-run", .description = "Import, step 4: three of six presets in",
       .page = Page::import, .script = kStageRig, .importer = "kind:qc;files:$fixtures/quadcortex;step:map;run:3"},
      {.name = "import-report", .description = "Import, step 5: the report, a preset that needs a capture selected",
       .page = Page::import, .script = kStageRig, .importer = "kind:qc;files:$fixtures/quadcortex;step:map;runall;click:218,283"},
      {.name = "import-markdown", .description = "Import: the same report as Markdown, printed (no picture of interest)",
       .page = Page::import, .script = kStageRig, .importer = "kind:qc;files:$fixtures/quadcortex;step:map;runall;markdown"},
      {.name = "import-report-clean", .description = "Import, step 5: a preset with nothing but an unverified mapping",
       .page = Page::import, .script = kStageRig, .importer = "kind:qc;files:$fixtures/quadcortex;step:map;runall;outcome:4"},
      {.name = "import-pedaleira", .description = "Import: three Pedaleira presets from a folder, the report",
       .page = Page::import, .script = kExportRig, .importer = "kind:pedaleira;files:$tmp/export;step:choose;runall"},
      {.name = "settings-audio", .description = "Settings, audio: an interface, the inputs in use, calibration, gate, oversampling",
       .page = Page::settings, .script = kSettingsRig, .audio = true, .settings = "part:audio;tick"},
      {.name = "settings-audio-lower", .description = "Settings, audio, scrolled: the rest of the inputs, the gate, performance",
       .page = Page::settings, .script = kSettingsRig, .audio = true, .settings = "part:audio;tick;scroll:560"},
      {.name = "settings-audio-busy", .description = "Settings, audio: two drivers, dropouts, a refused change, the microphone off",
       .page = Page::settings, .script = kSettingsRig, .settings = "part:audio;buffer:64", .device = "busy"},
      {.name = "settings-audio-none", .description = "Settings, audio: a process with no device of its own",
       .page = Page::settings, .script = kSettingsRig, .settings = "part:audio", .device = "none"},
      {.name = "settings-expression", .description = "Settings, expression: four parameters set aside for the pedal",
       .page = Page::settings, .script = kPedalRig, .settings = "part:expression"},
      {.name = "settings-expression-edited", .description = "Settings, expression: the first assignment removed",
       .page = Page::settings, .script = kPedalRig, .settings = "part:expression;unassign:0"},
      {.name = "settings-expression-empty", .description = "Settings, expression: nothing assigned",
       .page = Page::settings, .script = "[]", .settings = "part:expression"},
      {.name = "settings-plugins", .description = "Settings, plugins: nine found, one failed, an instrument, one missing",
       .page = Page::settings, .script = "[]", .settings = "part:plugins", .plugins = true},
      {.name = "settings-plugins-folders", .description = "Settings, plugins: a folder added and one removed",
       .page = Page::settings, .script = "[]",
       .settings = "part:plugins;addfolder:$tmp/Library;removefolder:/Library/Audio/Plug-Ins/Components", .plugins = true},
      {.name = "settings-plugins-scanning", .description = "Settings, plugins: a scan 142 of 187 in",
       .page = Page::settings, .script = "[]", .settings = "part:plugins;scanning", .plugins = true},
      {.name = "settings-plugins-search", .description = "Settings, plugins: a search for \"sl\"",
       .page = Page::settings, .script = "[]", .settings = "part:plugins;search:sl", .plugins = true},
      {.name = "settings-plugins-empty", .description = "Settings, plugins: nothing scanned, no scan helper",
       .page = Page::settings, .script = "[]", .settings = "part:plugins"},
      {.name = "settings-agents", .description = "Settings, AI and MCP: the server running, an agent, tags, three guitars",
       .page = Page::settings, .script = kAgentRig, .settings = "part:agents;server:on;agent:Claude Code"},
      {.name = "settings-agents-off", .description = "Settings, AI and MCP: the server off, a permission turned on",
       .page = Page::settings, .script = kAgentRig, .settings = "part:agents;permission:live:on"},
      {.name = "settings-midi", .description = "Settings, MIDI: the defaults and three mappings, read only",
       .page = Page::settings, .script = kMidiRig, .settings = "part:midi"},
      {.name = "settings-remote", .description = "Settings, remote: a later build", .page = Page::settings, .script = "[]",
       .settings = "part:remote"},
      {.name = "settings-about", .description = "Settings, about: version, licences and credits", .page = Page::settings,
       .script = "[]", .settings = "part:about"},
      // The capture wizard. kReampLevels brings a re-amp to the levels step with the tone in range;
      // kReampRecord to the record step with a stand-in input file.
      {.name = "capture-source", .description = "Capture, step 1: what are you capturing, nothing typed yet",
       .page = Page::capture, .script = "[]"},
      {.name = "capture-source-filled", .description = "Capture, step 1: Amp Head chosen, a name typed",
       .page = Page::capture, .script = "[]", .capture = "name:Brit 800 Crunch;source:1"},
      {.name = "capture-source-pedal", .description = "Capture, step 1: a pedal chosen",
       .page = Page::capture, .script = "[]", .capture = "name:Tube Screamer;source:0"},
      {.name = "capture-wiring-box", .description = "Capture, step 2: amp head through a re-amp box",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:reamp;goto:wiring"},
      {.name = "capture-wiring-nobox", .description = "Capture, step 2: without a re-amp box, the send starts at -30 dB",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:reamp;goto:wiring;variant:1"},
      {.name = "capture-wiring-drywet", .description = "Capture, step 2: play it in, dry and wet recorded together",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:dryWet;goto:wiring"},
      {.name = "capture-wiring-pedal", .description = "Capture, step 2: a pedal through a re-amp box, outputs 2 and inputs 1",
       .page = Page::capture, .script = "[]", .capture = "new:pedal:reamp;goto:wiring;channels:1:0"},
      {.name = "capture-wiring-cab", .description = "Capture, step 2: amp and cab with a microphone",
       .page = Page::capture, .script = "[]", .capture = "new:ampCab:reamp;goto:wiring"},
      {.name = "capture-wiring-modeller", .description = "Capture, step 2: another modeller, without a box",
       .page = Page::capture, .script = "[]", .capture = "new:modeller:reamp;goto:wiring;variant:1"},
      {.name = "capture-levels-idle", .description = "Capture, step 3: the tone off, nothing measured yet",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:reamp;goto:wiring:levels"},
      {.name = "capture-levels-inrange", .description = "Capture, step 3: the tone playing, the return in range, the noise floor measured",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;noise"},
      {.name = "capture-levels-low", .description = "Capture, step 3: the return too low",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:0.8"},
      {.name = "capture-levels-clipping", .description = "Capture, step 3: the return clipping",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:40"},
      {.name = "capture-levels-drywet", .description = "Capture, step 3: dry/wet, both inputs metered while the guitar is played",
       .page = Page::capture, .script = "[]", .capture = "new:ampHead:dryWet;goto:wiring:levels;listen:2.5"},
      {.name = "capture-record-nofile", .description = "Capture, step 4: no standard input file chosen yet",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:none;open"},
      {.name = "capture-record-unrecognised", .description = "Capture, step 4: a file the trainer would not recognise",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;open"},
      {.name = "capture-record-running", .description = "Capture, step 4: recording, a third of the way in",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:38:2.5"},
      {.name = "capture-record-complete", .description = "Capture, step 4: the recording is done",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5"},
      {.name = "capture-check-pass", .description = "Capture, step 5: the checks all pass, the trainer is ready",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks"},
      {.name = "capture-check-warn", .description = "Capture, step 5: a warning (level a little low)",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:1.6;python:ok;trainer;checks"},
      {.name = "capture-check-fail", .description = "Capture, step 5: the recording clipped, the trainer is not installed",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:40;python:none;trainer;checks"},
      {.name = "capture-train-running", .description = "Capture, step 6: epoch 46 of 100",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks;"
                  "train:running:46"},
      {.name = "capture-train-failed", .description = "Capture, step 6: the trainer's own data checks failed",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks;"
                  "train:fail:12"},
      {.name = "capture-train-stopped", .description = "Capture, step 6: training stopped, the trainer cannot be found",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks;"
                  "train:running:8;cancel;python:none;trainer;input:missing;open"},
      {.name = "capture-review", .description = "Capture, step 7: trained, ESR 0.008, ready to save",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks;"
                  "train:done:100"},
      {.name = "capture-review-rig", .description = "Capture, step 7: added to the rig to play it",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks;"
                  "train:done:100;rig"},
      {.name = "capture-review-saved", .description = "Capture, step 7: saved to the library",
       .page = Page::capture, .script = "[]",
       .capture = "new:ampHead:reamp;goto:wiring:levels;send:-20;tone:2.5;goto:record;input:wav;record:100:2.5;python:ok;trainer;checks;"
                  "train:done:100;press:primary"},
  };
  return list;
}

juce::String pageName(Page page) {
  switch (page) {
    case Page::grid: return "Grid";
    case Page::library: return "Library";
    case Page::capture: return "Capture";
    case Page::import: return "Import";
    case Page::live: return "Live";
    case Page::settings: break;
  }
  return "Settings";
}

// The scratch engine's library folder, for scripts that load from it.
juce::File& libraryFolder() {
  static juce::File folder;
  return folder;
}

// The scratch engine's data folder, for scripts that write files ($tmp/...).
juce::File& scratchFolder() {
  static juce::File folder;
  return folder;
}

// The fixtures the importer's scenarios read ($fixtures/...).
juce::File fixtureFolder() {
  return juce::File(PEDALEIRA_SOURCE_DIR).getChildFile("plugin/ui/pedaleira/testbed/fixtures");
}

// The board "Settings, plugins" as the scanner's cache file: nine plugins of
// the board, one that failed its scan, an instrument, one gone from disk.
void seedPluginCache(const juce::File& cacheFile) {
  struct Seed {
    const char* name;
    const char* vendor;
    const char* format;
    int in, out;
    const char* state;
    const char* failure;
  };
  const Seed seeds[] = {
      {"Seventh Heaven", "LiquidSonics", "VST3", 2, 2, "ok", ""},
      {"Virtual Mix Rack", "Slate Digital", "AudioUnit", 2, 2, "ok", ""},
      {"Native Channel Strip 2", "SSL", "VST3", 2, 2, "ok", ""},
      {"Pro-Q 3", "FabFilter", "VST3", 2, 2, "ok", ""},
      {"ValhallaDelay", "Valhalla DSP", "AudioUnit", 1, 2, "ok", ""},
      {"Crashy Chorus", "Acme Audio", "VST3", 2, 2, "failed", "the scan helper crashed"},
      {"Hollow Keys", "Acme Audio", "VST3", 0, 2, "instrument", ""},
      {"Old Spring Tank", "Retro Labs", "VST3", 2, 2, "missing", ""},
      {"Kirchhoff EQ", "Sonnox", "AudioUnit", 2, 2, "ok", ""},
  };
  juce::XmlElement root("PedaleiraPluginCache");
  root.setAttribute("version", 1);
  for (const char* folder : {"/Library/Audio/Plug-Ins/VST3", "~/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/Components"})
    root.createNewChildElement("Folder")->setAttribute("path", folder);
  int uid = 100;
  for (const auto& seed : seeds) {
    const juce::String format = seed.format;
    juce::PluginDescription description;
    description.name = seed.name;
    description.manufacturerName = seed.vendor;
    description.pluginFormatName = format;
    description.category = "Fx";
    description.version = "1.0.0";
    description.numInputChannels = seed.in;
    description.numOutputChannels = seed.out;
    description.isInstrument = juce::String(seed.state) == "instrument";
    description.uniqueId = description.deprecatedUid = ++uid;
    description.fileOrIdentifier = format == "AudioUnit"
                                       ? "AudioUnit:Effects/aufx,fx" + juce::String(uid) + ",test"
                                       : "/Library/Audio/Plug-Ins/VST3/" + juce::String(seed.name) + ".vst3";
    auto* record = root.createNewChildElement("Record");
    record->setAttribute("format", format);
    record->setAttribute("file", description.fileOrIdentifier);
    record->setAttribute("stamp", "1");
    record->setAttribute("present", juce::String(seed.state) != "missing");
    if (juce::String(seed.failure).isNotEmpty())
      record->setAttribute("failure", seed.failure);
    else
      record->addChildElement(description.createXml().release());
  }
  cacheFile.getParentDirectory().createDirectory();
  root.writeTo(cacheFile);
}

// The engine's files go to a scratch folder that goes with the process.
struct ScratchEngine {
  explicit ScratchEngine(bool withPlugins = false) {
    data = juce::File::getSpecialLocation(juce::File::tempDirectory)
               .getChildFile("PedaleiraUiTestbed-" + juce::Uuid().toDashedString());
    data.createDirectory();
    const juce::File library = data.getChildFile("Library");
    library.createDirectory();
    // A few captures and IRs in the library, for the block library to list.
    const juce::File files = juce::File(PEDALEIRA_SOURCE_DIR).getChildFile("test").getChildFile("files");
    library.getChildFile("Amps").createDirectory();
    library.getChildFile("Cabs").createDirectory();
    files.getChildFile("a2-amp-test.nam").copyFileTo(library.getChildFile("Amps").getChildFile("Brit 800 Crunch.nam"));
    files.getChildFile("a2-amp-cab-test.nam").copyFileTo(library.getChildFile("Amps").getChildFile("Twin Clean.nam"));
    files.getChildFile("cab-ir-test.wav").copyFileTo(library.getChildFile("Cabs").getChildFile("4x12 V30.wav"));
    files.getChildFile("cab-ir-test-2.wav").copyFileTo(library.getChildFile("Cabs").getChildFile("2x12 Blue.wav"));
    libraryFolder() = library;
    scratchFolder() = data;
    TONE3000Processor::disableMachineDefaultParametersForTesting();
    TONE3000Processor::setFootswitchSettingsFileForTesting(data.getChildFile("footswitch.settings"));
    data.getChildFile("LocalModels").createDirectory();
    TONE3000Processor::setLocalModelsDirForTesting(data.getChildFile("LocalModels"));
    data.getChildFile("PluginCache").createDirectory();
    PluginScanner::setCacheDirForTesting(data.getChildFile("PluginCache"));
    PluginScanner::setDefaultLocationsEnabled(false);
    if (withPlugins) {
      seedPluginCache(data.getChildFile("PluginCache").getChildFile("PluginCache.xml"));
      // Any file that exists makes the scanner available; it is never run.
      PluginScanner::setHelperForTesting(juce::File::getSpecialLocation(juce::File::currentExecutableFile));
    } else {
      PluginScanner::setHelperForTesting(data.getChildFile("no-such-helper"));
    }
    AgentSettings::setFileForTesting(data.getChildFile("agents.settings"));
    CommandServer::setConnectionFileForTesting(data.getChildFile("command-server.json"));
    LibraryPaths::setLibraryRootForTesting(library);
    SetlistLibrary::setTrashDirForTesting(data.getChildFile("Trash"));
    data.getChildFile("Scratch").createDirectory();
    LibraryPaths::setScratchDirForTesting(data.getChildFile("Scratch"));
    data.getChildFile("Presets").createDirectory();

    processor = std::make_unique<TONE3000Processor>();
    processor->setMultiCoreEnabled(false, /*persist=*/false);
    processor->setPresetStoreForTesting(data.getChildFile("Presets"), library);
    processor->setPlayConfigDetails(2, 2, 48000.0, 128);
    processor->prepareToPlay(48000.0, 128);
    commands = std::make_unique<EngineCommands>(*processor);
    client = std::make_unique<pedaleira::ui::LocalEngineClient>(commands->agents());
  }

  ~ScratchEngine() {
    client.reset();
    commands.reset();
    processor.reset();
    data.deleteRecursively();
  }

  juce::File data;
  std::unique_ptr<TONE3000Processor> processor;
  std::unique_ptr<EngineCommands> commands;
  std::unique_ptr<pedaleira::ui::LocalEngineClient> client;
};

// The repo's test files, for scripts that load captures and IRs.
juce::File testFiles() {
  return juce::File(PEDALEIRA_SOURCE_DIR).getChildFile("test").getChildFile("files");
}

// A script names a block by where it is ("$slot:row:slot") and a test file
// by "$files/<name>"; both are resolved when the step runs.
juce::var resolve(pedaleira::ui::EngineClient& client, const juce::var& value) {
  if (value.isString()) {
    const juce::String text = value.toString();
    if (text.startsWith("$library/"))
      return libraryFolder().getChildFile(text.fromFirstOccurrenceOf("/", false, false)).getFullPathName();
    if (text.startsWith("$tmp/"))
      return scratchFolder().getChildFile(text.fromFirstOccurrenceOf("/", false, false)).getFullPathName();
    if (text.startsWith("$fixtures/"))
      return fixtureFolder().getChildFile(text.fromFirstOccurrenceOf("/", false, false)).getFullPathName();
    if (text.startsWith("$preset:")) {
      // The id of the preset with this name, in whichever setlist holds it.
      const juce::String wanted = text.fromFirstOccurrenceOf(":", false, false);
      const juce::var setlists = client.read("setlist.list");
      if (const auto* list = setlists["setlists"].getArray())
        for (const auto& setlist : *list) {
          const juce::var full = client.read("setlist.get", pedaleira::ui::object({{"setlist", setlist["id"]}}));
          if (const auto* slots = full["slots"].getArray())
            for (const auto& slot : *slots)
              if (slot["name"].toString() == wanted)
                return slot["id"];
        }
      return {};
    }
    if (text.startsWith("$guitar:")) {
      // The id of the guitar profile with this name.
      const juce::String wanted = text.fromFirstOccurrenceOf(":", false, false);
      // Kept in a local: the list must outlive the loop over it.
      const juce::var guitars = client.read("guitar.list")["guitars"];
      if (const auto* list = guitars.getArray())
        for (const auto& guitar : *list)
          if (guitar["name"].toString() == wanted)
            return guitar["id"];
      return {};
    }
    if (text.startsWith("$files/"))
      return testFiles().getChildFile(text.fromFirstOccurrenceOf("/", false, false)).getFullPathName();
    if (text.startsWith("$slot:")) {
      const juce::StringArray parts = juce::StringArray::fromTokens(text.substring(6), ":", "");
      const juce::var grid = client.read("grid.get");
      return grid["rows"][parts[0].getIntValue()]["slots"][parts[1].getIntValue()];
    }
    return value;
  }
  if (const auto* list = value.getArray()) {
    juce::Array<juce::var> out;
    for (const auto& item : *list)
      out.add(resolve(client, item));
    return out;
  }
  if (auto* object = value.getDynamicObject()) {
    juce::DynamicObject::Ptr out = new juce::DynamicObject();
    for (const auto& property : object->getProperties())
      out->setProperty(property.name, resolve(client, property.value));
    return juce::var(out.get());
  }
  return value;
}

bool runScript(pedaleira::ui::EngineClient& client, const juce::String& script, juce::String& error) {
  const juce::var steps = juce::JSON::parse(script);
  if (!steps.isArray()) {
    error = "the script is not a JSON array";
    return false;
  }
  for (const auto& step : *steps.getArray()) {
    const auto reply = client.run(step["command"].toString(), resolve(client, step["args"]));
    if (!reply.ok) {
      error = step["command"].toString() + ": " + reply.errorCode + ": " + reply.errorMessage;
      return false;
    }
  }
  return true;
}

// A tone through the rig, so its meters read something.
void playTone(TONE3000Processor& processor, double hz) {
  juce::AudioBuffer<float> buffer(2, 128);
  juce::MidiBuffer midi;
  double phase = 0.0;
  for (int block = 0; block < 200; ++block) {
    for (int i = 0; i < 128; ++i) {
      const float sample = 0.25f * std::sin(static_cast<float>(phase));
      phase += 2.0 * juce::MathConstants<double>::pi * hz / 48000.0;
      buffer.setSample(0, i, sample);
      buffer.setSample(1, i, sample);
    }
    processor.processBlock(buffer, midi);
  }
}

// A mouse press at a point of a page, as the mouse would make it (the pages
// act on the press).
void clickAt(juce::Component& page, const juce::String& point) {
  const juce::Point<float> at(point.upToFirstOccurrenceOf(",", false, false).getFloatValue(),
                              point.fromFirstOccurrenceOf(",", false, false).getFloatValue());
  const juce::Time now = juce::Time::getCurrentTime();
  const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, &page, &page, now, at, now, 1, false);
  page.mouseDown(event);
}

// The setlist id with this name.
juce::String setlistIdNamed(pedaleira::ui::EngineClient& client, const juce::String& name) {
  const juce::var setlists = client.read("setlist.list");
  if (const auto* list = setlists["setlists"].getArray())
    for (const auto& setlist : *list)
      if (setlist["name"].toString() == name)
        return setlist["id"].toString();
  return {};
}

// Plays the Library scenario's clicks.
void driveLibrary(pedaleira::ui::EngineClient& client, pedaleira::ui::LibraryPage& library, const juce::String& drive) {
  using Page = pedaleira::ui::LibraryPage;
  for (const auto& step : juce::StringArray::fromTokens(drive, ";", "")) {
    const juce::String key = step.upToFirstOccurrenceOf(":", false, false);
    const juce::String value = step.fromFirstOccurrenceOf(":", false, false);
    if (key == "setlist") {
      library.showSetlist(setlistIdNamed(client, value));
    } else if (key == "view") {
      library.showView(value == "all" ? Page::View::all : value == "favourites" ? Page::View::favourites : Page::View::recent);
    } else if (key == "search") {
      library.setSearch(value);
    } else if (key == "sort") {
      library.setSort(value == "name" ? Page::Sort::name : value == "tempo" ? Page::Sort::tempo : Page::Sort::recent);
    } else if (key == "slot") {
      library.selectSlot(value.getIntValue());
    } else if (key == "row") {
      library.selectRow(value.getIntValue());
    } else if (key == "action") {
      library.pressAction(value);
    } else if (key == "type") {
      library.dialog().field().setText(value, false);
    } else if (key == "confirm") {
      library.dialog().confirmNow();
    } else if (key == "click") {
      clickAt(library, value);
    } else if (key == "newsetlist") {
      library.newSetlist();
    } else if (key == "renamesetlist") {
      library.renameSetlistDialog(setlistIdNamed(client, value));
    } else if (key == "deletesetlist") {
      library.deleteSetlistDialog(setlistIdNamed(client, value));
    } else if (key == "reorder") {
      library.reorderSetlist(setlistIdNamed(client, value.upToLastOccurrenceOf(":", false, false)),
                             value.fromLastOccurrenceOf(":", false, false).getIntValue());
    } else if (key == "moveto") {
      library.moveSelected(value.getIntValue());
    } else if (key == "copyto") {
      library.copySelectedTo(setlistIdNamed(client, value));
    }
  }
}

// Plays the importer scenario's choices.
void driveImport(pedaleira::ui::EngineClient& client, pedaleira::ui::ImportPage& importer, const juce::String& drive) {
  using Step = pedaleira::ui::ImportPage::Step;
  for (const auto& step : juce::StringArray::fromTokens(drive, ";", "")) {
    const juce::String key = step.upToFirstOccurrenceOf(":", false, false);
    const juce::String value = step.fromFirstOccurrenceOf(":", false, false);
    if (key == "kind") {
      importer.chooseKind(value == "qc" ? pedaleira::ui::ImportKind::quadCortex : pedaleira::ui::ImportKind::pedaleira);
    } else if (key == "files") {
      importer.setFiles({juce::File(resolve(client, value).toString())});
    } else if (key == "unselect") {
      importer.setSelected(value.getIntValue(), false);
    } else if (key == "step") {
      importer.goTo(value == "choose" ? Step::choose : Step::map);
    } else if (key == "run") {
      importer.runNow(value.getIntValue());
    } else if (key == "runall") {
      importer.runNow();
    } else if (key == "outcome") {
      importer.selectOutcome(value.getIntValue());
    } else if (key == "click") {
      clickAt(importer, value);
    } else if (key == "markdown") {
      // The exported report, on stdout.
      std::cout << importer.markdown() << "\n";
    }
  }
}

// Clients connected to the command server, kept open until the scenario ends.
std::vector<std::unique_ptr<juce::StreamingSocket>>& agentSockets() {
  static std::vector<std::unique_ptr<juce::StreamingSocket>> sockets;
  return sockets;
}

// An agent connecting as a real client would: the token from the connection
// file, then one command so the list has a last action.
void connectAgent(const juce::String& name) {
  const juce::var file = juce::JSON::parse(scratchFolder().getChildFile("command-server.json"));
  if (!file.isObject())
    return;
  auto socket = std::make_unique<juce::StreamingSocket>();
  if (!socket->connect("127.0.0.1", static_cast<int>(file["port"]), 2000))
    return;
  const juce::String hello = juce::JSON::toString(pedaleira::ui::object({{"auth", file["token"]}, {"client", name}}), true) + "\n";
  const juce::String request = "{\"id\":1,\"command\":\"preset.list\"}\n";
  socket->write(hello.toRawUTF8(), static_cast<int>(hello.getNumBytesAsUTF8()));
  socket->write(request.toRawUTF8(), static_cast<int>(request.getNumBytesAsUTF8()));
  // The message thread serves the request.
  juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
  agentSockets().push_back(std::move(socket));
}

// Plays the Settings scenario's choices.
void driveSettings(pedaleira::ui::EngineClient& client, pedaleira::ui::SettingsPage& page, const juce::String& drive) {
  using Part = pedaleira::ui::SettingsPage::Part;
  const juce::StringArray names{"audio", "midi", "expression", "plugins", "agents", "remote", "about"};
  for (const auto& step : juce::StringArray::fromTokens(drive, ";", "")) {
    const juce::String key = step.upToFirstOccurrenceOf(":", false, false);
    const juce::String value = step.fromFirstOccurrenceOf(":", false, false);
    auto& plugins = static_cast<pedaleira::ui::SettingsPlugins&>(page.section(Part::plugins));
    auto& agents = static_cast<pedaleira::ui::SettingsAgents&>(page.section(Part::agents));
    if (key == "part") {
      page.showPart(static_cast<Part>(juce::jmax(0, names.indexOf(value))));
    } else if (key == "tick") {
      page.tickNow();
    } else if (key == "scroll") {
      page.scrollTo(value.getIntValue());
    } else if (key == "unassign") {
      static_cast<pedaleira::ui::SettingsExpression&>(page.section(Part::expression)).unassign(value.getIntValue());
    } else if (key == "addfolder") {
      plugins.addFolderPath(resolve(client, value).toString());
    } else if (key == "removefolder") {
      plugins.removeFolderPath(value);
    } else if (key == "search") {
      plugins.setSearch(value);
    } else if (key == "scanning") {
      plugins.showScan(pedaleira::ui::object({{"state", "scanning"},
                                              {"done", 142},
                                              {"total", 187},
                                              {"currentFile", "/Library/Audio/Plug-Ins/VST3/Seventh Heaven.vst3"}}));
    } else if (key == "buffer") {
      static_cast<pedaleira::ui::SettingsAudio&>(page.section(Part::audio)).chooseBuffer(value.getIntValue());
    } else if (key == "server") {
      agents.setServerEnabled(value == "on");
    } else if (key == "agent") {
      connectAgent(value);
      agents.refresh();
    } else if (key == "permission") {
      agents.setPermission(value.upToFirstOccurrenceOf(":", false, false), value.endsWith(":on"));
    }
  }
}

int capture(const juce::File& folder, float scale, const juce::StringArray& only) {
  if (!folder.createDirectory()) {
    std::cerr << "cannot make " << folder.getFullPathName() << "\n";
    return 1;
  }
  int failures = 0;
  for (const auto& scenario : scenarios()) {
    if (!only.isEmpty() && !only.contains(scenario.name))
      continue;
    ScratchEngine engine(scenario.plugins);
    juce::String error;
    if (!runScript(*engine.client, scenario.script, error)) {
      std::cerr << scenario.name << ": " << error << "\n";
      ++failures;
      continue;
    }
    if (scenario.audio)
      playTone(*engine.processor, scenario.toneHz);
    pedaleira::ui::AppShell shell(*engine.client);
    // The canned audio device, which the Settings page outlives.
    std::unique_ptr<pedaleira::ui::testbed::FakeDevice> device;
    if (const juce::String kind(scenario.device); kind != "none") {
      pedaleira::ui::testbed::FakeDevice::Setup setup;
      if (kind == "full") {
        setup.inputChannels = 8;
      } else {
        setup.drivers = 2;
        setup.inputChannels = 2;
        setup.outputChannels = 2;
        setup.dropouts = 3;
        setup.micDenied = true;
        setup.controlPanel = true;
        setup.error = "The device is in use by another application.";
      }
      device = std::make_unique<pedaleira::ui::testbed::FakeDevice>(setup);
    }
    std::vector<std::unique_ptr<juce::Component>> pages;
    for (Page page : {Page::grid, Page::library, Page::capture, Page::import, Page::live, Page::settings}) {
      if (scenario.gallery && page == Page::grid) {
        pages.push_back(std::make_unique<pedaleira::ui::testbed::Gallery>());
      } else if (page == Page::grid) {
        auto grid = std::make_unique<pedaleira::ui::GridPage>(*engine.client);
        if (juce::String(scenario.select).isNotEmpty())
          grid->selectBlock(resolve(*engine.client, juce::String(scenario.select)).toString());
        for (const auto& open : juce::StringArray::fromTokens(scenario.open, ";", "")) {
          if (open.startsWith("picker:")) {
            const juce::StringArray parts = juce::StringArray::fromTokens(open.substring(7), ":", "");
            grid->openPicker(parts[0].getIntValue(), parts[1].getIntValue());
            if (parts.size() > 2)
              grid->picker().setFilter(parts[2], 0, {});
          } else if (open.startsWith("editor:")) {
            grid->openEditor(resolve(*engine.client, open.substring(7)).toString());
          } else if (open.startsWith("replace:")) {
            // What the editor's Swap capture or Load button asks for: the
            // library over the slot, on that source.
            if (auto* body = grid->blockEditor().currentBody())
              body->onReplace(open.fromFirstOccurrenceOf(":", false, false).getIntValue());
          }
        }
        if (const juce::String point(scenario.point); point.isNotEmpty())
          grid->selectPoint({point.fromFirstOccurrenceOf(":", false, false).getIntValue(), point.startsWith("mix")});
        if (const juce::String panel(scenario.panel); panel.isNotEmpty()) {
          if (panel.startsWith("scene"))
            grid->showPanel(pedaleira::ui::GridPage::Panel::scene);
          else if (panel.startsWith("stomp")) {
            grid->showPanel(pedaleira::ui::GridPage::Panel::stomp);
            if (panel.contains(":"))
              grid->stompView().selectSwitch(panel.fromFirstOccurrenceOf(":", false, false).getIntValue());
          }
        }
        pages.push_back(std::move(grid));
      } else if (page == Page::library) {
        pages.push_back(std::make_unique<pedaleira::ui::LibraryPage>(*engine.client));
      } else if (page == Page::capture) {
        pages.push_back(std::make_unique<pedaleira::ui::CapturePage>(*engine.client));
      } else if (page == Page::import) {
        pages.push_back(std::make_unique<pedaleira::ui::ImportPage>(*engine.client));
      } else if (page == Page::settings) {
        pages.push_back(std::make_unique<pedaleira::ui::SettingsPage>(*engine.client, device.get()));
      } else
        pages.push_back(std::make_unique<pedaleira::ui::ComingSoon>(
            pageName(page), "This page comes in a later build."));
      shell.setPageComponent(page, pages.back().get());
    }
    shell.showPage(scenario.page);
    pedaleira::ui::FootswitchModesPage modes(*engine.client);
    pedaleira::ui::GigView gig(*engine.client);
    shell.setModesView(&modes);
    shell.setGigView(&gig);
    shell.setSize(pedaleira::ui::size::windowWidth, pedaleira::ui::size::windowHeight);
    if (const juce::String view(scenario.view); view == "modes") {
      modes.refresh();
      shell.showModesView(true);
    } else if (view == "gig") {
      shell.showGigView(true);
      // The clicks of the drive, as the mouse would: down, then up.
      for (const auto& step : juce::StringArray::fromTokens(scenario.drive, ";", "")) {
        if (!step.startsWith("gig:"))
          continue;
        const int index = juce::String("ABCDEFGH").indexOf(step.fromFirstOccurrenceOf(":", false, false));
        auto& pedal = gig.footswitchAt(index);
        pedal.onPress();
        pedal.onRelease();
      }
    }
    if (!juce::String(scenario.library).isEmpty())
      driveLibrary(*engine.client, static_cast<pedaleira::ui::LibraryPage&>(*pages[1]), scenario.library);
    if (!juce::String(scenario.importer).isEmpty())
      driveImport(*engine.client, static_cast<pedaleira::ui::ImportPage&>(*pages[3]), scenario.importer);
    if (!juce::String(scenario.capture).isEmpty()) {
      const juce::String problem = pedaleira::ui::testbed::driveCapture(
          *engine.client, *engine.processor, engine.data, static_cast<pedaleira::ui::CapturePage&>(*pages[2]), scenario.capture);
      if (problem.isNotEmpty()) {
        std::cerr << scenario.name << ": " << problem << "\n";
        ++failures;
      }
    }
    if (!juce::String(scenario.settings).isEmpty())
      driveSettings(*engine.client, static_cast<pedaleira::ui::SettingsPage&>(*pages[5]), scenario.settings);
    shell.header().refresh();
    const juce::Image image = shell.createComponentSnapshot(shell.getLocalBounds(), true, scale);
    const juce::File out = folder.getChildFile(juce::String(scenario.name) + ".png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    if (!stream.openedOk() || !juce::PNGImageFormat().writeImageToStream(image, stream)) {
      std::cerr << scenario.name << ": cannot write " << out.getFullPathName() << "\n";
      ++failures;
      continue;
    }
    std::cout << scenario.name << "  " << out.getFullPathName() << "\n";
    agentSockets().clear();
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juceInit;
  juce::StringArray arguments;
  for (int i = 1; i < argc; ++i)
    arguments.add(juce::String::fromUTF8(argv[i]));

  if (arguments.contains("--list")) {
    for (const auto& scenario : scenarios())
      std::cout << scenario.name << "  " << scenario.description << "\n";
    return 0;
  }
  const int at = arguments.indexOf("--capture");
  if (at < 0 || at + 1 >= arguments.size()) {
    std::cerr << "usage: PedaleiraUiTestbed --capture <folder> [--scale 1|2] [scenario...]\n"
                 "       PedaleiraUiTestbed --list\n";
    return 2;
  }
  const juce::File folder = juce::File::getCurrentWorkingDirectory().getChildFile(arguments[at + 1]);
  float scale = 1.0f;
  juce::StringArray only;
  for (int i = 0; i < arguments.size(); ++i) {
    if (i == at || i == at + 1)
      continue;
    if (arguments[i] == "--scale" && i + 1 < arguments.size()) {
      scale = arguments[++i].getFloatValue();
      continue;
    }
    only.add(arguments[i]);
  }
  return capture(folder, juce::jlimit(1.0f, 3.0f, scale), only);
}
