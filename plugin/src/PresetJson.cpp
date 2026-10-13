#include "PresetJson.h"

#include "PresetFile.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace t3k::presetjson {

namespace {

constexpr const char* kNodeKey = "node";
constexpr const char* kChildrenKey = "children";
constexpr const char* kChainKey = "chain";
constexpr const char* kParamsKey = "params";
constexpr const char* kAssetKey = "$asset";
constexpr const char* kBase64Key = "$base64";
constexpr const char* kInt64Key = "$int64";
constexpr const char* kIntKey = "$int";
constexpr const char* kDoubleKey = "$double";
constexpr const char* kUndefinedKey = "$undefined";

// A node without children that fits is written on one line: a scene's block
// state or a kind's parameter is one line of a diff, not five. Maps (the
// faceplate parameters, library.json) are never folded: there a line is one
// entry.
constexpr int kMaxInlineChars = 100;

bool isHex64(const juce::String& text) {
  return text.length() == 64 && text.containsOnly("0123456789abcdef");
}

// ####################
// CANONICAL WRITER
// ####################

void writeString(juce::MemoryOutputStream& out, const juce::String& text) {
  out << '"';
  for (auto t = text.getCharPointer(); !t.isEmpty();) {
    const juce::juce_wchar c = t.getAndAdvance();
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << juce::String::toHexString(static_cast<int>(c)).paddedLeft('0', 4);
        } else {
          // Everything else as UTF-8: a name with an accent reads as itself.
          char utf8[8]{};
          juce::CharPointer_UTF8 writer(utf8);
          writer.write(c);
          out.write(utf8, static_cast<size_t>(writer.getAddress() - utf8));
        }
        break;
    }
  }
  out << '"';
}

bool readsBackAs(const juce::String& text, double value) {
  auto pointer = text.getCharPointer();
  const double back = juce::CharacterFunctions::readDoubleValue(pointer);
  return std::memcmp(&back, &value, sizeof(double)) == 0;
}

// The shortest decimal text that reads back as exactly `value`, checked
// against the reader JSON::parse uses, and always recognisable as a double
// (JSON::parse makes an int of "1" and a double of "1.0"). Plain digits for
// the magnitudes a rig holds (380.0, not 3.8e+02), an exponent beyond them.
// Empty when no text does: that reader gives up below 1e-308, so the
// smallest denormals cannot be written as a number (see Encoder::value).
juce::String doubleText(double value) {
  if (!std::isfinite(value))
    return {};
  if (value == 0.0)
    return std::signbit(value) ? "-0.0" : "0.0";

  for (int digits = 1; digits <= 17; ++digits) {
    // "d.ddde+XX" in the classic locale: a host may have set another, and a
    // decimal comma is not JSON.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.setf(std::ios::scientific, std::ios::floatfield);
    stream.precision(digits - 1);
    stream << value;
    const juce::String scientific(stream.str());
    if (!readsBackAs(scientific, value))
      continue;

    const juce::String mantissa = scientific.upToFirstOccurrenceOf("e", false, false);
    const int exponent = scientific.fromFirstOccurrenceOf("e", false, false).getIntValue();
    const bool negative = mantissa.startsWithChar('-');
    const juce::String figures = mantissa.removeCharacters("-.");
    juce::String plain;
    if (exponent >= 0 && exponent < 17) {
      const juce::String padded = figures.paddedRight('0', exponent + 1);
      const juce::String fraction = padded.substring(exponent + 1);
      plain = padded.substring(0, exponent + 1) + "." + (fraction.isEmpty() ? "0" : fraction);
    } else if (exponent < 0 && exponent >= -6) {
      plain = "0." + juce::String::repeatedString("0", -exponent - 1) + figures;
    }
    if (plain.isNotEmpty()) {
      plain = (negative ? "-" : "") + plain;
      if (readsBackAs(plain, value))
        return plain;
    }
    return scientific;
  }
  return {};
}

bool isScalar(const juce::var& value) {
  return !value.isArray() && value.getDynamicObject() == nullptr;
}

void writeScalar(juce::MemoryOutputStream& out, const juce::var& value) {
  if (value.isString())
    writeString(out, value.toString());
  else if (value.isBool())
    out << (static_cast<bool>(value) ? "true" : "false");
  else if (value.isInt())
    out << juce::String(static_cast<int>(value));
  else if (value.isInt64())
    out << juce::String(static_cast<juce::int64>(value));
  else if (value.isDouble()) {
    // (A double with no text is wrapped before it gets here.)
    const juce::String text = doubleText(static_cast<double>(value));
    out << (text.isNotEmpty() ? text : juce::String("null"));
  } else {
    out << "null";
  }
}

