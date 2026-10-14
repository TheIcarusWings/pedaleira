#include "NamScenes.h"

#include "NamLibrary.h"

#include <algorithm>
#include <vector>

namespace pedaleira::ui::testbed {

namespace {

const char* const kLstm = R"({"version":"0.5.4","architecture":"LSTM","config":{"num_layers":1,"input_size":1,"hidden_size":16},"weights":[0.1,0.2,0.3]})";

Tone3000Tone tone(int id, const char* title, const char* creator, const char* gear, const char* format, int captures,
                  bool verified = false, bool favourite = false) {
  Tone3000Tone t;
  t.id = id;
  t.title = title;
  t.creator = creator;
  t.creatorVerified = verified;
  t.gear = gear;
  t.format = format;
  t.captures = captures;
  t.favourite = favourite;
  t.licence = "CC BY-NC 4.0";
  t.description = "Three British head captures, gain 3 to 7, recorded without a cab.";
  t.url = "https://www.tone3000.com/tones/" + juce::String(id);
  return t;
}

std::vector<Tone3000Tone> trending() {
  return {tone(101, "Harbor Crunch pack", "sample-user-a", "amp", "nam", 3, true),
          tone(102, "Coastline Clean", "sample-user-b", "full-rig", "nam", 3),
          tone(103, "Jade overdrive", "sample-user-c", "pedal", "nam", 2),
          tone(104, "4x12 Slate cab IRs", "sample-user-d", "cab", "ir", 6),
          tone(105, "Cobalt 100 Wall", "sample-user-a", "amp", "nam", 5, true),
          tone(106, "Plate and Spring spaces", "sample-user-e", "space", "ir", 4),
          tone(107, "Maple Edge of Breakup", "sample-user-b", "full-rig", "nam", 3),
          tone(108, "Germanium fuzz", "sample-user-f", "pedal", "nam", 2)};
}

}  // namespace

void FakeTone3000Feed::list(Tone3000List which, std::function<void(Listed)> reply) {
  ++lists;
  Listed out;
  if (setup.unreachable) {
    out.error = "listTrending failed: 0";
    reply(std::move(out));
    return;
  }
  if (tone3000ListNeedsAccount(which) && !setup.signedIn) {
    out.needsAccount = true;
    out.error = "Sign in to TONE3000 to see this list.";
    reply(std::move(out));
    return;
  }
  out.ok = true;
  out.tones = trending();
  if (which == Tone3000List::latest) {
    std::reverse(out.tones.begin(), out.tones.end());
    out.tones.resize(6);
  } else if (which == Tone3000List::favorites) {
    out.tones.resize(3);
  } else if (which == Tone3000List::downloaded) {
    out.tones = {out.tones[1], out.tones[3], out.tones[6]};
  } else if (which == Tone3000List::created) {
    out.tones.clear();
  }
  reply(std::move(out));
}

void FakeTone3000Feed::captures(const Tone3000Tone& tone, std::function<void(Captures)> reply) {
  Captures out;
  if (!setup.signedIn) {
    out.needsAccount = true;
    out.error = "Sign in to TONE3000 to add its captures.";
    reply(std::move(out));
    return;
  }
  out.ok = true;
  const bool ir = tone.format.equalsIgnoreCase("ir");
  for (int i = 0; i < tone.captures && i < 3; ++i) {
    static const char* const names[] = {"Clean edge, gain 3", "Crunch, gain 5", "Push, gain 7"};
    out.captures.push_back({tone.id * 10 + i, ir ? juce::String("Position ") + juce::String(i + 1) : juce::String(names[i]),
                            "https://example.invalid/model/" + juce::String(tone.id * 10 + i)});
  }
  reply(std::move(out));
}

void FakeTone3000Feed::download(const Tone3000Tone& tone, const Tone3000Capture& capture, std::function<void(Downloaded)> reply) {
  ++downloads;
  Downloaded out;
  const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("PedaleiraTestbed-TONE3000");
  folder.createDirectory();
  const juce::File file = folder.getChildFile(tone.title + " - " + capture.name + ".nam");
  if (!setup.capture.existsAsFile() || !setup.capture.copyFileTo(file))
    out.error = "The download could not be saved.";
  else
    out.file = file;
  reply(std::move(out));
}

juce::String namVariant(const juce::File& fixtureFile, const juce::String& make, const juce::String& model) {
  const juce::String text = fixtureFile.loadFileAsString();
  if (make.isEmpty() && model.isEmpty())
    return text;
  // The gear the file says it is made of, whatever the test capture had.
  juce::var parsed = juce::JSON::parse(text);
  auto* metadata = parsed["metadata"].getDynamicObject();
  if (metadata == nullptr)
    return {};
  metadata->setProperty("gear_type", "amp");
  if (make.isNotEmpty())
    metadata->setProperty("gear_make", make);
  if (model.isNotEmpty())
    metadata->setProperty("gear_model", model);
  return juce::JSON::toString(parsed, true);
}

juce::String prepareNam(EngineClient& client, const juce::File& scratch, const juce::File& fixtures, const juce::String& steps) {
  static std::vector<juce::File> tone3000Folders;
  for (const auto& step : juce::StringArray::fromTokens(steps, ";", "")) {
    const juce::StringArray parts = juce::StringArray::fromTokens(step, ":", "");
    const juce::String key = parts[0];
    // A dash stands for a field left out.
    const auto arg = [&parts](int i) { return parts[i] == "-" ? juce::String() : parts[i]; };
    if (key == "file") {
      const juce::File folder = scratch.getChildFile(arg(1));
      folder.createDirectory();
      const juce::String fixture = arg(3).isNotEmpty() ? arg(3) : juce::String("a2-amp-test");
      const juce::String text = fixture == "old" ? juce::String(kLstm)
                                                  : namVariant(fixtures.getChildFile(fixture + ".nam"), arg(4), arg(5));
      if (text.isEmpty() || !folder.getChildFile(arg(2) + ".nam").replaceWithText(text))
        return "cannot write " + step;
    } else if (key == "load") {
      const EngineReply reply = client.run("chain.addToneFromFiles", object({{"paths", juce::Array<juce::var>{scratch.getChildFile(parts[1]).getFullPathName()}}}));
      if (!reply.ok)
        return step + ": " + reply.errorMessage;
    } else if (key == "gone") {
      scratch.getChildFile(parts[1]).deleteFile();
    } else if (key == "root") {
      const EngineReply reply = client.run("roots.add", object({{"path", scratch.getChildFile(parts[1]).getFullPathName()}}));
      if (!reply.ok)
        return step + ": " + reply.errorMessage;
    } else if (key == "tone3000") {
      tone3000Folders = {scratch.getChildFile(parts[1])};
      NamLibrary::setTone3000FoldersForTesting(&tone3000Folders);
    } else if (key == "pause") {
      juce::Thread::sleep(5);
    } else if (key.isNotEmpty()) {
      return "unknown step " + step;
    }
  }
  return {};
}

}  // namespace pedaleira::ui::testbed
