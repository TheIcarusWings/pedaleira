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
#include "NamLibrary.h"
#include "NamScenes.h"
#include "FakeDevice.h"
#include "widgets/Motion.h"
#include "Gallery.h"
#include "pages/BlockStyle.h"
#include "pages/CapturePage.h"
#include "pages/GridModel.h"
#include "pages/ComingSoon.h"
#include "pages/FootswitchModesPage.h"
#include "pages/GigView.h"
#include "pages/GridPage.h"
#include "pages/ImportPage.h"
#include "pages/NamEditor.h"
#include "pages/LibraryPage.h"
#include "pages/LivePage.h"
#include "pages/SettingsPage.h"
#include "pages/Tone3000Credit.h"
#include "shell/AppShell.h"

#include <cmath>
#include <deque>
#include <functional>
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
  // Or the block library: "picker:<row>:<slot>[:<category>[:<source>[:<search>]]]" (a dash
  // leaves a field out; source is the chip's number), "pickclick:<name>" (a click on the
  // row with that name), "t3k:tab:<n>", "t3k:tone:<n>", "t3k:capture:<n>" and "t3k:add"
  // on the TONE3000 page.
  // Or a block's editor: "editor:<block, as $slot:row:slot>"; "replace:<source>"
  // then presses what asks for another file (Swap capture). Several, with ";".
  // Or files dragged onto the grid: "dragover:<file>@<row>:<slot>" holds them over a
  // slot, "drop:<file>@<row>:<slot>" lets go (the file as $files/<name> or $library/..).
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
  // in progress), "unassign:<index>" (an expression assignment), "capturefolder:<path>" and
  // "forgetfolder:<path>" (the capture folders), "addfolder:<path>" and
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
  // The Live page, several with ";": "tab:tuner" (tempo, looper), "tap:<ms>" (a
  // tap at that time on the page's clock; the tuner tab listens to the tone
  // played first) and "bpm:<value>" (typed into the tempo).
  const char* live = "";
  // The capture wizard, steps separated by ";" (CaptureScenes.h): the engine is
  // brought into a state with commands, a simulated device and a stand-in trainer.
  const char* capture = "";
  // Keys pressed on the grid page, several with ";": "left", "right", "up",
  // "down", "escape", and "expand" or "collapse" (the settings panel's chevron).
  const char* keys = "";
  // The window, when it is not the default size (0 for the default).
  int width = 0;
  int height = 0;
  // The captures to set up first, steps with ";" (NamScenes.h): files in folders,
  // loads, user folders, captures downloaded from TONE3000.
  const char* nam = "";
  // The TONE3000 feed of the block library: "none", "out" (signed out), "in"
  // (signed in), "down" (TONE3000 can not be reached) or "first" (signed out, its
  // splash not seen yet). Every feed but "first" has had its splash seen.
  const char* tone3000 = "none";
  // The control of an amp's faceplate the mouse rests on (a parameter id, such as
  // "gain"), so the picture shows its value bubble.
  const char* hover = "";
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
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Amps/Harbor Crunch.nam"],"row":0,"slot":2}},
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Cabs/4x12 Slate.wav"],"row":0,"slot":3}},
    {"command":"chain.addBlock","args":{"kind":"delay.tape","row":0,"slot":4}},
    {"command":"chain.addBlock","args":{"kind":"reverb.plate","row":0,"slot":5}},
    {"command":"state.waitUntilLoaded","args":{}},
    {"command":"scene.add"},{"command":"scene.add"},{"command":"scene.add"},
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
    {"command":"chain.addToneFromFiles","args":{"paths":["$library/Amps/Lantern Clean.nam"],"row":1,"slot":2}},
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
    {"command":"assets.tag","args":{"file":"$library/Amps/Harbor Crunch.nam","add":["crunch","brit"]}},
    {"command":"assets.tag","args":{"file":"$library/Cabs/4x12 Slate.wav","add":["4x12","slate"]}},
    {"command":"guitar.create","args":{"name":"Birch","pickupType":"single-coil","positions":["bridge","middle","neck"]}},
    {"command":"guitar.create","args":{"name":"Walnut","pickupType":"humbucker","positions":["bridge","neck"]}},
    {"command":"guitar.create","args":{"name":"Tele","pickupType":"single-coil","positions":["bridge","middle","neck"]}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Birch","position":"bridge","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Birch","position":"middle","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Birch","position":"neck","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Walnut","position":"bridge","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Walnut","position":"neck","take":"soft","path":"$files/cab-ir-test.wav"}},
    {"command":"guitar.importClip","args":{"guitar":"$guitar:Tele","position":"bridge","take":"soft","path":"$files/cab-ir-test.wav"}}])";

// Three mappings on top of the defaults, on channel 2.
const char* kMidiRig = R"([
    {"command":"midi.setCc","args":{"targetId":"gateThreshold","cc":20}},
    {"command":"midi.setCc","args":{"targetId":"block1Power","cc":21}},
    {"command":"midi.setCc","args":{"targetId":"presetNext","cc":22}},
    {"command":"midi.setChannelFilter","args":{"channel":2}}])";

// The Live boards' rig: Glass Cleans of Friday Covers in Scene mode, the
// tuner listening.
const char* kLiveRig = joined({kStageRig, R"([{"command":"footswitch.setMode","args":{"mode":"scene"}},
    {"command":"tuner.setEnabled","args":{"enabled":true}}])"});

// The Tempo board's rig: the stage rig's tape echo follows the tempo in dotted
// eighths (kEditorRig), and a tremolo is added to follow it in eighths.
const char* kLiveTempoRig = joined({kStageRig, R"([{"command":"footswitch.setMode","args":{"mode":"scene"}},
    {"command":"chain.addBlock","args":{"kind":"modulation.tremolo","row":0,"slot":6}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:6","param":"sync","value":1}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:6","param":"division","value":9}}])"});

// A row of one block of every family, and a second row with more: the rig the
// settings panel's scenarios select from.
const char* kFamiliesRig = R"([{"command":"grid.convert","args":{"to":"grid"}},
    {"command":"chain.addBlock","args":{"kind":"drive.overdrive","row":0,"slot":0}},
    {"command":"chain.addBlock","args":{"kind":"amp.highgain","row":0,"slot":1}},
    {"command":"chain.addBlock","args":{"kind":"dynamics.compressor","row":0,"slot":2}},
    {"command":"chain.addBlock","args":{"kind":"eq.parametric8","row":0,"slot":3}},
    {"command":"chain.addBlock","args":{"kind":"eq.graphic10","row":0,"slot":4}},
    {"command":"chain.addBlock","args":{"kind":"delay.tape","row":0,"slot":5}},
    {"command":"chain.addBlock","args":{"kind":"reverb.plate","row":0,"slot":6}},
    {"command":"chain.addBlock","args":{"kind":"modulation.chorus","row":0,"slot":7}},
    {"command":"row.setInput","args":{"row":1,"source":"device","channel":2}},
    {"command":"row.setOutput","args":{"row":1,"target":"device","pair":2}},
    {"command":"chain.addBlock","args":{"kind":"pitch.shifter","row":1,"slot":0}},
    {"command":"chain.addBlock","args":{"kind":"filter.wah","row":1,"slot":1}},
    {"command":"chain.addBlock","args":{"kind":"utility.volume","row":1,"slot":2}},
    {"command":"chain.addBlock","args":{"kind":"modulation.tremolo","row":1,"slot":3}},
    {"command":"chain.addBlock","args":{"kind":"eq.parametric3","row":1,"slot":4}},
    {"command":"chain.addBlock","args":{"kind":"delay.pingpong","row":1,"slot":5}},
    {"command":"chain.addBlock","args":{"kind":"dynamics.gate","row":1,"slot":6}},
    {"command":"chain.addBlock","args":{"kind":"delay.multitap","row":1,"slot":7}},
    {"command":"state.waitUntilLoaded","args":{}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"drivePercent","value":70}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"tonePercent","value":62}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:1","param":"gain","value":72}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:1","param":"treble","value":64}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:2","param":"thresholdDb","value":-28}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:2","param":"ratio","value":6}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:2","param":"kneeDb","value":8}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b1Type","value":2}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b1Freq","value":80}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b3Freq","value":400}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b3Gain","value":-3.5}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b4Freq","value":1200}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b4Gain","value":4.5}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b4Q","value":1.6}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b6Freq","value":3200}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b6Gain","value":3}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b8Type","value":1}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b8Freq","value":9000}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:3","param":"b8Gain","value":-4}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"band63","value":4}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"band250","value":-3}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"band1k","value":5.5}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"band4k","value":-6}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:4","param":"band16k","value":2}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:5","param":"feedback","value":55}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:6","param":"decayS","value":3.2}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:6","param":"preDelayMs","value":40}},
    {"command":"block.setParam","args":{"blockId":"$slot:0:6","param":"damping","value":55}},
    {"command":"block.setParam","args":{"blockId":"$slot:1:0","param":"semitones","value":7}},
    {"command":"block.setParam","args":{"blockId":"$slot:1:1","param":"positionPct","value":68}},
    {"command":"block.setParam","args":{"blockId":"$slot:1:5","param":"feedback","value":50}},
    {"command":"block.setParam","args":{"blockId":"$slot:1:6","param":"thresholdDb","value":-45}}])";

// The captures of the capture scenarios (NamScenes.h): three loaded from a folder that is
// not a library folder, two in a folder the player added (one an older LSTM), and the
// captures downloaded on the TONE3000 page, kept in the library's TONE3000 folder (a pack's
// captures and one old fuzz).
const char* kCaptureFiles =
    "file:Elsewhere:Fav Northfield:a2-amp-test:Northfield:N59 Lead;file:Elsewhere:Quill Clean:a2-amp-cab-test:Quill:Q1;"
    "file:Elsewhere:Cobalt Lead:a2-am-test-2:Northfield:Grand Lead;"
    "load:Elsewhere/Fav Northfield.nam;pause;load:Elsewhere/Quill Clean.nam;pause;load:Elsewhere/Cobalt Lead.nam;"
    "file:Folder:Old Granite:old;file:Folder:Studio Preamp:a2-amp-test:Aldermoor:A73;root:Folder;"
    "download:Harbor Crunch pack:Crunch gain 5:a2-amp-test:Northfield:N800;"
    "download:Coastline Clean:Bright vol 6:a2-amp-cab-test:Seabright:Coast Reverb;"
    "download:Jade overdrive:Edge:a2-am-test-2:Kestrel:KS8;download:Old Fuzz:LSTM:old";