void writeValue(juce::MemoryOutputStream& out, const juce::var& value, int depth);

// "{ "a": 1, "b": "c" }" when every value is a scalar, else empty.
juce::String inlineObject(const juce::DynamicObject& object) {
  juce::MemoryOutputStream line;
  line << "{ ";
  bool first = true;
  for (const auto& property : object.getProperties()) {
    if (!isScalar(property.value))
      return {};
    if (!first)
      line << ", ";
    first = false;
    writeString(line, property.name.toString());
    line << ": ";
    writeScalar(line, property.value);
  }
  line << " }";
  return line.toString();
}

void newLine(juce::MemoryOutputStream& out, int depth) {
  out << '\n';
  for (int i = 0; i < depth; ++i)
    out << "  ";
}

void writeValue(juce::MemoryOutputStream& out, const juce::var& value, int depth) {
  if (const auto* array = value.getArray()) {
    if (array->isEmpty()) {
      out << "[]";
      return;
    }
    out << '[';
    for (int i = 0; i < array->size(); ++i) {
      newLine(out, depth + 1);
      writeValue(out, array->getReference(i), depth + 1);
      if (i + 1 < array->size())
        out << ',';
    }
    newLine(out, depth);
    out << ']';
    return;
  }

  if (const auto* object = value.getDynamicObject()) {
    const auto& properties = object->getProperties();
    if (properties.isEmpty()) {
      out << "{}";
      return;
    }
    // A typed value (an asset reference, inline bytes) is one line whatever
    // its length: it is one value, and a line per key would bury the block
    // it belongs to.
    const juce::String firstKey = properties.getName(0).toString();
    const bool typed = firstKey.startsWithChar('$');
    if (typed || firstKey == kNodeKey) {
      if (const juce::String line = inlineObject(*object);
          line.isNotEmpty() && (typed || line.length() <= kMaxInlineChars)) {
        out << line;
        return;
      }
    }
    out << '{';
    for (int i = 0; i < properties.size(); ++i) {
      newLine(out, depth + 1);
      writeString(out, properties.getName(i).toString());
      out << ": ";
      writeValue(out, properties.getValueAt(i), depth + 1);
      if (i + 1 < properties.size())
        out << ',';
    }
    newLine(out, depth);
    out << '}';
    return;
  }

  writeScalar(out, value);
}

// ####################
// ASSET REFERENCES
// ####################

juce::var assetVar(const AssetRef& ref) {
  juce::DynamicObject::Ptr object = new juce::DynamicObject();
  object->setProperty(kAssetKey, ref.hash);
  object->setProperty("ext", ref.ext);
  object->setProperty("name", ref.name);
  // An int where it fits, so the text reads back as the same kind of number.
  if (ref.size <= std::numeric_limits<int>::max())
    object->setProperty("size", static_cast<int>(ref.size));
  else
    object->setProperty("size", ref.size);
  return juce::var(object.get());
}

std::optional<AssetRef> assetOf(const juce::var& value) {
  const auto* object = value.getDynamicObject();
  if (object == nullptr || !object->hasProperty(kAssetKey))
    return std::nullopt;
  AssetRef ref;
  ref.hash = object->getProperty(kAssetKey).toString().toLowerCase();
  ref.ext = AssetStore::cleanExtension(object->getProperty("ext").toString());
  ref.name = object->getProperty("name").toString();
  ref.size = static_cast<juce::int64>(object->getProperty("size"));
  return ref;
}

juce::String missingMarker(const AssetRef& ref) {
  return kMissingAssetPrefix + juce::JSON::toString(assetVar(ref), true);
}

// What to call the bytes of a model: the name its tone gives it and the
// extension its URL (or failing that its block type) says it has.
void describeModelAsset(const juce::ValueTree& cachedModel, AssetRef& ref) {
  const juce::ValueTree block = cachedModel.getParent().getParent();
  const juce::String type = block.getProperty("type").toString();
  ref.ext = type == "nam" ? "nam" : type == "ir" ? "wav" : "bin";

  // (toneJson came out of a file like everything else.)
  juce::var tone;
  parseJsonGuarded(block.getProperty("toneJson").toString(), tone);
  ref.name = tone["title"].toString();
  const int modelId = cachedModel.getProperty("modelId");
  if (const auto* models = tone["models"].getArray()) {
    for (const auto& model : *models) {
      if (static_cast<int>(model["id"]) != modelId)
        continue;
      if (model["name"].toString().isNotEmpty())
        ref.name = model["name"].toString();
      const juce::String url =
          model["model_url"].toString().upToFirstOccurrenceOf("?", false, false);
      const juce::String ext = url.fromLastOccurrenceOf("/", false, false).contains(".")
                                   ? url.fromLastOccurrenceOf(".", false, false).toLowerCase()
                                   : juce::String();
      if (ext == "nam" || ext == "wav" || ext == "aiff" || ext == "aif" || ext == "flac")
        ref.ext = ext;
      break;
    }
  }
  if (ref.name.isEmpty())
    ref.name = "model " + juce::String(modelId);
}

