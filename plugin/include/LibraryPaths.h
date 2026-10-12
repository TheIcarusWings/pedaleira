#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// Where the library and the agent layer keep their files, and the one check
// that decides whether a path a command server client names may be touched.
//
// The library root is resolved here, in one function: <Documents>/Pedaleira by
// default. The setlist library (SetlistLibrary::defaultRoot) and the agent
// layer both come here, so Setlists, Assets and Guitars are always siblings
// under one folder (docs/library.md, "Folder layout").
namespace LibraryPaths {

// <user app data>/Pedaleira (with "Application Support" on macOS): the folder
// the settings, presets and model stash already live in.
juce::File appDataDir();

// The library root every processor made without one of its own uses: the
// setlists, the asset store and the guitar profiles.
juce::File libraryRoot();
// Tests and tools: keep the library in `dir` instead. Process wide; an empty
// File restores the default. Returns the previous override. The same
// override as TONE3000Processor::setLibraryRootForTesting and
// SetlistLibrary::setRootForTesting, which forward here.
juce::File setLibraryRootForTesting(const juce::File& dir);
// True while such an override is in place.
bool libraryRootIsOverridden();

// The guitar profiles of the library at `root`: <root>/Guitars, beside
// Setlists and Assets. The agent layer passes the root of the library the
// rig being played uses (TONE3000Processor::getLibraryRoot), so a processor
// whose library a test re-rooted keeps its guitars there too.
juce::File guitarsDir(const juce::File& root);

// The captures of the library at `root`: <root>/Captures. Saved captures are
// .nam files in it; the working folder of each capture session sits beside
// them (<root>/Captures/<id>/, with the recordings and the trainer's output).
juce::File capturesDir(const juce::File& root);
// True for a file in a capture session's working folder (a folder under
// Captures that holds session.json): recordings and
// unsaved models, which are nobody's to load as an IR or a tone.
bool isCaptureWorkFile(const juce::File& file, const juce::File& root);

// Creates `dir` and any missing parents, each private to the user (mode 0700
// on POSIX, given to mkdir itself; on Windows the folder inherits the user
// profile's ACL). A directory that is already there is left as it is,
// whatever its mode. False when it cannot be made.
bool createPrivateDirectory(const juce::File& dir);

// Where renders written on request and other throwaway files go:
// <app data>/Pedaleira/Scratch, per user, created on first use and private to
// the user. An empty File when it cannot be used, with `problem` saying why:
// it could not be created, or the path is a symbolic link (never followed).
juce::File scratchDir(juce::String* problem = nullptr);
// Tests and the headless host: use `dir` as the scratch folder. Process
// wide; an empty File restores the default. Returns the previous override.
juce::File setScratchDirForTesting(const juce::File& dir);

// True when `path` lies inside `root`, after resolving `..` and symbolic
// links on both (a link inside an allowed folder that points outside it is
// outside). The root itself is not inside it. A path that does not exist yet
// is judged by its nearest existing parent.
bool isInside(const juce::File& path, const juce::File& root);

// True when `path` lies inside at least one of `roots`.
bool isInsideAny(const juce::File& path, const std::vector<juce::File>& roots);

// A name reduced to lower-case letters, digits and dashes, for folder and
// file names made from user text ("Les Paul #2" -> "les-paul-2"). Never
// empty ("item" when nothing survives), never longer than 48 characters.
juce::String slug(const juce::String& text);

}  // namespace LibraryPaths