// Steps of a capture scenario with one more.
const char* joinedSteps(const char* steps, const char* more) {
  static std::deque<std::string> kept;
  kept.push_back(std::string(steps) + ";" + more);
  return kept.back().c_str();
}

// A scenario of the settings panel: a block of the families rig selected,
// keys pressed on the grid, and a window size.
Scenario settingsScenario(const char* name, const char* description, const char* script, const char* select,
                          const char* keys = "", bool audio = false, int width = 0, int height = 0) {
  Scenario scenario{name, description, Page::grid, script};
  scenario.select = select;
  scenario.keys = keys;
  scenario.audio = audio;
  scenario.width = width;
  scenario.height = height;
  return scenario;
}

// One amp in the first slot of a grid, its controls set as the design canvas
// shows them (percent: the canvas reads them from 0 to 10). `file` is a
// description given to an amp.custom block with block.loadDescription (see
// writeAmpVariants), or null for an amp of its own kind.
const char* ampRig(const char* kind, const char* file, std::initializer_list<std::pair<const char*, int>> values) {
  static std::deque<std::string> kept;
  std::string script = std::string(R"([{"command":"grid.convert","args":{"to":"grid"}},)") +
                       R"({"command":"chain.addBlock","args":{"kind":")" + kind + R"(","row":0,"slot":0}},)" +
                       R"({"command":"state.waitUntilLoaded","args":{}})";
  if (file != nullptr)
    script += std::string(R"(,{"command":"block.loadDescription","args":{"blockId":"$slot:0:0","file":")") + file + R"("}})";
  for (const auto& [param, value] : values)
    script += std::string(R"(,{"command":"block.setParam","args":{"blockId":"$slot:0:0","param":")") + param + R"(","value":)" +
              std::to_string(value) + "}}";
  script += "]";
  kept.push_back(script);
  return kept.back().c_str();
}

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
           {"command":"scene.add"},{"command":"scene.add"},{"command":"scene.add"},
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
      {.name = "picker-captures", .description = "The block library, NAM captures: loaded files by name, a folder, the downloads from TONE3000",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:2", .nam = kCaptureFiles},
      {.name = "picker-recent", .description = "The block library, Recent: the files loaded last, newest first",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:5", .nam = kCaptureFiles},
      {.name = "picker-find-name", .description = "The block library searched for \"northfield\": by file name, make and model",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:0:northfield", .nam = kCaptureFiles},
      {.name = "picker-unsupported", .description = "The block library: a click on a capture Pedaleira can not load says why",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:2;pickclick:Old Granite", .nam = kCaptureFiles},
      {.name = "picker-gone", .description = "The block library, Recent: a file whose original is gone is offered from the stash",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:5", .nam = joinedSteps(kCaptureFiles, "gone:Elsewhere/Fav Northfield.nam")},
      {.name = "picker-tone3000-out", .description = "The block library, TONE3000, signed out: Trending and the tone's detail",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6;t3k:tone:0", .tone3000 = "out"},
      {.name = "picker-tone3000-account", .description = "The block library, TONE3000, signed out: Favorites needs an account",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6;t3k:tab:2", .tone3000 = "out"},
      {.name = "picker-tone3000-in", .description = "The block library, TONE3000, signed in: Favorites, a tone and its captures",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6;t3k:tab:2;t3k:tone:1;t3k:capture:1", .tone3000 = "in"},
      {.name = "picker-tone3000-down", .description = "The block library, TONE3000 can not be reached",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6", .tone3000 = "down"},
      {.name = "picker-tone3000-none", .description = "The block library, TONE3000 in a build without a feed",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6"},
      {.name = "picker-tone3000-splash", .description = "The block library, TONE3000 opened the first time: its splash",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6", .tone3000 = "first"},
      {.name = "picker-tone3000-narrow", .description = "The block library, TONE3000 at the narrowest window: creator and licence stay",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:6;t3k:tab:2;t3k:tone:0", .width = 1120, .height = 700,
       .tone3000 = "in"},
      {.name = "picker-tone3000-recent", .description = "The block library, Recent: a capture added from TONE3000 keeps its credit",
       .page = Page::grid, .script = R"([{"command":"grid.convert","args":{"to":"grid"}}])",
       .open = "picker:0:6:-:6;t3k:tab:2;t3k:tone:1;t3k:capture:1;t3k:add;picker:0:7:-:5", .nam = kCaptureFiles, .tone3000 = "in"},
      {.name = "picker-captures-narrow", .description = "The block library at the narrowest window: the downloads from TONE3000 and Browse TONE3000",
       .page = Page::grid, .script = "[]", .open = "picker:0:6:-:2", .width = 1120, .height = 700, .nam = kCaptureFiles},
      {"editor-effect", "A tape echo's editor: time with tempo sync, its other parameters, output", Page::grid,
       kEditorRig, false, "", "", "editor:$slot:0:4"},
      {"editor-geq", "The graphic equaliser's editor: band sliders over their curve", Page::grid,
       R"([{"command":"grid.convert","args":{"to":"grid"}},
           {"command":"chain.addBlock","args":{"kind":"eq.graphic10","row":0,"slot":0}},
           {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"band63","value":4}},
           {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"band250","value":-3}},
           {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"band1k","value":5.5}},
           {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"band4k","value":-6}},
           {"command":"block.setParam","args":{"blockId":"$slot:0:0","param":"band16k","value":2}}])",
       false, "", "", "editor:$slot:0:0"},
      {"editor-reverb", "A plate reverb's editor: no tempo, one parameters section and the output", Page::grid,
       kEditorRig, false, "", "", "editor:$slot:0:5"},
      {"editor-nam", "A NAM capture's editor: capture info, levels, block EQ and meters", Page::grid, kEditorRig,
       false, "", "", "editor:$slot:0:2", true},
      {"grid-drag-over", "A capture dragged over a block: the slot that would be replaced is outlined", Page::grid,
       kEditorRig, false, "", "", "dragover:$files/a2-amp-cab-test.nam@0:2"},
      {"grid-drag-empty", "A capture dragged over an empty slot: the slot that would take it is outlined", Page::grid,
       kEditorRig, false, "", "", "dragover:$files/a2-amp-cab-test.nam@0:6"},
      {"grid-dropped", "A capture dropped on a block: replaced in place, the row as it was", Page::grid, kEditorRig,
       false, "", "", "drop:$files/a2-amp-cab-test.nam@0:2"},
      {.name = "editor-nam-tone3000", .description = "A NAM capture from TONE3000: its image, title, gear, format, creator and licence",
       .page = Page::grid, .script = R"([{"command":"grid.convert","args":{"to":"grid"}}])", .open = "editor:$slot:0:0",
       .nam = "file:Downloads:Harbor Crunch pack - Crunch, gain 5;t3kload:Downloads/Harbor Crunch pack - Crunch, gain 5.nam",
       .tone3000 = "in"},
      {"editor-swap", "Swap capture: the library over the editor, on the NAM captures", Page::grid, kEditorRig, false,
       "", "", "editor:$slot:0:2;replace:2"},
      {"editor-ir", "An IR's editor: file, its folder's IRs, levels", Page::grid, kEditorRig, false, "", "",
       "editor:$slot:0:3"},
      {"grid-threeway", "Three amps in parallel: row 1 splits into row 2, row 2 into row 3", Page::grid, kFullRig},
      settingsScenario("settings-drive", "Settings panel: an overdrive selected, saturation curve and tone tilt", kFamiliesRig,
                       "$slot:0:0"),
      settingsScenario("settings-eq8", "Settings panel: the 8-band parametric EQ, its response and a section per band",
                       kFamiliesRig, "$slot:0:3"),
      settingsScenario("settings-geq", "Settings panel: the graphic EQ, its band sliders over the curve", kFamiliesRig,
                       "$slot:0:4"),
      settingsScenario("settings-none", "Settings panel: nothing selected (Escape), the quiet hint", kFamiliesRig,
                       "$slot:0:0", "escape"),
      settingsScenario("settings-min", "Settings panel at the minimum window size: a slim bar with a chevron",
                       kFamiliesRig, "$slot:0:0", "", false, 1120, 700),
      settingsScenario("settings-min-open", "Settings panel at the minimum window size, expanded over the grid",
                       kFamiliesRig, "$slot:0:0", "expand", false, 1120, 700),
      settingsScenario("settings-amp", "Settings panel: the amp, its knobs drawn as a face", kFamiliesRig, "$slot:0:1", "",
                       true),
      settingsScenario("settings-amp-lead100", "Faceplate: Lead 100, silver, nameplate left, wedge caps, slash strip",
                       ampRig("amp.lead100", nullptr, {{"gain", 75}, {"bass", 50}, {"middle", 65}, {"treble", 60}, {"presence", 55},
                                                       {"depth", 45}, {"master", 40}, {"bright", 1}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-redforty", "Faceplate: Red Forty, red, banner on top, round caps, dot strip",
                       ampRig("amp.redforty", nullptr, {{"gain", 70}, {"bass", 60}, {"middle", 50}, {"treble", 65}, {"presence", 50},
                                                        {"depth", 40}, {"master", 45}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-modernthree", "Faceplate: Modern Three, blue, round caps, wave strip",
                       ampRig("amp.modernthree", nullptr, {{"gain", 80}, {"bass", 55}, {"middle", 40}, {"treble", 65}, {"presence", 60},
                                                           {"depth", 50}, {"master", 35}, {"bright", 1}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-cellfour", "Faceplate: Cell Four, green, nameplate right, four boxes, cell strip",
                       ampRig("amp.cellfour", nullptr, {{"gain", 65}, {"bass", 60}, {"middle", 55}, {"treble", 55}, {"presence", 45},
                                                        {"depth", 55}, {"master", 40}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-heavyhot", "Faceplate: Heavy Hot, purple, wedge caps, a box of four knobs in two columns",
                       ampRig("amp.heavyhot", nullptr, {{"gain", 85}, {"depth", 75}, {"bass", 70}, {"middle", 45}, {"treble", 55},
                                                        {"presence", 65}, {"master", 35}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-highgain", "Faceplate: High Gain, three voicings on a segmented switch",
                       ampRig("amp.highgain", nullptr, {{"gain", 72}, {"treble", 64}, {"bright", 1}, {"voicing", 2}}), "$slot:0:0", "",
                       true),
      settingsScenario("settings-amp-plain", "Faceplate: an amp.custom block whose description gives no style, the plain default",
                       ampRig("amp.custom", "$tmp/plain-amp.json", {{"gain", 60}, {"bass", 55}, {"middle", 45}, {"treble", 65}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-chicken", "Faceplate: chicken head caps, nameplate at the bottom",
                       ampRig("amp.custom", "$tmp/chicken-amp.json", {{"gain", 60}, {"bass", 50}, {"middle", 65}, {"treble", 55}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-fader", "Faceplate: faders instead of knobs, steps strip, nameplate right",
                       ampRig("amp.custom", "$tmp/fader-amp.json", {{"gain", 30}, {"bass", 65}, {"middle", 50}, {"treble", 35}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-glassforty", "Faceplate: Glass Forty, clean, cyan, banner on top, ring strip",
                       ampRig("amp.glassforty", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-bellthirty", "Faceplate: Bell Thirty, clean, pink, chicken head caps, bell strip",
                       ampRig("amp.bellthirty", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-steady120", "Faceplate: Steady 120, clean, grey, faders, step strip",
                       ampRig("amp.steady120", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-velvetfifty", "Faceplate: Velvet Fifty, clean, green, banner on top, diamond strip",
                       ampRig("amp.velvetfifty", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-emberfive", "Faceplate: Ember Five, clean, orange red, chicken head caps, flame strip",
                       ampRig("amp.emberfive", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-amberfifty", "Faceplate: Amber Fifty, crunch, yellow, nameplate right, chevron strip",
                       ampRig("amp.amberfifty", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-coppereighteen", "Faceplate: Copper Eighteen, crunch, copper, cross strip",
                       ampRig("amp.coppereighteen", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-dustforty", "Faceplate: Dust Forty, crunch, sand, banner on top, tick strip",
                       ampRig("amp.dustforty", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-bronzetwenty", "Faceplate: Bronze Twenty, crunch, olive, nameplate right, arc strip",
                       ampRig("amp.bronzetwenty", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      settingsScenario("settings-amp-silkhundred", "Faceplate: Silk Hundred, crunch, lilac, ripple strip",
                       ampRig("amp.silkhundred", nullptr, {{"gain", 60}, {"bass", 50}, {"middle", 55}, {"treble", 60}, {"presence", 50}, {"master", 50}}),
                       "$slot:0:0", "", true),
      {.name = "settings-amp-hover", .description = "Faceplate: Lead 100 with the mouse on Gain, its value in a bubble",
       .page = Page::grid,
       .script = ampRig("amp.lead100", nullptr, {{"gain", 75}, {"bass", 50}, {"middle", 65}, {"treble", 60}, {"presence", 55},
                                                 {"depth", 45}, {"master", 40}}),
       .select = "$slot:0:0", .audio = true, .hover = "gain"},
      settingsScenario("settings-comp", "Settings panel: a compressor, transfer curve with live gain reduction",
                       kFamiliesRig, "$slot:0:2", "", true),
      settingsScenario("settings-delay", "Settings panel: a tape echo, the echoes on a time axis", kFamiliesRig, "$slot:0:5"),
      settingsScenario("settings-reverb", "Settings panel: a plate reverb, its decay", kFamiliesRig, "$slot:0:6"),
      settingsScenario("settings-mod", "Settings panel: a chorus, its wave and rate", kFamiliesRig, "$slot:0:7"),
      settingsScenario("settings-pitch", "Settings panel: a pitch shifter, the interval on a keyboard", kFamiliesRig,
                       "$slot:1:0"),
      settingsScenario("settings-filter", "Settings panel: a wah, its sweep", kFamiliesRig, "$slot:1:1"),
      settingsScenario("settings-utility", "Settings panel: a volume pedal, the level glyph", kFamiliesRig, "$slot:1:2", "",
                       true),
      settingsScenario("settings-pingpong", "Settings panel: a ping-pong delay, echoes on two sides", kFamiliesRig,
                       "$slot:1:5"),
      settingsScenario("settings-gate", "Settings panel: a noise gate, its transfer curve", kFamiliesRig, "$slot:1:6"),
      settingsScenario("settings-keys", "Settings panel after the arrow keys: right, right, down", kFamiliesRig,
                       "$slot:0:0", "right;right;down"),
      settingsScenario("settings-nam", "Settings panel: a NAM capture, the capture editor scrolls inside", kEditorRig,
                       "$slot:0:2", "", true),
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
      {.name = "settings-captures", .description = "Settings, capture folders: a folder added",
       .page = Page::settings, .script = "[]", .settings = "part:captures", .nam = kCaptureFiles},
      {.name = "settings-captures-empty", .description = "Settings, capture folders: nothing added",
       .page = Page::settings, .script = "[]", .settings = "part:captures"},
      {.name = "settings-midi", .description = "Settings, MIDI: the defaults and three mappings, read only",
       .page = Page::settings, .script = kMidiRig, .settings = "part:midi"},
      {.name = "settings-remote", .description = "Settings, remote: a later build", .page = Page::settings, .script = "[]",
       .settings = "part:remote"},
      {.name = "settings-about", .description = "Settings, about: version, licences and credits", .page = Page::settings,
       .script = "[]", .settings = "part:about"},
      {.name = "live-tuner", .description = "Live, Tuner: an E2 eight cents flat, the sixth string marked, settings beside it",
       .page = Page::live, .script = kLiveRig, .audio = true, .toneHz = 82.03, .live = "tab:tuner"},
      {.name = "live-tuner-intune", .description = "Live, Tuner: a D3 in tune, the band and the needle green",
       .page = Page::live, .script = kLiveRig, .audio = true, .toneHz = 146.83, .live = "tab:tuner"},
      {.name = "live-tuner-idle", .description = "Live, Tuner: nothing played, a fresh rig",
       .page = Page::live, .script = R"([{"command":"tuner.setEnabled","args":{"enabled":true}}])", .live = "tab:tuner"},
      {.name = "live-tempo", .description = "Live, Tempo: four taps, 120 typed in, two synced blocks, the metronome and clock later",
       .page = Page::live, .script = kLiveTempoRig, .live = "tab:tempo;tap:10000;tap:10512;tap:11008;tap:11512;tap:12012;bpm:120"},
      {.name = "live-tempo-fresh", .description = "Live, Tempo: a fresh rig, no taps and no synced block",
       .page = Page::live, .script = "[]", .live = "tab:tempo"},
      {.name = "live-looper", .description = "Live, Looper: the tab says it comes in a later build",
       .page = Page::live, .script = "[]", .live = "tab:looper"},
      // The capture wizard. kReampLevels brings a re-amp to the levels step with the tone in range;
      // kReampRecord to the record step with a stand-in input file.
      {.name = "capture-source", .description = "Capture, step 1: what are you capturing, nothing typed yet",
       .page = Page::capture, .script = "[]"},
      {.name = "capture-source-filled", .description = "Capture, step 1: Amp Head chosen, a name typed",
       .page = Page::capture, .script = "[]", .capture = "name:Harbor Crunch;source:1"},
      {.name = "capture-source-pedal", .description = "Capture, step 1: a pedal chosen",
       .page = Page::capture, .script = "[]", .capture = "name:Jade Drive;source:0"},
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
// The shipped high-gain description with its panel style changed, for the
// faceplate scenarios that need an amp the design canvas does not show: one
// with no style at all (what a user's own description is) and two with the
// caps and strips that none of the six high gain amps use. Written as text,
// the same way the engine's tests edit a description.
void writeAmpVariants(const juce::File& folder) {
  const juce::File source = juce::File(PEDALEIRA_SOURCE_DIR).getChildFile("resources").getChildFile("amps").getChildFile("high-gain.json");
  const std::string original = source.loadFileAsString().toStdString();
  const size_t from = original.find(",\n    \"faceplate\": {");
  // The faceplate object closes on a line of its own, four spaces in.
  const size_t to = from == std::string::npos ? from : original.find("\n    }", from);
  const size_t family = original.find("    \"family\": ");
  if (from == std::string::npos || to == std::string::npos || family == std::string::npos)
    return;
  const auto make = [&](const char* file, const char* name, const char* style) {
    std::string text = original;
    text.erase(from, to + 6 - from);
    // The new faceplate goes where the old one was, before anything above it changes length.
    if (style != nullptr)
      text.insert(from, std::string(",\n    \"faceplate\": ") + style);
    text.erase(family, text.find('\n', family) + 1 - family);
    const std::string key = "\"name\": \"High Gain\"";
    const size_t at = text.find(key);
    if (at != std::string::npos)
      text.replace(at, key.size(), std::string("\"name\": \"") + name + "\"");
    folder.getChildFile(file).replaceWithText(juce::String(text));
  };
  make("plain-amp.json", "My Own Amp", nullptr);
  make("chicken-amp.json", "Chicken Test",
       R"({"accent": "#F5C518", "nameplate": "bottom", "tag": "Brit Crunch", "character": "Chicken head caps on a custom amp",
           "strip": "bells", "knob_cap": "chicken_head",
           "groups": [{"title": "Input", "controls": ["gain", "bright"]},
                      {"title": "Tone", "controls": ["bass", "middle", "treble", "presence"]},
                      {"title": "Master", "controls": ["depth", "master", "voicing"]}]})");
  make("fader-amp.json", "Fader Test",
       R"({"accent": "#9AA4AF", "nameplate": "right", "tag": "Solid State", "character": "Faders on a custom amp",
           "strip": "steps", "knob_cap": "fader",
           "groups": [{"title": "Channel", "controls": ["gain", "bright"]},
                      {"title": "Equaliser", "controls": ["bass", "middle", "treble", "presence", "depth", "voicing"]},
                      {"title": "Master", "controls": ["master"]}]})");
}

struct ScratchEngine {
  explicit ScratchEngine(bool withPlugins = false) {
    data = juce::File::getSpecialLocation(juce::File::tempDirectory)
               .getChildFile("PedaleiraUiTestbed-" + juce::Uuid().toDashedString());
    data.createDirectory();
    writeAmpVariants(data);
    const juce::File library = data.getChildFile("Library");
    library.createDirectory();
    // A few captures and IRs in the library, for the block library to list.
    const juce::File files = juce::File(PEDALEIRA_SOURCE_DIR).getChildFile("test").getChildFile("files");
    library.getChildFile("Amps").createDirectory();
    library.getChildFile("Cabs").createDirectory();
    files.getChildFile("a2-amp-test.nam").copyFileTo(library.getChildFile("Amps").getChildFile("Harbor Crunch.nam"));
    files.getChildFile("a2-amp-cab-test.nam").copyFileTo(library.getChildFile("Amps").getChildFile("Lantern Clean.nam"));
    files.getChildFile("cab-ir-test.wav").copyFileTo(library.getChildFile("Cabs").getChildFile("4x12 Slate.wav"));
    files.getChildFile("cab-ir-test-2.wav").copyFileTo(library.getChildFile("Cabs").getChildFile("2x12 Blue.wav"));
    libraryFolder() = library;
    scratchFolder() = data;
    // The capture names and the recent list live in the app data folder: a scratch one,
    // so a run never reads or writes the player's own.
    data.getChildFile("AppData").createDirectory();
    previousAppData = LibraryPaths::setAppDataDirForTesting(data.getChildFile("AppData"));
    NamLibrary::resetForTesting();
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
    LibraryPaths::setAppDataDirForTesting(previousAppData);
    NamLibrary::resetForTesting();
    data.deleteRecursively();
  }

  juce::File previousAppData;
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

// The mouse arriving on a component, as the pointer would.
void moveOver(juce::Component& target) {
  const juce::Point<float> at = target.getLocalBounds().getCentre().toFloat();
  const juce::Time now = juce::Time::getCurrentTime();
  const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, &target, &target, now, at, now, 0, false);
  target.mouseEnter(event);
}

// The mouse leaving it.
void moveOut(juce::Component& target) {
  const juce::Point<float> at = target.getLocalBounds().getCentre().toFloat();
  const juce::Time now = juce::Time::getCurrentTime();
  const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, &target, &target, now, at, now, 0, false);
  target.mouseExit(event);
}

// A press on the middle of a component, a drag of `dy` pixels (up is negative)
// and the release; `whileDown` runs between the drag and the release.
void dragBy(juce::Component& target, int dy, const std::function<void()>& whileDown) {
  const juce::Point<float> start = target.getLocalBounds().getCentre().toFloat();
  const juce::Point<float> end = start.translated(0.0f, static_cast<float>(dy));
  const juce::Time now = juce::Time::getCurrentTime();
  const auto event = [&](juce::Point<float> at, bool dragged) {
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &target, &target, now, start, now, 1, dragged);
  };
  target.mouseDown(event(start, false));
  target.mouseDrag(event(end, true));
  whileDown();
  target.mouseUp(event(end, true));
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
  const juce::StringArray names{"audio", "midi", "expression", "plugins", "captures", "agents", "remote", "about"};
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
    } else if (key == "capturefolder") {
      static_cast<pedaleira::ui::SettingsCaptures&>(page.section(Part::captures)).addFolderPath(resolve(client, value).toString());
    } else if (key == "forgetfolder") {
      static_cast<pedaleira::ui::SettingsCaptures&>(page.section(Part::captures)).removeFolderPath(resolve(client, value).toString());
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

// Plays the Live scenario's choices.
void driveLive(pedaleira::ui::LivePage& page, const juce::String& drive) {
  using Tab = pedaleira::ui::LivePage::Tab;
  for (const auto& step : juce::StringArray::fromTokens(drive, ";", "")) {
    const juce::String key = step.upToFirstOccurrenceOf(":", false, false);
    const juce::String value = step.fromFirstOccurrenceOf(":", false, false);
    if (key == "tab")
      page.showTab(value == "tempo" ? Tab::tempo : value == "looper" ? Tab::looper : Tab::tuner);
    else if (key == "tap")
      page.tempoView().tap(value.getDoubleValue());
    else if (key == "bpm")
      page.tempoView().setBpm(value.getDoubleValue());
  }
}

// Checks of the grid page's logic that need no picture: the keyboard
// selection, the panel following the rig, the collapse rule and the curves.
// A stash name: sixteen hex digits, a dash and a size.
bool looksLikeHash(const juce::String& name) {
  const juce::String head = name.upToFirstOccurrenceOf("-", false, false);
  return head.length() >= 12 && head.containsOnly("0123456789abcdef");
}

// Finding captures by name, the recent list, the architecture message, the
// TONE3000 page over a canned feed, and the capture folders of Settings.
void captureFindingChecks(const std::function<void(bool, const char*)>& check) {
  using namespace pedaleira::ui;
  ScratchEngine engine;
  const juce::String problem = testbed::prepareNam(*engine.client, engine.data, testFiles(), kCaptureFiles);
  check(problem.isEmpty(), "the capture files are set up");
  if (problem.isNotEmpty())
    std::cerr << problem << "\n";
  // The grid page does this when it opens the library.
  engine.client->run("grid.convert", object({{"to", "grid"}}));

  BlockPicker picker(*engine.client);
  picker.setSize(1440, 900);
  juce::String addedBlock;
  int added = 0;
  picker.onAdded = [&](const juce::String& id) {
    addedBlock = id;
    ++added;
  };
  picker.openFor(0, 6);

  // A loaded file is listed by the name it was loaded under, never by its hash.
  picker.setFilter({}, BlockPicker::captures, {});
  const juce::StringArray all = picker.shownNames();
  bool hashes = false;
  for (const auto& name : all)
    hashes = hashes || looksLikeHash(name);
  check(!hashes, "no capture is listed by its hash name");
  check(all.contains("Fav Northfield") && all.contains("Quill Clean") && all.contains("Cobalt Lead"),
        "the captures loaded from a file are listed by their file names");
  check(all.contains("Studio Preamp") && all.contains("Old Granite"), "a folder added in Settings is listed");
  check(all.contains("Harbor Crunch pack - Crunch gain 5"), "a capture downloaded from TONE3000 is listed");
  check(engine.data.getChildFile("Library").getChildFile("TONE3000").getChildFile("Harbor Crunch pack - Crunch gain 5.nam").existsAsFile(),
        "and kept in the library's TONE3000 folder");
  picker.setFilter({}, BlockPicker::captures, "harbor crunch pack");
  check(picker.creditOf("Harbor Crunch pack - Crunch gain 5").creator.getWidth() >= 40, "listed with its creator and licence");
  picker.setFilter({}, BlockPicker::captures, {});
  check(all.contains("Harbor Crunch") && all.contains("Lantern Clean"), "the library's captures are still listed");
  int count = 0;
  for (const auto& name : all)
    count += name == "Fav Northfield" ? 1 : 0;
  check(count == 1, "the stash copy and the asset store copy of a file are one row");

  // Search: the file name, the make and model inside the file, a folder's file.
  picker.setFilter({}, BlockPicker::all, "fav north");
  check(picker.shownNames().contains("Fav Northfield"), "a search finds a loaded capture by its file name");
  picker.setFilter({}, BlockPicker::all, "grand lead");
  check(picker.shownNames().contains("Cobalt Lead"), "a search finds a capture by the model inside the file");
  picker.setFilter({}, BlockPicker::all, "n800");
  check(picker.shownNames().contains("Harbor Crunch pack - Crunch gain 5"), "and one downloaded from TONE3000");
  picker.setFilter({}, BlockPicker::all, "nothing like this");
  check(picker.shownNames().isEmpty(), "a search that matches nothing lists nothing");

  // Recent: the last loaded first, only what was loaded.
  picker.setFilter({}, BlockPicker::recent, {});
  const juce::StringArray recent = picker.shownNames();
  check(recent.size() == 3 && recent[0] == "Cobalt Lead" && recent[1] == "Quill Clean" && recent[2] == "Fav Northfield",
        "Recent lists the loaded captures, the newest first");
  picker.setFilter({}, BlockPicker::recent, "quill");
  check(picker.shownNames().size() == 1, "and a search narrows it");

  // A capture whose file is damaged (an LSTM short of weights) says why and is not added.
  picker.setFilter({}, BlockPicker::captures, {});
  check(picker.clickName("Old Granite"), "an unsupported capture can be clicked");
  check(added == 0, "and is not added");
  check(picker.noticeText().contains("Old Granite") && picker.noticeText().contains("LSTM") && picker.noticeText().contains("damaged"),
        "the message names the capture and its architecture");
  const EngineReply unsupported = engine.client->run(
      "chain.addToneFromFiles", object({{"paths", juce::Array<juce::var>{engine.data.getChildFile("Folder/Old Granite.nam").getFullPathName()}}}));
  check(!unsupported.ok && unsupported.errorMessage.contains("LSTM"), "the engine refuses it with the same reason");
  check(picker.clickName("Studio Preamp") && added == 1 && addedBlock.isNotEmpty(), "a supported capture is added to the slot");

  // Clearing Recent leaves the names searchable.
  engine.client->run("nam.clearRecent");
  picker.openFor(0, 7);
  picker.setFilter({}, BlockPicker::recent, {});
  check(picker.shownNames().isEmpty(), "Recent is empty after it is cleared");
  picker.setFilter({}, BlockPicker::all, "fav northfield");
  check(picker.shownNames().contains("Fav Northfield"), "but the file is still found by name");

  // The TONE3000 page, signed out: lists that do not need an account work.
  {
    testbed::FakeTone3000Feed::Setup setup;
    setup.capture = testFiles().getChildFile("a2-amp-test.nam");
    setup.engine = engine.client.get();
    testbed::FakeTone3000Feed feed(setup);
    picker.setTone3000Feed(&feed);
    picker.setFilter({}, BlockPicker::tone3000, {});
    auto& panel = picker.tone3000Panel();
    check(panel.isVisible() && panel.toneCount() == 8, "Trending lists tones without an account");
    panel.selectTab(Tone3000List::latest);
    check(panel.toneCount() == 6, "so does Latest");
    panel.selectTab(Tone3000List::favorites);
    check(panel.toneCount() == 0, "Favorites needs an account and lists nothing");
    panel.selectTab(Tone3000List::trending);
    panel.selectTone(0);
    check(panel.selectedTone() == 0 && !panel.canAdd(), "a tone can be looked at but not added signed out");
    panel.addSelected();
    check(feed.downloads == 0, "and nothing is downloaded");
    picker.setFilter({}, BlockPicker::all, {});
    check(!panel.isVisible(), "leaving the TONE3000 chip hides the page");
    picker.setTone3000Feed(nullptr);
  }
  // Signed in: a capture is chosen, downloaded only when added, and lands in the slot.
  {
    testbed::FakeTone3000Feed::Setup setup;
    setup.signedIn = true;
    setup.capture = testFiles().getChildFile("a2-amp-test.nam");
    setup.engine = engine.client.get();
    testbed::FakeTone3000Feed feed(setup);
    picker.setTone3000Feed(&feed);
    picker.openFor(0, 7);
    picker.setFilter({}, BlockPicker::tone3000, {});
    auto& panel = picker.tone3000Panel();
    panel.selectTab(Tone3000List::favorites);
    check(panel.toneCount() == 3, "signed in, Favorites lists the player's tones");
    panel.selectTone(1);
    check(panel.selectedCapture() == 0 && panel.canAdd(), "choosing a tone selects its first capture");
    check(feed.downloads == 0, "nothing is downloaded before Add");
    panel.selectCapture(2);
    const int before = added;
    panel.addSelected();
    check(feed.downloads == 1 && added == before + 1, "Add downloads the capture and puts it in the slot");
    check(panel.problem().isEmpty() && panel.canAdd(), "and the page is ready again");
    // The download is kept in the library, and the same capture is not fetched twice.
    check(engine.client->read("nam.recent")["recent"][0]["path"].toString().contains("TONE3000"),
          "the capture added from TONE3000 is loaded from the library's TONE3000 folder");
    panel.addSelected();
    check(feed.downloads == 1, "adding the same capture again downloads nothing");
    check(added == before + 2, "and still puts it in the slot");
    picker.setTone3000Feed(nullptr);
  }
  {
    testbed::FakeTone3000Feed::Setup setup;
    setup.unreachable = true;
    testbed::FakeTone3000Feed feed(setup);
    picker.setTone3000Feed(&feed);
    picker.setFilter({}, BlockPicker::tone3000, {});
    check(picker.tone3000Panel().toneCount() == 0, "an unreachable TONE3000 lists nothing");
    picker.setTone3000Feed(nullptr);
  }

  // TONE3000's terms. The splash comes first, once: nothing is listed behind it.
  {
    testbed::FakeTone3000Feed::Setup setup;
    setup.introSeen = false;
    testbed::FakeTone3000Feed feed(setup);
    picker.setTone3000Feed(&feed);
    picker.openFor(0, 7);
    auto& panel = picker.tone3000Panel();
    panel.selectTab(Tone3000List::trending);
    picker.setFilter({}, BlockPicker::tone3000, {});
    check(panel.splashShown(), "the first visit to TONE3000 shows its splash");
    check(feed.lists == 0 && panel.toneCount() == 0, "and loads nothing behind it");
    int before = tone3000::wordmarkDraws();
    juce::ignoreUnused(panel.createComponentSnapshot(panel.getLocalBounds()));
    check(tone3000::wordmarkDraws() > before, "the splash draws TONE3000's wordmark");
    panel.continueSplash();
    check(!panel.splashShown() && feed.introSeen(), "Continue closes it and remembers it was seen");
    check(panel.toneCount() == 8, "and the list loads");
    before = tone3000::wordmarkDraws();
    juce::ignoreUnused(panel.createComponentSnapshot(panel.getLocalBounds()));
    check(tone3000::wordmarkDraws() > before, "the page says Powered by with the wordmark");
    picker.setFilter({}, BlockPicker::all, {});
    picker.setFilter({}, BlockPicker::tone3000, {});
    check(!panel.splashShown(), "the splash is shown once");
    panel.beginSignIn();
    check(panel.splashShown(), "a sign-in starts with the splash");
    panel.continueSplash();
    check(!panel.splashShown() && panel.problem().contains("not available yet"), "then says signing in is not here yet");
    picker.setTone3000Feed(nullptr);
  }
  // At the narrowest window the creator and the licence of every tone keep their room.
  {
    testbed::FakeTone3000Feed::Setup setup;
    setup.signedIn = true;
    setup.capture = testFiles().getChildFile("a2-amp-test.nam");
    setup.engine = engine.client.get();
    testbed::FakeTone3000Feed feed(setup);
    picker.setTone3000Feed(&feed);
    picker.setSize(1120 - size::railWidth, 700 - size::headerHeight);
    picker.openFor(0, 7);
    const auto chip = picker.sourceChipBounds(BlockPicker::tone3000);
    check(chip.getRight() <= picker.getWidth() - 24 - 20, "Browse TONE3000 fits in the chips at the narrowest window");
    picker.setFilter({}, BlockPicker::tone3000, {});
    auto& panel = picker.tone3000Panel();
    panel.selectTab(Tone3000List::trending);
    bool roomy = panel.toneCount() == 8;
    int shown = 0;
    for (int i = 0; i < panel.toneCount(); ++i) {
      const auto line = panel.rowCredit(i);
      if (line.creator.isEmpty())
        continue;
      ++shown;
      roomy = roomy && line.creator.getWidth() >= 40 && line.licence.getWidth() >= 40 &&
              line.licence.getRight() <= panel.tableBounds().getRight() && !line.avatar.isEmpty();
    }
    check(roomy && shown >= 3, "each listed tone shows its creator, avatar and licence at the narrowest window");
    panel.selectTone(6);
    const auto detail = panel.detailCredit();
    check(detail.creator.getWidth() >= 40 && !detail.avatar.isEmpty() && panel.detailLicence().getHeight() > 0 &&
              panel.detailLicence().getBottom() < panel.getHeight() - 100,
          "the chosen tone shows its creator and licence above its buttons");
    // A capture added from TONE3000 keeps its credit in the rig and in Recent.
    panel.selectTone(1);
    panel.selectCapture(1);
    panel.addSelected();
    const juce::var recentNow = engine.client->read("nam.recent");
    check(recentNow["recent"][0]["tone3000"]["user"]["username"].toString() == "sample-user-b" &&
              recentNow["recent"][0]["tone3000"]["license"].toString() == "cc0",
          "the added capture keeps its creator and licence");
    picker.openFor(0, 6);
    picker.setFilter({}, BlockPicker::recent, {});
    const auto credit = picker.creditOf("Coastline Clean - Crunch, gain 5");
    check(credit.creator.getWidth() >= 40 && credit.licence.getWidth() >= 40 &&
              credit.licence.getRight() <= picker.listBounds().getRight(),
          "Recent shows its creator and licence at the narrowest window");
    picker.setTone3000Feed(nullptr);
    picker.setSize(1440, 900);
  }

  // The header: the mode switch never touches the tempo box, at the narrowest
  // window (1120 px) and at the default one (1440 px).
  {
    pedaleira::ui::AppShell headerShell(*engine.client);
    for (const int shellWidth : {1120, size::windowWidth}) {
      headerShell.setSize(shellWidth, shellWidth == 1120 ? 700 : size::windowHeight);
      headerShell.header().refresh();
      const auto modeBox = headerShell.header().modeBounds();
      const auto tempoBox = headerShell.header().tempoBounds();
      check(!modeBox.isEmpty() && modeBox.getWidth() >= 200, "the mode switch keeps a usable width");
      check(modeBox.getRight() + 12 <= tempoBox.getX(), "the mode switch keeps a gap before the BPM box");
    }
  }

  // Settings: the folders to scan, added and removed, kept by the engine.
  SettingsCaptures folders(*engine.client);
  folders.refresh();
  check(folders.folderCount() == 1, "the folder the scenario added is listed");
  const juce::File more = engine.data.getChildFile("More captures");
  more.createDirectory();
  folders.addFolderPath(more.getFullPathName());
  check(folders.folderCount() == 2, "a folder can be added");
  SettingsCaptures again(*engine.client);
  again.refresh();
  check(again.folderCount() == 2, "and is kept, a new page sees it");
  folders.removeFolderPath(more.getFullPathName());
  check(folders.folderCount() == 1, "a folder can be removed");
}

int selfTest() {
  using namespace pedaleira::ui;
  int failures = 0, total = 0;
  const auto check = [&](bool ok, const char* what) {
    ++total;
    if (!ok) {
      ++failures;
      std::cerr << "FAIL  " << what << "\n";
    }
  };
  captureFindingChecks(check);
  ScratchEngine engine;
  juce::String error;
  if (!runScript(*engine.client, kFamiliesRig, error)) {
    std::cerr << "rig: " << error << "\n";
    return 1;
  }
  const auto id = [&](const char* slot) { return resolve(*engine.client, juce::var(slot)).toString(); };

  // Keyboard navigation over the model.
  GridModel model = GridModel::read(*engine.client);
  check(model.neighbour(id("$slot:0:0"), 1, 0) == id("$slot:0:1"), "right moves to the next block of the row");
  check(model.neighbour(id("$slot:0:1"), -1, 0) == id("$slot:0:0"), "left moves back");
  check(model.neighbour(id("$slot:0:0"), -1, 0) == id("$slot:0:0"), "left at the first block stays");
  check(model.neighbour(id("$slot:0:7"), 1, 0) == id("$slot:0:7"), "right at the last block stays");
  check(model.neighbour(id("$slot:0:3"), 0, 1) == id("$slot:1:3"), "down goes to the row below, same slot");
  check(model.neighbour(id("$slot:1:3"), 0, -1) == id("$slot:0:3"), "up goes back");
  check(model.neighbour(id("$slot:0:3"), 0, -1) == id("$slot:0:3"), "up at the first row stays");
  check(model.neighbour(id("$slot:1:3"), 0, 1) == id("$slot:1:3"), "down at the last shown row stays");
  check(model.neighbour("no-such-block", 1, 0) == id("$slot:0:0"), "an unknown block starts at the first");
  engine.client->run("chain.removeBlock", object({{"blockId", id("$slot:1:2")}}));
  model = GridModel::read(*engine.client);
  check(model.neighbour(id("$slot:0:2"), 0, 1) == id("$slot:1:1"), "down to the nearest block, the lower on a tie");
  engine.client->run("chain.undo");

  // The page: selection, Escape, the panel following the rig.
  GridPage page(*engine.client);
  page.setSize(1368, 836);
  check(page.selectedBlock() == id("$slot:0:0"), "the page opens on the first block");
  check(page.settingsPanel().hasBlock() && page.settingsPanel().blockId() == id("$slot:0:0"), "the panel shows it");
  page.moveSelection(1, 0);
  check(page.selectedBlock() == id("$slot:0:1"), "an arrow key moves the selection");
  check(page.settingsPanel().blockId() == id("$slot:0:1"), "and the panel follows");
  page.clearSelection();
  check(page.selectedBlock().isEmpty() && !page.settingsPanel().hasBlock(), "Escape clears the selection and the panel");
  page.rigChanged();
  check(page.selectedBlock().isEmpty(), "a rig change does not pick a block again");
  page.moveSelection(1, 0);
  check(page.selectedBlock() == id("$slot:0:0"), "an arrow key after Escape starts at the first block");

  page.selectBlock(id("$slot:0:0"));
  const auto drive = [&] { return page.settingsPanel().shownValue("drivePercent").value_or(-1.0); };
  check(std::abs(drive() - 70.0) < 0.01, "the panel shows the block's parameter");
  engine.client->run("block.setParam", object({{"blockId", id("$slot:0:0")}, {"param", "drivePercent"}, {"value", 20.0}}));
  page.rigChanged();
  check(std::abs(drive() - 20.0) < 0.01, "an outside change shows in the panel");
  engine.client->run("block.setParam", object({{"blockId", id("$slot:0:0")}, {"param", "drivePercent"}, {"value", 33.0}}));
  engine.client->run("chain.undo");
  page.rigChanged();
  check(std::abs(drive() - 33.0) > 0.01, "an undo shows in the panel");
  engine.client->run("block.setParam", object({{"blockId", id("$slot:0:0")}, {"param", "drivePercent"}, {"value", 20.0}}));
  engine.client->run("scene.assignParam", object({{"blockId", id("$slot:0:0")}, {"param", "drivePercent"}}));
  engine.client->run("scene.add");
  engine.client->run("scene.select", object({{"scene", "B"}}));
  engine.client->run("block.setParam", object({{"blockId", id("$slot:0:0")}, {"param", "drivePercent"}, {"value", 90.0}}));
  page.rigChanged();
  check(std::abs(drive() - 90.0) < 0.01, "scene B shows its own value");
  engine.client->run("scene.select", object({{"scene", "A"}}));
  page.rigChanged();
  check(std::abs(drive() - 20.0) < 0.01, "a scene switch shows scene A's value");

  // Collapse: a small window starts collapsed, the player's choice is kept.
  page.setSize(1048, 636);
  check(page.settingsCollapsed() && page.settingsPanel().getHeight() == BlockSettingsPanel::kBarHeight,
        "a small window collapses the panel to its bar");
  page.setSize(1368, 836);
  check(!page.settingsCollapsed() && page.settingsPanel().getHeight() >= BlockSettingsPanel::kExpandedHeight,
        "the default window docks the panel at full height");
  check(page.settingsPanel().getY() == 56 + GridView::heightForRows(2), "the grid takes only the room its two rows need");
  page.setSize(1048, 636);
  page.setSettingsCollapsed(false);
  check(!page.settingsCollapsed() && page.settingsPanel().getHeight() == BlockSettingsPanel::kExpandedHeight,
        "the player can expand it in a small window, over the grid");
  page.setSize(1368, 836);
  check(!page.settingsCollapsed(), "and the choice is kept");
  page.setSettingsCollapsed(true);
  check(page.settingsCollapsed(), "collapsing in a large window is kept too");

  // The curves and the grouping.
  check(std::abs(blockmath::peakDb(1000.0, 1000.0, 6.0, 1.0) - 6.0) < 0.1, "a peak band reaches its gain at its centre");
  check(std::abs(blockmath::lowShelfDb(30.0, 300.0, 6.0) - 6.0) < 0.4, "a low shelf reaches its gain below its corner");
  check(std::abs(blockmath::highShelfDb(20000.0, 4000.0, -6.0) + 6.0) < 0.6, "a high shelf reaches its gain above its corner");
  check(std::abs(blockmath::highPassDb(100.0, 100.0, 2) + 3.01) < 0.05, "a cut is 3 dB down at its corner");
  check(std::abs(blockmath::compressorOut(-10.0, -20.0, 4.0, 0.0) + 17.5) < 1.0e-6, "a compressor's slope above the threshold");
  check(blockmath::compressorOut(-30.0, -20.0, 4.0, 0.0) == -30.0, "and none below it");
  const auto group = [](Category c, const char* paramId) {
    KnobParam p;
    p.id = paramId;
    return paramGroup(c, p);
  };
  check(group(Category::delay, "feedback") == "Feedback", "a delay's feedback is grouped as Feedback");
  check(group(Category::drive, "tonePercent") == "Tone", "a drive's tone is grouped as Tone");
  check(group(Category::eq, "b3Gain") == "Band 3", "an EQ band is its own section");
  check(group(Category::utility, "levelDb") == "Output", "a level is output in any category");
  check(group(Category::utility, "somethingNew") == "Parameters", "an unknown parameter of an unknown kind is Parameters");
  check(group(Category::pitch, "dampingNew") == "Tone", "the keyword fallback groups a new parameter");

  // The amps: every shipped amp is a block kind, listed by the library under
  // its name and family, and drawn as its faceplate in the settings panel.
  {
    ScratchEngine amps;
    const juce::var listed = amps.client->read("kinds.list");
    std::vector<juce::var> ampKinds;
    int highGain = 0;
    for (const auto& kind : *listed["kinds"].getArray())
      if (kind["family"].toString().isNotEmpty()) {
        ampKinds.push_back(kind);
        highGain += kind["family"].toString() == "High gain" ? 1 : 0;
      }
    check(ampKinds.size() >= 7, "every shipped amp is a block kind with a family");

    GridPage library(*amps.client);
    library.setSize(1368, 836);
    library.openPicker(0, 0);
    auto& picker = library.picker();
    picker.setFilter("amp", 0, {});
    const juce::StringArray everyAmp = picker.shownEntries();
    bool listedWithFamily = true;
    for (const auto& kind : ampKinds)
      listedWithFamily = listedWithFamily && everyAmp.contains(kind["name"].toString() + " | " + kind["family"].toString());
    check(listedWithFamily, "the block library lists each amp under its own name and its family");
    picker.setFilter({}, 0, "high gain");
    if (picker.shownEntries().size() != highGain)
      std::cerr << "family search found: " << picker.shownEntries().joinIntoString(" / ") << " (wanted " << highGain << ")\n";
    check(picker.shownEntries().size() == highGain, "a search for a family finds the amps of that family");
    picker.setFilter({}, 0, "red forty");
    check(picker.shownEntries().size() == 1 && picker.shownEntries()[0] == "Red Forty | High gain",
          "a search for a name finds that amp");
    picker.setFilter({}, 0, "clean");
    check(picker.shownEntries().isEmpty() || !picker.shownEntries()[0].startsWith("Red Forty"), "and not the others");

    amps.client->run("grid.convert", object({{"to", "grid"}}));
    GridPage page(*amps.client);
    page.setSize(1368, 836);
    // The panel stays collapsed once a player collapsed it, in any page of the session.
    page.setSettingsCollapsed(false);
    const auto blockId = [&] { return resolve(*amps.client, juce::var("$slot:0:0")).toString(); };
    const auto paramOf = [&](const juce::String& block, const juce::String& param) {
      const juce::var params = amps.client->read("block.listParams", object({{"blockId", block}}));
      for (const auto& item : *params["params"].getArray())
        if (item["id"].toString() == param)
          return static_cast<double>(item["value"]);
      return -1.0;
    };
    // The layout of a faceplate: every box inside it, none over another, the
    // nameplate clear of them, every knob inside its box, the strip below.
    const auto checkLayout = [&](const AmpFaceplate& plate, const juce::String& label) {
      const auto area = plate.getLocalBounds();
      bool inside = true, apart = true, knobsInside = true;
      for (int i = 0; i < static_cast<int>(plate.spec().groups.size()); ++i) {
        const auto box = plate.groupBounds(i);
        if (box.isEmpty())
          continue;
        inside = inside && area.contains(box) && box.getWidth() >= 90;
        apart = apart && !box.intersects(plate.nameplateBounds());
        for (int j = i + 1; j < static_cast<int>(plate.spec().groups.size()); ++j)
          apart = apart && !box.intersects(plate.groupBounds(j));
        for (const auto& control : plate.spec().groups[static_cast<size_t>(i)].controls)
          if (const auto* knob = dynamic_cast<const ParamKnob*>(plate.controlFor(control.id)))
            knobsInside = knobsInside && box.contains(knob->getBounds());
      }
      check(inside, ("the boxes sit inside the faceplate: " + label).toRawUTF8());
      check(apart, ("no box overlaps another or the nameplate: " + label).toRawUTF8());
      check(knobsInside, ("every knob sits inside its box: " + label).toRawUTF8());
      check(plate.nameplateBounds().getWidth() > 100 && plate.nameplateBounds().getHeight() >= 40, ("the nameplate has room: " + label).toRawUTF8());
      check(plate.knobDiameter() >= 34, ("the knobs are big enough to use: " + label).toRawUTF8());
      check(plate.stripBounds().getHeight() == AmpFaceplate::kStripHeight && plate.stripBounds().getBottom() == area.getBottom(),
            ("the slim strip runs along the bottom: " + label).toRawUTF8());
      const auto* level = plate.controlFor("levelDb");
      check(level != nullptr && plate.stripBounds().contains(level->getBounds()), ("the Level knob is in the strip: " + label).toRawUTF8());
    };

    for (const auto& kind : ampKinds) {
      const juce::String name = kind["name"].toString();
      amps.client->run("chain.addBlock", object({{"kind", kind["id"]}, {"row", 0}, {"slot", 0}}));
      page.rigChanged();
      page.selectBlock(blockId());
      const AmpFaceplate* plate = page.settingsPanel().ampFaceplate();
      check(plate != nullptr, ("an amp block shows its faceplate: " + name).toRawUTF8());
      if (plate == nullptr)
        continue;
      check(!page.settingsPanel().hasPicture(), "and no picture and sections above it");
      check(plate->spec().name == name && !plate->spec().groups.empty() && plate->spec().styled, ("the faceplate is the amp's own: " + name).toRawUTF8());
      checkLayout(*plate, name);

      // Every control of the description is drawn: a knob for each parameter that is one.
      int wanted = 0;
      bool all = true;
      for (const auto& group : plate->spec().groups)
        for (const auto& control : group.controls) {
          const bool switchLike = control.id == "bright" || control.id == "voicing";
          wanted += switchLike ? 0 : 1;
          all = all && (plate->controlFor(control.id) != nullptr || (control.id == "voicing" && plate->spec().voicings.size() < 2));
        }
      check(all && plate->numKnobs() == wanted, ("every control of the panel is drawn: " + name).toRawUTF8());

      // Turning a knob goes through block.setParam, and the panel shows it.
      auto* gain = dynamic_cast<ParamKnob*>(plate->controlFor("gain"));
      check(gain != nullptr, "the Gain knob is a parameter knob");
      if (gain != nullptr) {
        const juce::String block = blockId();
        gain->setPosition(0.3, juce::sendNotification);
        check(std::abs(paramOf(block, "gain") - 30.0) < 0.01, ("turning Gain sets the parameter: " + name).toRawUTF8());
        page.rigChanged();
        check(std::abs(page.settingsPanel().shownValue("gain").value_or(-1.0) - 30.0) < 0.01, "the panel shows the new value");
        check(gain->getValueText() == "3.0", "and reads it on the 0 to 10 of the panel");
        // Hovering shows the value in a bubble, and so does dragging.
        check(plate->bubbleText().isEmpty(), "no bubble while the mouse is away");
        moveOver(*gain);
        check(plate->bubbleText() == gain->getName() + " 3.0", "hovering a knob shows its value in a bubble");
        moveOut(*gain);
        check(plate->bubbleText().isEmpty(), "the bubble goes when the mouse leaves");
        dragBy(*gain, -40, [&] { check(plate->bubbleText() == gain->getName() + " " + gain->getValueText(), "dragging shows the value in the bubble"); });
        // A knob turns 20 points for 40 px; a fader's travel is its own length.
        const bool fader = plate->spec().cap == Knob::Cap::fader;
        check(fader ? paramOf(block, "gain") > 31.0 : std::abs(paramOf(block, "gain") - 50.0) < 0.5,
              ("dragging a knob up turns it up: " + name).toRawUTF8());
      }
      if (auto* bright = dynamic_cast<juce::Button*>(plate->controlFor("bright"))) {
        const double before = paramOf(blockId(), "bright");
        // A click toggles the button and runs its onClick (triggerClick is posted to the message loop).
        bright->setToggleState(!bright->getToggleState(), juce::dontSendNotification);
        bright->onClick();
        check(paramOf(blockId(), "bright") == 1.0 - before, ("the Bright switch sets the parameter: " + name).toRawUTF8());
      }
      if (auto* voicing = dynamic_cast<FaceVoicingSwitch*>(plate->controlFor("voicing"))) {
        voicing->setSelected(plate->spec().voicings.size() - 1, juce::sendNotification);
        check(static_cast<int>(paramOf(blockId(), "voicing")) == plate->spec().voicings.size() - 1,
              ("the Voicing switch sets the parameter: " + name).toRawUTF8());
      }
      amps.client->run("chain.removeBlock", object({{"blockId", blockId()}}));
    }

    // An amp whose description gives no style gets the plain default faceplate;
    // two others use the caps, strips and switch styles the six above do not.
    struct Variant {
      const char* file;
      const char* name;
      bool styled;
      Knob::Cap cap;
    };
    for (const Variant& variant : {Variant{"plain-amp.json", "My Own Amp", false, Knob::Cap::round},
                                   Variant{"chicken-amp.json", "Chicken Test", true, Knob::Cap::chickenHead},
                                   Variant{"fader-amp.json", "Fader Test", true, Knob::Cap::fader}}) {
      amps.client->run("chain.addBlock", object({{"kind", "amp.custom"}, {"row", 0}, {"slot", 0}}));
      const auto loaded = amps.client->run(
          "block.loadDescription", object({{"blockId", blockId()}, {"file", resolve(*amps.client, juce::var(juce::String("$tmp/") + variant.file))}}));
      check(loaded.ok, "the variant description loads");
      page.rigChanged();
      page.selectBlock(blockId());
      const AmpFaceplate* plate = page.settingsPanel().ampFaceplate();
      check(plate != nullptr && plate->spec().name == variant.name, (juce::String("a custom amp shows its faceplate: ") + variant.name).toRawUTF8());
      if (plate != nullptr) {
        check(plate->spec().styled == variant.styled, "styled or the plain default");
        check(plate->spec().cap == variant.cap, "the cap of the description");
        check(variant.styled || plate->spec().accent == categoryColour(Category::amp), "the plain default takes the amp category colour");
        check(variant.styled || (plate->spec().groups.size() == 2 && plate->spec().strip == FaceStrip::none), "the plain default is two boxes and no strip");
        check(plate->controlFor("treble") != nullptr && plate->controlFor("voicing") != nullptr, "the plain default has every control");
        checkLayout(*plate, variant.name);
      }
      amps.client->run("chain.removeBlock", object({{"blockId", blockId()}}));
    }
  }

  // Capture and IR files dropped on the grid.
  {
    ScratchEngine drops;
    juce::String dropError;
    if (!runScript(*drops.client, kEditorRig, dropError)) {
      std::cerr << "drop rig: " << dropError << "\n";
      return 1;
    }
    GridPage page(*drops.client);
    page.setSize(1368, 836);
    GridView& view = page.gridView();
    const auto slotId = [&](int row, int slot) { return resolve(*drops.client, juce::var("$slot:" + juce::String(row) + ":" + juce::String(slot))).toString(); };
    const auto paramOf = [&](const juce::String& block, const juce::String& param) {
      const juce::var params = drops.client->read("block.listParams", object({{"blockId", block}}));
      for (const auto& item : *params["params"].getArray())
        if (item["id"].toString() == param)
          return static_cast<double>(item["value"]);
      return -1.0;
    };
    const auto slotOf = [&](int row, int slot) { return GridModel::read(*drops.client).rows[static_cast<size_t>(row)].slots[static_cast<size_t>(slot)]; };
    const auto centreOf = [&](int row, int slot) { return view.slotBounds(row, slot).getCentre(); };
    const auto dropOn = [&](const juce::String& file, int row, int slot) {
      const auto at = centreOf(row, slot);
      view.filesDropped({file}, at.x, at.y);
      drops.client->run("state.waitUntilLoaded");
      page.rigChanged();
    };
    const juce::String nam = testFiles().getChildFile("a2-amp-cab-test.nam").getFullPathName();
    const juce::String ir = testFiles().getChildFile("cab-ir-test-2.wav").getFullPathName();
    const juce::File notes = drops.data.getChildFile("notes.txt");
    notes.replaceWithText("not a capture");

    check(view.isInterestedInFileDrag({nam}) && view.isInterestedInFileDrag({ir}), "the grid takes .nam and .wav files");
    check(!view.isInterestedInFileDrag({notes.getFullPathName()}), "and nothing else");
    check(!view.isInterestedInFileDrag({drops.data.getFullPathName()}), "not a folder either");

    // A drop on a capture replaces it in place, keeping its settings.
    const juce::String amp = slotId(0, 2);
    const juce::String ampName = slotOf(0, 2).name;
    const double gainBefore = paramOf(amp, "inputGain"), levelBefore = paramOf(amp, "outputGain");
    check(std::abs(gainBefore - 0.55) < 0.01, "the capture starts with the settings of the rig");
    {
      const auto at = centreOf(0, 2);
      view.fileDragEnter({nam}, at.x, at.y);
      check(view.fileDropTarget() == std::make_pair(0, 2), "a capture dragged over a capture outlines its slot");
      const auto empty = centreOf(0, 6);
      view.fileDragMove({nam}, empty.x, empty.y);
      check(view.fileDropTarget() == std::make_pair(0, 6), "and the outline follows to an empty slot");
      view.fileDragMove({ir}, at.x, at.y);
      check(!view.fileDropTarget().has_value(), "but an impulse response over a capture outlines nothing");
      view.fileDragExit({nam});
      check(!view.fileDropTarget().has_value(), "and the outline goes when the drag leaves");
    }
    dropOn(nam, 0, 2);
    check(slotId(0, 2) == amp, "a drop on a capture keeps its block");
    check(!slotOf(0, 2).empty && slotOf(0, 2).kindId == "nam" && slotOf(0, 2).name != ampName,
          "and puts the new capture in it");
    check(std::abs(paramOf(amp, "inputGain") - gainBefore) < 1.0e-6 && std::abs(paramOf(amp, "outputGain") - levelBefore) < 1.0e-6,
          "with its settings kept");
    check(page.selectedBlock() == amp, "and selects it");

    // A drop on an empty slot adds a block there.
    check(slotOf(0, 6).empty, "slot 7 starts empty");
    dropOn(nam, 0, 6);
    check(!slotOf(0, 6).empty && slotOf(0, 6).kindId == "nam", "a drop on an empty slot adds a capture there");
    dropOn(ir, 0, 7);
    check(!slotOf(0, 7).empty && slotOf(0, 7).kindId == "ir", "and an impulse response too");

    // An IR replaces an IR; the wrong kind and other files change nothing.
    const juce::String cab = slotId(0, 3);
    const juce::String cabName = slotOf(0, 3).name;
    dropOn(nam, 0, 3);
    check(slotOf(0, 3).kindId == "ir" && slotOf(0, 3).name == cabName, "a capture does not replace an impulse response");
    dropOn(ir, 0, 2);
    check(slotOf(0, 2).kindId == "nam", "nor an impulse response a capture");
    dropOn(nam, 0, 0);
    check(slotOf(0, 0).kindId == "dynamics.gate", "nor does a file replace an effect");
    dropOn(notes.getFullPathName(), 0, 6);
    check(slotOf(0, 6).kindId == "nam", "a file that is not a capture changes nothing");
    dropOn(ir, 0, 3);
    check(slotId(0, 3) == cab && slotOf(0, 3).kindId == "ir" && slotOf(0, 3).name != cabName,
          "an impulse response dropped on one replaces it in place");

    // The drag marks no slot where a drop would do nothing.
    const auto onGate = centreOf(0, 0);
    view.fileDragEnter({nam}, onGate.x, onGate.y);
    check(!view.fileDropTarget().has_value(), "a capture dragged over an effect outlines nothing");
    view.fileDragExit({nam});
    int added = 0;
    view.onDropFiles = [&](const juce::StringArray&, int, int) { ++added; };
    view.filesDropped({nam}, onGate.x, onGate.y);
    check(added == 0, "the grid hands nothing on for a slot that can not take the file");
    const auto atCapture = centreOf(0, 2);
    view.filesDropped({nam}, atCapture.x, atCapture.y);
    check(added == 1, "and hands on a drop that it can");

    // The capture editor's Load file... replaces the block's capture in place.
    page.openEditor(amp);
    auto* editor = dynamic_cast<NamEditor*>(page.blockEditor().currentBody());
    check(page.isEditorOpen() && editor != nullptr, "a capture's block opens the capture editor");
    if (editor != nullptr) {
      const juce::String before = slotOf(0, 2).name;
      const juce::File other = testFiles().getChildFile("a2-am-test-2.nam");
      editor->swapTo(other);
      drops.client->run("state.waitUntilLoaded");
      page.rigChanged();
      check(slotId(0, 2) == amp && slotOf(0, 2).name == other.getFileNameWithoutExtension() && slotOf(0, 2).name != before,
            "a file loaded from the editor replaces the capture in its block");
      check(std::abs(paramOf(amp, "inputGain") - gainBefore) < 1.0e-6 && std::abs(paramOf(amp, "outputGain") - levelBefore) < 1.0e-6,
            "and the editor leaves its settings");
      page.openEditor(amp);
      editor = dynamic_cast<NamEditor*>(page.blockEditor().currentBody());
      check(editor != nullptr && editor->modelSizeShown(), "a slimmable capture shows its model size");
      // A Linear capture comes in one size: the choice gives way to a note.
      const juce::File oneSize = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pedaleira-one-size.nam");
      oneSize.replaceWithText(R"({"version":"0.5.4","architecture":"Linear","config":{"receptive_field":1,"bias":false},)"
                              R"("weights":[1.0],"sample_rate":48000})");
      if (editor != nullptr)
        editor->swapTo(oneSize);
      drops.client->run("state.waitUntilLoaded");
      page.rigChanged();
      page.openEditor(amp);
      editor = dynamic_cast<NamEditor*>(page.blockEditor().currentBody());
      check(editor != nullptr && !editor->modelSizeShown(), "a capture of one size hides the model size");
      oneSize.deleteFile();
    }
  }

  std::cout << (total - failures) << " of " << total << " checks passed\n";
  return failures == 0 ? 0 : 1;
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
    if (const juce::String steps(scenario.nam); steps.isNotEmpty()) {
      const juce::String problem = pedaleira::ui::testbed::prepareNam(*engine.client, engine.data, testFiles(), steps);
      if (problem.isNotEmpty()) {
        std::cerr << scenario.name << ": " << problem << "\n";
        ++failures;
        continue;
      }
    }
    // The block library's TONE3000 page, over a feed that answers at once.
    std::unique_ptr<pedaleira::ui::testbed::FakeTone3000Feed> tone3000;
    if (const juce::String kind(scenario.tone3000); kind != "none") {
      pedaleira::ui::testbed::FakeTone3000Feed::Setup setup;
      setup.signedIn = kind == "in";
      setup.unreachable = kind == "down";
      setup.introSeen = kind != "first";
      setup.capture = testFiles().getChildFile("a2-amp-test.nam");
      setup.engine = engine.client.get();
      tone3000 = std::make_unique<pedaleira::ui::testbed::FakeTone3000Feed>(setup);
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
        if (tone3000 != nullptr)
          grid->setTone3000Feed(tone3000.get());
        for (const auto& open : juce::StringArray::fromTokens(scenario.open, ";", "")) {
          if (open.startsWith("picker:")) {
            const juce::StringArray parts = juce::StringArray::fromTokens(open.substring(7), ":", "");
            grid->openPicker(parts[0].getIntValue(), parts[1].getIntValue());
            if (parts.size() > 2) {
              // A dash leaves the category out.
              grid->picker().setFilter(parts[2] == "-" ? juce::String() : parts[2], parts[3].getIntValue(), parts[4]);
            }
          } else if (open.startsWith("pickclick:")) {
            grid->picker().clickName(open.fromFirstOccurrenceOf(":", false, false));
          } else if (open.startsWith("t3k:")) {
            auto& panel = grid->picker().tone3000Panel();
            const juce::StringArray parts = juce::StringArray::fromTokens(open.substring(4), ":", "");
            if (parts[0] == "tab")
              panel.selectTab(static_cast<pedaleira::ui::Tone3000List>(parts[1].getIntValue()));
            else if (parts[0] == "tone")
              panel.selectTone(parts[1].getIntValue());
            else if (parts[0] == "capture")
              panel.selectCapture(parts[1].getIntValue());
            else if (parts[0] == "add")
              panel.addSelected();
          } else if (open.startsWith("editor:")) {
            grid->openEditor(resolve(*engine.client, open.substring(7)).toString());
          } else if (open.startsWith("dragover:") || open.startsWith("drop:")) {
            const juce::String rest = open.fromFirstOccurrenceOf(":", false, false);
            const juce::String file = resolve(*engine.client, rest.upToLastOccurrenceOf("@", false, false)).toString();
            const juce::StringArray at = juce::StringArray::fromTokens(rest.fromLastOccurrenceOf("@", false, false), ":", "");
            const auto spot = grid->gridView().slotBounds(at[0].getIntValue(), at[1].getIntValue()).getCentre();
            if (open.startsWith("drop:")) {
              grid->gridView().filesDropped({file}, spot.x, spot.y);
              engine.client->run("state.waitUntilLoaded");
              grid->rigChanged();
            } else {
              grid->gridView().fileDragEnter({file}, spot.x, spot.y);
            }
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
      } else if (page == Page::live) {
        pages.push_back(std::make_unique<pedaleira::ui::LivePage>(*engine.client));
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
    shell.setSize(scenario.width > 0 ? scenario.width : pedaleira::ui::size::windowWidth,
                  scenario.height > 0 ? scenario.height : pedaleira::ui::size::windowHeight);
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
    if (!juce::String(scenario.live).isEmpty())
      driveLive(static_cast<pedaleira::ui::LivePage&>(*pages[4]), scenario.live);
    if (!scenario.gallery && juce::String(scenario.keys).isNotEmpty()) {
      auto& grid = static_cast<pedaleira::ui::GridPage&>(*pages[0]);
      for (const auto& key : juce::StringArray::fromTokens(scenario.keys, ";", "")) {
        if (key == "escape")
          grid.clearSelection();
        else if (key == "left")
          grid.moveSelection(-1, 0);
        else if (key == "right")
          grid.moveSelection(1, 0);
        else if (key == "up")
          grid.moveSelection(0, -1);
        else if (key == "down")
          grid.moveSelection(0, 1);
        else if (key == "expand")
          grid.setSettingsCollapsed(false);
        else if (key == "collapse")
          grid.setSettingsCollapsed(true);
      }
    }
    if (const juce::String hover(scenario.hover); hover.isNotEmpty() && !scenario.gallery) {
      auto& grid = static_cast<pedaleira::ui::GridPage&>(*pages[0]);
      const auto* plate = grid.settingsPanel().ampFaceplate();
      if (plate == nullptr || plate->controlFor(hover) == nullptr) {
        std::cerr << scenario.name << ": no control '" << hover << "' on the faceplate\n";
        ++failures;
      } else {
        moveOver(*plate->controlFor(hover));
      }
    }
    // The live levels and readings the shell would hand the page.
    if (scenario.audio && !scenario.gallery && scenario.page == Page::grid)
      static_cast<pedaleira::ui::GridPage&>(*pages[0]).settingsPanel().showMeters(engine.client->read("meters.get"));
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
  // Pictures show where things end up, never halfway through a motion.
  pedaleira::ui::motion::setEnabled(false);
  juce::StringArray arguments;
  for (int i = 1; i < argc; ++i)
    arguments.add(juce::String::fromUTF8(argv[i]));

  if (arguments.contains("--list")) {
    for (const auto& scenario : scenarios())
      std::cout << scenario.name << "  " << scenario.description << "\n";
    return 0;
  }
  if (arguments.contains("--selftest"))
    return selfTest();
  const int at = arguments.indexOf("--capture");
  if (at < 0 || at + 1 >= arguments.size()) {
    std::cerr << "usage: PedaleiraUiTestbed --capture <folder> [--scale 1|2] [scenario...]\n"
                 "       PedaleiraUiTestbed --selftest\n"
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