// ####################
// TREE -> JSON
// ####################

struct Encoder {
  const WriteOptions& options;
  juce::String error;

  static juce::String escapeKey(const juce::String& name, bool root) {
    const bool reserved = name == kNodeKey || name == kChildrenKey ||
                          (root && (name == "format" || name == "version" || name == kChainKey ||
                                    name == kParamsKey));
    return reserved || name.startsWithChar('$') ? "$" + name : name;
  }

  static juce::var typed(const char* key, const juce::var& value) {
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty(key, value);
    return juce::var(object.get());
  }

  juce::var binary(const juce::MemoryBlock& bytes, const juce::ValueTree& node,
                   const juce::Identifier& property) {
    const bool model = node.hasType("CachedModel") && property == juce::Identifier("data");
    if (options.assets == nullptr ||
        (!model && bytes.getSize() <= static_cast<size_t>(kInlineBinaryLimit)))
      return typed(kBase64Key, juce::Base64::toBase64(bytes.getData(), bytes.getSize()));

    AssetRef ref;
    if (model) {
      describeModelAsset(node, ref);
    } else {
      ref.ext = "bin";
      const juce::String owner = node.getProperty("name").toString();
      ref.name = (owner.isNotEmpty() ? owner : node.getType().toString()) + " " +
                 property.toString();
    }
    if (!options.assets->put(bytes.getData(), bytes.getSize(), ref.ext, ref)) {
      error = "could not store an asset in " + options.assets->directory().getFullPathName();
      return {};
    }
    return assetVar(ref);
  }

  juce::var value(const juce::var& v, const juce::ValueTree& node,
                  const juce::Identifier& property) {
    if (v.isString()) {
      // An asset that was missing when the tree was read goes back out as
      // the reference it came in as.
      if (const auto ref = missingAssetOf(v))
        return assetVar(*ref);
      return v;
    }
    if (v.isBool())
      return v;
    if (v.isInt()) {
      // The one int JSON::parse reads back as an int64.
      if (static_cast<int>(v) == std::numeric_limits<int>::min())
        return typed(kIntKey, v.toString());
      return v;
    }
    if (v.isInt64()) {
      // A small int64 would read back as an int.
      const auto number = static_cast<juce::int64>(v);
      const bool readsAsInt64 = number != std::numeric_limits<juce::int64>::min() &&
                                ((number < 0 ? -number : number) >> 31) != 0;
      return readsAsInt64 ? v : typed(kInt64Key, v.toString());
    }
    if (v.isDouble()) {
      const auto number = static_cast<double>(v);
      if (std::isnan(number))
        return typed(kDoubleKey, "nan");
      if (std::isinf(number))
        return typed(kDoubleKey, number > 0.0 ? "inf" : "-inf");
      // The few doubles no decimal text brings back: by their bits.
      if (doubleText(number).isEmpty()) {
        juce::uint64 bits = 0;
        std::memcpy(&bits, &number, sizeof(bits));
        return typed(kDoubleKey, "0x" + juce::String::toHexString(static_cast<juce::int64>(bits))
                                            .paddedLeft('0', 16));
      }
      return v;
    }
    if (const auto* bytes = v.getBinaryData())
      return binary(*bytes, node, property);
    if (const auto* array = v.getArray()) {
      juce::Array<juce::var> items;
      for (const auto& item : *array)
        items.add(value(item, node, property));
      return items;
    }
    if (v.isUndefined())
      return typed(kUndefinedKey, true);
    return {};  // void, and what a saved tree cannot hold anyway
  }

  void properties(juce::DynamicObject& object, const juce::ValueTree& node, bool root) {
    for (int i = 0; i < node.getNumProperties(); ++i) {
      const juce::Identifier name = node.getPropertyName(i);
      object.setProperty(escapeKey(name.toString(), root), value(node.getProperty(name), node, name));
    }
  }

  juce::var children(const juce::ValueTree& node) {
    juce::Array<juce::var> list;
    for (int i = 0; i < node.getNumChildren(); ++i)
      list.add(encode(node.getChild(i), true));
    return list;
  }

