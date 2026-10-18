#pragma once

#include <juce_core/juce_core.h>

#include "json.hpp"

#include <vector>

// What the engine knows about NAM capture files beyond their bytes: whether
// Pedaleira can load one and what it is (the architecture check), what a file the user loaded
// was called (the stash keeps only content-hash names), the list of recently
// loaded captures, and the captures downloaded from TONE3000.
// Any thread; one mutex guards the recents file.
namespace NamLibrary {

// What a .nam file says about itself, read without its weights.
struct Info {
  // The file parsed as a NAM model at all.
  bool readable = false;
  // Pedaleira can load it: an architecture NeuralAmpModelerCore builds
  // (WaveNet A1 or A2, LSTM, ConvNet, Linear, a slimmable container of
  // those), a NAM version it reads, and settings that are complete. The
  // weights are not looked at here; loading checks them.
  bool supported = false;
  // The "architecture" field as written ("WaveNet", "LSTM", ...).
  juce::String architecture;
  // A short name for the architecture a player recognises: "WaveNet A2",
  // "WaveNet A1", "LSTM", "ConvNet", "Linear", "Slimmable A2", "Slimmable".
  juce::String label;
  // NAM A2: an A2 WaveNet, or a container whose models all are.
  bool a2 = false;
  // The rate the file says the model was trained at, 0 when it does not say
  // (it then runs at 48 kHz). The engine runs it at this rate whatever the
  // session's.
  double sampleRate = 0.0;
  // A sentence for the player when the file cannot be loaded (also when it
  // cannot be read); empty for a supported file.
  juce::String reason;
  // metadata.name, empty when the file has none.
  juce::String name;
};

// The check on a parsed model. The weights need not be in it.
Info inspectJson(const nlohmann::json& model);
// The sentence for a file that passed inspectJson but could not be built
// (NamModels::build threw `what`, usually weights that do not fit the
// settings).
juce::String damagedReason(const Info& info, const juce::String& what);
// The same on the bytes of a file: parsed with the weight arrays skipped, so a
// large capture costs time but not memory.
Info inspectBytes(const void* data, size_t size);
// The same on a file on disk; unreadable or oversize files come back not
// readable, with a reason.
Info inspectFile(const juce::File& file);

// ---- Names of loaded files and the recent list ----

struct Loaded {
  // The file name without its extension, as the player knew it.
  juce::String name;
  // The file it was loaded from (empty when only its name is known).
  juce::String sourcePath;
  // The copy in the model stash (a content-hash name).
  juce::String stashFile;
  // "capture" (.nam) or "ir" (.wav).
  juce::String kind;
  // When it was last loaded, in milliseconds since 1970.
  juce::int64 loadedAt = 0;
  // Shown in the recent list; false after nam.clearRecent.
  bool recent = true;
  // Where the file came from on TONE3000 (TONE3000Processor::
  // tone3000Attribution), so its creator and licence stay shown; void for a
  // file that did not come from there.
  juce::var tone3000;
};

// Remembers that `name` was loaded from `sourcePath` and kept as `stashFile`
// (a file name in the stash). Called by the model loader for every local file
// it stashes. Keeps the newest kMaxRemembered; the file is
// <app data>/NamRecents.json, outside the stash, whose files age out.
void noteLoaded(const juce::String& name, const juce::String& sourcePath,
                const juce::String& stashFile);
// A file that is already in the stash was loaded again by its hash name:
// brings its record to the front of the recent list and keeps the name it has.
void touchLoaded(const juce::String& stashFile);
// The stash file came from TONE3000: keeps the attribution with its record
// (and with every later load of the same bytes).
void noteTone3000(const juce::String& stashFile, const juce::var& attribution);
// The record for a stash file name, or an empty name when none.
Loaded loadedFor(const juce::String& stashFileName);
// The loaded files, newest first. `onlyRecent` leaves out the ones cleared
// from the recent list.
std::vector<Loaded> loaded(bool onlyRecent);
// Takes everything off the recent list; the names stay, so search still finds
// the files.
void clearRecent();
// Tests: forget everything held in memory (the file is read again on use).
void resetForTesting();

constexpr int kMaxRemembered = 1000;

// ---- Captures downloaded from TONE3000 ----
//
// The TONE3000 desktop app keeps no downloads folder, so there is nothing of
// it to look for. A capture the player downloads on Pedaleira's own TONE3000
// page is kept in the library: <library root>/TONE3000/<tone> - <capture>.nam,
// with an index (downloads.json, beside the files) that remembers which tone
// and capture each file is and who made it, so a tone is downloaded once and
// its credit stays with the file after a restart.

constexpr const char* kDownloadsFolder = "TONE3000";

// <root>/TONE3000, where the downloads of the library at `root` are kept.
juce::File downloadsDir(const juce::File& root);
// A file name (no extension) for a capture, made from the tone's title and the
// capture's name: "Title - Capture". Safe on every OS: no characters
// Windows or macOS refuse, no control characters, no trailing dot or space,
// not a reserved Windows device name, at most 120 characters; never empty.
juce::String downloadBaseName(const juce::String& toneTitle, const juce::String& captureName);
// The file kept for this tone and capture, or an empty File when none is (it
// was never downloaded, or the file was deleted).
juce::File findDownload(const juce::File& root, int toneId, int captureId);
// Moves `temp` (a .nam or .wav the caller downloaded) into the downloads folder
// of the library at `root` under a name made from the attribution's title and
// capture (a number is added on a collision) and records it with its
// attribution. `tone3000` is the cleaned attribution (TONE3000Processor::
// tone3000Attribution); its id is the tone's. `kept` is the new file.
juce::Result keepDownload(const juce::File& root, const juce::File& temp, int captureId,
                          const juce::var& tone3000, juce::File& kept);
// What the index remembers about a file in the downloads folder: the
// attribution, or void when the file is not in it.
juce::var downloadCredit(const juce::File& root, const juce::File& file);

}  // namespace NamLibrary
