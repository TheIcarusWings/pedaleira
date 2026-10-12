#include "LibraryPaths.h"

#include "AppIdentity.h"

#include <cerrno>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#if !JUCE_WINDOWS
#include <sys/stat.h>
#endif

namespace {

std::mutex& overrideMutex() {
  static std::mutex mutex;
  return mutex;
}

juce::File& libraryOverride() {
  static juce::File file;
  return file;
}

juce::File& scratchOverride() {
  static juce::File file;
  return file;
}

// The path with `..` and links resolved. weakly_canonical accepts a path
// whose tail does not exist yet (a file about to be written).
std::filesystem::path canonical(const juce::File& file) {
  std::error_code error;
#if JUCE_WINDOWS
  const std::filesystem::path raw(file.getFullPathName().toWideCharPointer());
#else
  const std::filesystem::path raw(file.getFullPathName().toStdString());
#endif
  std::filesystem::path resolved = std::filesystem::weakly_canonical(raw, error);
  if (error)
    return {};
  return resolved;
}

}  // namespace

namespace LibraryPaths {

juce::File appDataDir() {
  juce::File base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
  base = base.getChildFile("Application Support");
#endif
  return base.getChildFile(kAppDataFolderName);
}

juce::File libraryRoot() {
  {
    const std::lock_guard<std::mutex> lock(overrideMutex());
    if (libraryOverride() != juce::File())
      return libraryOverride();
  }
  return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
      .getChildFile(kAppDataFolderName);
}

juce::File setLibraryRootForTesting(const juce::File& dir) {
  const std::lock_guard<std::mutex> lock(overrideMutex());
  return std::exchange(libraryOverride(), dir);
}

bool libraryRootIsOverridden() {
  const std::lock_guard<std::mutex> lock(overrideMutex());
  return libraryOverride() != juce::File();
}

juce::File guitarsDir(const juce::File& root) { return root.getChildFile("Guitars"); }

juce::File capturesDir(const juce::File& root) { return root.getChildFile("Captures"); }

bool isCaptureWorkFile(const juce::File& file, const juce::File& root) {
  // A capture session's own folder (<captures>/<id>, holding session.json)
  // is work in progress. Any other folder under Captures is the user's own
  // filing of finished captures and is listed as usual.
  const juce::File captures = capturesDir(root);
  if (!file.isAChildOf(captures))
    return false;
  juce::File session = file.getParentDirectory();
  while (session.getParentDirectory() != captures && session != captures && session.isAChildOf(captures))
    session = session.getParentDirectory();
  return session != captures && session.getChildFile("session.json").existsAsFile();
}

bool createPrivateDirectory(const juce::File& dir) {
  if (dir == juce::File())
    return false;
  if (dir.isDirectory())
    return true;
#if JUCE_WINDOWS
  // The per-user folders this is used under take their access from the user
  // profile's ACL, which a new folder inherits.
  return dir.createDirectory().wasOk();
#else
  const juce::File parent = dir.getParentDirectory();
  if (parent != dir && !createPrivateDirectory(parent))
    return false;
  // The mode is given to mkdir itself, so the folder is never open to others
  // for a moment first. Losing a race with another creator is fine as long as
  // what is there is a directory.
  if (::mkdir(dir.getFullPathName().toRawUTF8(), S_IRWXU) != 0 && errno != EEXIST)
    return false;
  return dir.isDirectory();
#endif
}

juce::File scratchDir(juce::String* problem) {
  juce::File dir;
  {
    const std::lock_guard<std::mutex> lock(overrideMutex());
    dir = scratchOverride();
  }
  // Per user, under the app data folder: the shared temp directory is a
  // place where another account can put a folder or a link of that name
  // first.
  if (dir == juce::File())
    dir = appDataDir().getChildFile("Scratch");

  const auto refuse = [&](const juce::String& why) {
    if (problem != nullptr)
      *problem = why;
    return juce::File();
  };
  // A link would send what is written here, and the allowed folder with it,
  // wherever the link points. Looked at before and after creating: mkdir on a
  // link to a folder reports that it exists.
  const juce::String linked = "the scratch folder " + dir.getFullPathName() +
                              " is a symbolic link; Pedaleira does not use it (remove the link)";
  if (dir.isSymbolicLink())
    return refuse(linked);
  if (!createPrivateDirectory(dir))
    return refuse("the scratch folder " + dir.getFullPathName() + " could not be created");
  if (dir.isSymbolicLink())
    return refuse(linked);
  return dir;
}

juce::File setScratchDirForTesting(const juce::File& dir) {
  const std::lock_guard<std::mutex> lock(overrideMutex());
  return std::exchange(scratchOverride(), dir);
}

bool isInside(const juce::File& path, const juce::File& root) {
  if (path == juce::File() || root == juce::File())
    return false;
  const std::filesystem::path resolvedPath = canonical(path);
  const std::filesystem::path resolvedRoot = canonical(root);
  if (resolvedPath.empty() || resolvedRoot.empty())
    return false;

  // Component by component: "/a/bc" is not inside "/a/b".
  auto pathPart = resolvedPath.begin();
  for (auto rootPart = resolvedRoot.begin(); rootPart != resolvedRoot.end(); ++rootPart) {
    // A root written with a trailing separator ends in an empty component.
    if (rootPart->empty())
      continue;
    if (pathPart == resolvedPath.end() || *pathPart != *rootPart)
      return false;
    ++pathPart;
  }
  // Something has to be left: the root is the fence, not a place inside it
  // (a client that could name the root could have it replaced or scanned as
  // a whole). A trailing separator leaves an empty component, which is
  // nothing.
  for (; pathPart != resolvedPath.end(); ++pathPart)
    if (!pathPart->empty())
      return true;
  return false;
}

bool isInsideAny(const juce::File& path, const std::vector<juce::File>& roots) {
  for (const auto& root : roots)
    if (isInside(path, root))
      return true;
  return false;
}

juce::String slug(const juce::String& text) {
  juce::String out;
  bool dash = false;
  for (const juce::juce_wchar c : text.toLowerCase()) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (keep) {
      if (dash && out.isNotEmpty())
        out << '-';
      out << juce::String::charToString(c);
      dash = false;
    } else {
      dash = true;
    }
    if (out.length() >= 48)
      break;
  }
  return out.isEmpty() ? juce::String("item") : out;
}

}  // namespace LibraryPaths