  juce::var encode(const juce::ValueTree& node, bool withType) {
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    if (withType)
      object->setProperty(kNodeKey, node.getType().toString());
    properties(*object, node, false);
    if (node.getNumChildren() > 0)
      object->setProperty(kChildrenKey, children(node));
    return juce::var(object.get());
  }
};

// The shape savePreset writes: nothing on the node, and every child a
// Param of an id and a finite double. Only that shape can be a plain map and
// come back as the same tree.
bool isPlainParams(const juce::ValueTree& params) {
  if (!params.hasType("Params") || params.getNumProperties() != 0)
    return false;
  std::set<juce::String> seen;
  for (int i = 0; i < params.getNumChildren(); ++i) {
    const juce::ValueTree param = params.getChild(i);
    if (!param.hasType("Param") || param.getNumChildren() != 0 || param.getNumProperties() != 2 ||
        param.getPropertyName(0) != juce::Identifier("id") ||
        param.getPropertyName(1) != juce::Identifier("value"))
      return false;
    const juce::var& id = param.getProperty("id");
    const juce::var& value = param.getProperty("value");
    if (!id.isString() || id.toString().isEmpty() || !value.isDouble() ||
        !std::isfinite(static_cast<double>(value)) || !seen.insert(id.toString()).second)
      return false;
  }
  return true;
}

// ####################
// JSON -> TREE
// ####################

struct Decoder {
  const ReadOptions& options;
  juce::String error;
  std::vector<AssetRef> missing;

  static juce::String unescapeKey(const juce::String& key) {
    return key.startsWithChar('$') ? key.substring(1) : key;
  }

  juce::var asset(const AssetRef& ref) {
    if (isHex64(ref.hash)) {
      for (const AssetStore* store : options.assets) {
        juce::MemoryBlock bytes;
        if (store != nullptr && store->get(ref, bytes) == AssetStore::Status::ok)
          return juce::var(std::move(bytes));
      }
    }
    missing.push_back(ref);
    return missingMarker(ref);
  }

  juce::var value(const juce::var& v) {
    if (const auto* array = v.getArray()) {
      juce::Array<juce::var> items;
      for (const auto& item : *array)
        items.add(value(item));
      return items;
    }
    const auto* object = v.getDynamicObject();
    if (object == nullptr)
      return v;

    if (const auto ref = assetOf(v))
      return asset(*ref);
    if (object->hasProperty(kBase64Key)) {
      juce::MemoryOutputStream bytes;
      if (!juce::Base64::convertFromBase64(bytes, object->getProperty(kBase64Key).toString())) {
        error = "a $base64 value is not base64";
        return {};
      }
      return juce::var(juce::MemoryBlock(bytes.getData(), bytes.getDataSize()));
    }
    if (object->hasProperty(kInt64Key))
      return juce::var(object->getProperty(kInt64Key).toString().getLargeIntValue());
    if (object->hasProperty(kIntKey))
      return juce::var(object->getProperty(kIntKey).toString().getIntValue());
    if (object->hasProperty(kDoubleKey)) {
      const juce::String text = object->getProperty(kDoubleKey).toString();
      if (text.startsWith("0x")) {
        const auto bits = static_cast<juce::uint64>(text.substring(2).getHexValue64());
        double number = 0.0;
        std::memcpy(&number, &bits, sizeof(number));
        return juce::var(number);
      }
      const double infinity = std::numeric_limits<double>::infinity();
      return juce::var(text == "inf" ? infinity
                                     : text == "-inf" ? -infinity
                                                      : std::numeric_limits<double>::quiet_NaN());
    }
    if (object->hasProperty(kUndefinedKey))
      return juce::var::undefined();
    error = "a value is an object that is not one of the typed values";
    return {};
  }

  // Fills `node` from `object`: properties in order, then the children.
  void fill(juce::ValueTree& node, const juce::DynamicObject& object) {
    for (const auto& property : object.getProperties()) {
      const juce::String key = property.name.toString();
      if (key == kNodeKey)
        continue;
      if (key == kChildrenKey) {
        appendChildren(node, property.value);
        continue;
      }
      const juce::String name = unescapeKey(key);
      if (name.isEmpty()) {
        error = "a property has no name";
        return;
      }
      node.setProperty(juce::Identifier(name), value(property.value), nullptr);
    }
  }

  void appendChildren(juce::ValueTree& node, const juce::var& list) {
    const auto* items = list.getArray();
    if (items == nullptr) {
      error = "\"children\" must be an array";
      return;
    }
    for (const auto& item : *items) {
      const auto* object = item.getDynamicObject();
      const juce::String type = object != nullptr ? object->getProperty(kNodeKey).toString()
                                                  : juce::String();
      if (type.isEmpty()) {
        error = "every child needs a \"node\" type";
        return;
      }
      juce::ValueTree child{juce::Identifier(type)};
      fill(child, *object);
      node.appendChild(child, nullptr);
    }
  }
};

juce::String formatProblem(const juce::var& parsed) {
  if (parsed.getDynamicObject() == nullptr)
    return "not a JSON object";
  if (parsed["format"].toString() != kFormat)
    return "not a " + juce::String(kFormat) + " file";
  if (static_cast<int>(parsed["version"]) > kVersion)
    return "written by a newer version (" + parsed["version"].toString() + ")";
  return {};
}

void collectAssets(const juce::var& value, std::vector<AssetRef>& out) {
  if (const auto* array = value.getArray()) {
    for (const auto& item : *array)
      collectAssets(item, out);
    return;
  }
  const auto* object = value.getDynamicObject();
  if (object == nullptr)
    return;
  if (const auto ref = assetOf(value)) {
    out.push_back(*ref);
    return;
  }
  for (const auto& property : object->getProperties())
    collectAssets(property.value, out);
}

std::atomic<FileFault> fileFault{FileFault::none};

}  // namespace

// ####################
// FILES
// ####################

FileFault setFileFaultForTesting(FileFault fault) { return fileFault.exchange(fault); }
FileFault fileFaultForTesting() { return fileFault.load(); }

juce::Result writeFileAtomically(const juce::File& target, const void* data, size_t size) {
  const auto cannot = [&target](const juce::String& why) {
    return juce::Result::fail("cannot write " + target.getFullPathName() + ": " + why);
  };
  // Beside the target (the same volume, so the last step is a rename) and
  // under an extension nothing takes for a preset or an asset: a crash or a
  // full disk never leaves half a file under a real name.
  const juce::File temp = target.getSiblingFile(
      target.getFileName() + "." +
      juce::String::toHexString(juce::Random::getSystemRandom().nextInt()) + ".tmp");
  juce::String failure;
  {
    juce::FileOutputStream out(temp);
    if (!out.openedOk())
      return cannot("the folder cannot be written to");
    const bool written = fileFault.load() != FileFault::write &&
                         (size == 0 || out.write(data, size));
    out.flush();
    if (out.getStatus().failed())
      failure = out.getStatus().getErrorMessage();
    else if (!written)
      failure = "not every byte could be written";
  }
  // (The stream is closed by now: Windows does not delete an open file.)
  if (failure.isNotEmpty()) {
    temp.deleteFile();
    return cannot(failure);
  }
  // replaceFileIn, not moveFileTo: moveFileTo deletes the target and then
  // renames, so a rename that fails has already cost the old file. This is
  // one rename over the target (ReplaceFile on Windows): the name always
  // holds either the old bytes or the new.
  if (fileFault.load() != FileFault::replace && temp.replaceFileIn(target))
    return juce::Result::ok();
  temp.deleteFile();
  return cannot("the file could not be replaced");
}

juce::Result writeFileAtomically(const juce::File& target, const juce::String& text) {
  return writeFileAtomically(target, text.toRawUTF8(), text.getNumBytesAsUTF8());
}

juce::Result parseJsonGuarded(const juce::String& text, juce::var& out) {
  return parseJsonGuarded(text, out, kMaxJsonDepth);
}

juce::Result parseJsonGuarded(const juce::String& text, juce::var& out, int maxDepth) {
  out = juce::var();
  // Depth of [ and { outside strings, before the parser sees any of it.
  int depth = 0;
  bool inString = false;
  for (const char* c = text.toRawUTF8(); *c != 0; ++c) {
    if (inString) {
      if (*c == '\\' && c[1] != 0)
        ++c;
      else if (*c == '"')
        inString = false;
    } else if (*c == '"') {
      inString = true;
    } else if (*c == '[' || *c == '{') {
      if (++depth > maxDepth)
        return juce::Result::fail("nested deeper than " + juce::String(maxDepth) + " levels");
    } else if (*c == ']' || *c == '}') {
      --depth;
    }
  }
  return juce::JSON::parse(text, out);
}

juce::Result readTextFile(const juce::File& file, juce::int64 maxBytes, juce::String& text) {
  text.clear();
  if (!file.existsAsFile())
    return juce::Result::fail("the file is gone");
  if (file.getSize() > maxBytes)
    return juce::Result::fail("larger than " + juce::File::descriptionOfSizeInBytes(maxBytes));
  juce::FileInputStream in(file);
  if (!in.openedOk())
    return juce::Result::fail("the file cannot be read");
  // The size is asked again of the open file: it may have grown since.
  if (in.getTotalLength() > maxBytes)
    return juce::Result::fail("larger than " + juce::File::descriptionOfSizeInBytes(maxBytes));
  juce::MemoryBlock bytes;
  in.readIntoMemoryBlock(bytes, static_cast<juce::int64>(maxBytes) + 1);
  if (static_cast<juce::int64>(bytes.getSize()) > maxBytes)
    return juce::Result::fail("larger than " + juce::File::descriptionOfSizeInBytes(maxBytes));
  // As loadFileAsString reads it: UTF-8, or UTF-16 with a byte order mark.
  text = juce::String::createStringFromData(bytes.getData(), static_cast<int>(bytes.getSize()));
  return juce::Result::ok();
}

juce::Result readJsonFile(const juce::File& file, juce::int64 maxBytes, juce::var& out) {
  out = juce::var();
  juce::String text;
  if (const juce::Result read = readTextFile(file, maxBytes, text); read.failed())
    return read;
  if (const juce::Result parsed = parseJsonGuarded(text, out); parsed.failed())
    return juce::Result::fail("not valid JSON: " + parsed.getErrorMessage());
  return juce::Result::ok();
}

// ####################
// ASSET STORE
// ####################

juce::String AssetStore::cleanExtension(const juce::String& ext) {
  const juce::String lower = ext.toLowerCase();
  if (lower.isEmpty() || lower.length() > 8 ||
      !lower.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789"))
    return "bin";
  return lower;
}

juce::String AssetStore::hashOf(const void* data, size_t size) {
  return juce::SHA256(data, size).toHexString().removeCharacters(" ").toLowerCase();
}

juce::File AssetStore::fileFor(const juce::String& hash, const juce::String& ext) const {
  // The hash comes out of a file anyone may have edited: only a real hash
  // may become a path.
  if (dir == juce::File() || !isHex64(hash))
    return {};
  return dir.getChildFile(hash + "." + cleanExtension(ext));
}

bool AssetStore::isVerified(const juce::File& file) const {
  const juce::ScopedLock lock(verifiedLock);
  const auto it = verified.find(file.getFileName());
  return it != verified.end() &&
         it->second.modificationMs == file.getLastModificationTime().toMilliseconds() &&
         it->second.size == file.getSize();
}

void AssetStore::noteVerified(const juce::File& file) const {
  const juce::ScopedLock lock(verifiedLock);
  verified[file.getFileName()] = {file.getLastModificationTime().toMilliseconds(),
                                  file.getSize()};
}

bool AssetStore::holds(const juce::File& file, const AssetRef& ref) const {
  if (!file.existsAsFile() || file.getSize() != ref.size)
    return false;
  if (isVerified(file))
    return true;
  juce::MemoryBlock bytes;
  if (!file.loadFileAsData(bytes) || hashOf(bytes.getData(), bytes.getSize()) != ref.hash)
    return false;
  noteVerified(file);
  return true;
}

bool AssetStore::put(const void* data, size_t size, const juce::String& ext, AssetRef& ref) const {
  ref.hash = hashOf(data, size);
  ref.ext = cleanExtension(ext);
  ref.size = static_cast<juce::int64>(size);
  const juce::File file = fileFor(ref.hash, ref.ext);
  if (file == juce::File())
    return false;
  // There already: the name is the content, so there is nothing to write.
  // Only once the content has been seen to be that, though: a file of the
  // right size under the right name may still be a damaged one (a bad disk,
  // an interrupted copy), and every preset saved from here on would point
  // at it.
  if (holds(file, ref))
    return true;
  if (file.existsAsFile())
    juce::Logger::writeToLog("[Library] The asset " + file.getFileName() +
                             " is not the bytes its name says; writing it again");
  if (!dir.createDirectory())
    return false;

  if (const juce::Result written = writeFileAtomically(file, data, size); written.failed()) {
    juce::Logger::writeToLog("[Library] " + written.getErrorMessage());
    // Another instance may have put the same bytes there in the meantime.
    return holds(file, ref);
  }
  noteVerified(file);
  return true;
}

AssetStore::Status AssetStore::get(const AssetRef& ref, juce::MemoryBlock& out) const {
  const juce::File file = fileFor(ref.hash, ref.ext);
  if (file == juce::File() || !file.existsAsFile())
    return Status::missing;
  const juce::int64 size = file.getSize();
  if (ref.size > 0 && size != ref.size)
    return Status::corrupt;
  out.reset();
  if (!file.loadFileAsData(out) || static_cast<juce::int64>(out.getSize()) != size)
    return Status::missing;

  const juce::int64 modificationMs = file.getLastModificationTime().toMilliseconds();
  {
    const juce::ScopedLock lock(verifiedLock);
    const auto it = verified.find(file.getFileName());
    if (it != verified.end() && it->second.modificationMs == modificationMs &&
        it->second.size == size)
      return Status::ok;
  }
  if (hashOf(out.getData(), out.getSize()) != ref.hash) {
    out.reset();
    return Status::corrupt;
  }
  const juce::ScopedLock lock(verifiedLock);
  verified[file.getFileName()] = {modificationMs, size};
  return Status::ok;
}

bool AssetStore::adopt(const AssetStore& other, const AssetRef& ref) const {
  const juce::File target = fileFor(ref.hash, ref.ext);
  if (target == juce::File())
    return false;
  if (target.existsAsFile())
    return true;
  juce::MemoryBlock bytes;
  if (other.get(ref, bytes) != Status::ok)
    return false;
  AssetRef stored;
  return put(bytes.getData(), bytes.getSize(), ref.ext, stored) && stored.hash == ref.hash;
}

// ####################
// MISSING ASSETS
// ####################

bool isMissingAsset(const juce::var& value) {
  return value.isString() && value.toString().startsWith(kMissingAssetPrefix);
}

std::optional<AssetRef> missingAssetOf(const juce::var& value) {
  if (!isMissingAsset(value))
    return std::nullopt;
  juce::var parsed;
  if (parseJsonGuarded(value.toString().substring(static_cast<int>(std::strlen(kMissingAssetPrefix))),
                       parsed)
          .failed())
    return std::nullopt;
  return assetOf(parsed);
}

bool hasMissingAsset(const juce::ValueTree& node) {
  for (int i = 0; i < node.getNumProperties(); ++i)
    if (isMissingAsset(node.getProperty(node.getPropertyName(i))))
      return true;
  for (int i = 0; i < node.getNumChildren(); ++i)
    if (hasMissingAsset(node.getChild(i)))
      return true;
  return false;
}

// ####################
// PUBLIC CONVERSIONS
// ####################

juce::String writeCanonical(const juce::var& value) {
  juce::MemoryOutputStream out;
  writeValue(out, value, 0);
  out << '\n';
  return out.toString();
}

juce::String toJson(const juce::ValueTree& preset, const WriteOptions& options,
                    juce::String& error) {
  if (!preset.hasType(presetfile::kTag)) {
    error = "not a preset tree";
    return {};
  }

  Encoder encoder{options, {}};
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("format", kFormat);
  root->setProperty("version", kVersion);
  encoder.properties(*root, preset, true);

  if (preset.getNumChildren() == 2 && preset.getChild(0).hasType("ChainSnapshot") &&
      isPlainParams(preset.getChild(1))) {
    root->setProperty(kChainKey, encoder.encode(preset.getChild(0), false));
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    const juce::ValueTree paramsNode = preset.getChild(1);
    for (int i = 0; i < paramsNode.getNumChildren(); ++i)
      params->setProperty(juce::Identifier(paramsNode.getChild(i).getProperty("id").toString()),
                          paramsNode.getChild(i).getProperty("value"));
    root->setProperty(kParamsKey, juce::var(params.get()));
  } else if (preset.getNumChildren() > 0) {
    // Not the shape this build saves: written node by node, so it still
    // comes back as it went in.
    root->setProperty(kChildrenKey, encoder.children(preset));
  }

  if (encoder.error.isNotEmpty()) {
    error = encoder.error;
    return {};
  }
  return writeCanonical(juce::var(root.get()));
}

ReadResult fromJson(const juce::String& text, const ReadOptions& options) {
  ReadResult result;
  juce::var parsed;
  if (const juce::Result status = parseJsonGuarded(text, parsed); status.failed()) {
    result.error = "not valid JSON: " + status.getErrorMessage();
    return result;
  }
  if (const juce::String problem = formatProblem(parsed); problem.isNotEmpty()) {
    result.error = problem;
    return result;
  }

  Decoder decoder{options, {}, {}};
  juce::ValueTree preset{juce::Identifier(presetfile::kTag)};
  for (const auto& property : parsed.getDynamicObject()->getProperties()) {
    const juce::String key = property.name.toString();
    if (key == "format" || key == "version" || key == kNodeKey)
      continue;
    if (key == kChainKey) {
      const auto* chain = property.value.getDynamicObject();
      if (chain == nullptr) {
        decoder.error = "\"chain\" must be an object";
        break;
      }
      juce::ValueTree snapshot("ChainSnapshot");
      decoder.fill(snapshot, *chain);
      preset.appendChild(snapshot, nullptr);
    } else if (key == kParamsKey) {
      const auto* map = property.value.getDynamicObject();
      if (map == nullptr) {
        decoder.error = "\"params\" must be an object";
        break;
      }
      juce::ValueTree params("Params");
      for (const auto& entry : map->getProperties()) {
        juce::ValueTree param("Param");
        param.setProperty("id", entry.name.toString(), nullptr);
        // Always a double in the tree, also when someone typed "3".
        param.setProperty("value", static_cast<double>(entry.value), nullptr);
        params.appendChild(param, nullptr);
      }
      preset.appendChild(params, nullptr);
    } else if (key == kChildrenKey) {
      decoder.appendChildren(preset, property.value);
    } else {
      preset.setProperty(juce::Identifier(Decoder::unescapeKey(key)),
                         decoder.value(property.value), nullptr);
    }
    if (decoder.error.isNotEmpty())
      break;
  }

  if (decoder.error.isNotEmpty()) {
    result.error = decoder.error;
    return result;
  }
  result.preset = preset;
  result.missing = std::move(decoder.missing);
  return result;
}

Meta metaOf(const juce::var& parsed) {
  Meta meta;
  meta.problem = formatProblem(parsed);
  if (meta.problem.isNotEmpty())
    return meta;
  meta.valid = true;
  meta.id = parsed["id"].toString().trim();
  meta.name = parsed["name"].toString();
  if (const juce::var tempo = parsed["tempo"]; tempo.isDouble() || tempo.isInt())
    meta.tempo = static_cast<double>(tempo);
  if (const auto* tags = parsed["tags"].getArray()) {
    for (const auto& tag : *tags)
      if (tag.toString().trim().isNotEmpty())
        meta.tags.add(tag.toString().trim());
  } else if (parsed["tags"].isString()) {
    meta.tags = juce::StringArray::fromTokens(parsed["tags"].toString(), ",", "");
    meta.tags.trim();
    meta.tags.removeEmptyStrings();
  }

  // The chain is "chain", or a ChainSnapshot among "children".
  juce::var chain = parsed[kChainKey];
  if (chain.isVoid())
    if (const auto* children = parsed[kChildrenKey].getArray())
      for (const auto& child : *children)
        if (child[kNodeKey].toString() == "ChainSnapshot")
          chain = child;
  if (const auto* nodes = chain[kChildrenKey].getArray()) {
    for (const auto& node : *nodes) {
      if (node[kNodeKey].toString() != "Scenes")
        continue;
      const int active = node["active"];
      int used = 0;
      if (const auto* scenes = node[kChildrenKey].getArray()) {
        for (const auto& scene : *scenes) {
          if (scene[kNodeKey].toString() != "Scene")
            continue;
          bool hasBlock = false;
          if (const auto* entries = scene[kChildrenKey].getArray())
            for (const auto& entry : *entries)
              hasBlock = hasBlock || entry[kNodeKey].toString() == "Block";
          if (hasBlock || scene["name"].toString().isNotEmpty() ||
              static_cast<int>(scene["index"]) == active)
            ++used;
        }
      }
      meta.sceneCount = node["count"].isVoid() ? juce::jmax(1, used)
                                               : juce::jlimit(1, 8, static_cast<int>(node["count"]));
    }
  }
  return meta;
}

Meta readMeta(const juce::String& text) {
  juce::var parsed;
  if (const juce::Result status = parseJsonGuarded(text, parsed); status.failed()) {
    Meta meta;
    meta.problem = "not valid JSON: " + status.getErrorMessage();
    return meta;
  }
  return metaOf(parsed);
}

Meta readMetaFile(const juce::File& file) {
  juce::String text;
  if (const juce::Result read = readTextFile(file, kMaxPresetFileBytes, text); read.failed()) {
    Meta meta;
    meta.problem = read.getErrorMessage();
    return meta;
  }
  return readMeta(text);
}

ReadResult fromJsonFile(const juce::File& file, const ReadOptions& options) {
  juce::String text;
  if (const juce::Result read = readTextFile(file, kMaxPresetFileBytes, text); read.failed()) {
    ReadResult result;
    result.error = read.getErrorMessage();
    return result;
  }
  return fromJson(text, options);
}

std::vector<AssetRef> referencedAssets(const juce::var& parsed) {
  std::vector<AssetRef> out;
  collectAssets(parsed, out);
  return out;
}

}  // namespace t3k::presetjson
