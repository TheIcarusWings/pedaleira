#include "NamScenes.h"

#include "NamLibrary.h"
#include "pages/Tone3000Credit.h"

#include <algorithm>
#include <vector>

namespace pedaleira::ui::testbed {

namespace {

const char* const kLstm = R"({"version":"0.5.4","architecture":"LSTM","config":{"num_layers":1,"input_size":1,"hidden_size":16},"weights":[0.1,0.2,0.3]})";

// A tone as the live feed makes one from TONE3000's reply: the licence as
// TONE3000 sends it (an id), the image and avatar as URLs the fake answers.
Tone3000Tone tone(int id, const char* title, const char* creator, const char* gear, const char* format, int captures,
                  const char* licence, bool verified = false, bool image = true) {
  Tone3000Tone t;
  t.id = id;
  t.title = title;
  t.creator = creator;
  t.creatorVerified = verified;
  t.avatarUrl = "testbed:avatar/" + juce::String(creator);
  if (image)
    t.imageUrl = "testbed:art/" + juce::String(id) + "/" + gear;
  t.gear = gear;
  t.format = format;
  t.captures = captures;
  t.licence = licence;
  t.description = "Three British head captures, gain 3 to 7, recorded without a cab.";
  t.url = "https://www.tone3000.com/tones/" + juce::String(id);
  return t;
}

// One without an image and one without a licence, as TONE3000 can send them.
std::vector<Tone3000Tone> trending() {
  return {tone(101, "Harbor Crunch pack", "sample-user-a", "amp", "nam", 3, "cc-by", true),
          tone(102, "Coastline Clean", "sample-user-b", "full-rig", "nam", 3, "cc0"),
          tone(103, "Jade overdrive", "sample-user-c", "pedal", "nam", 2, "cc-by-nc"),
          tone(104, "4x12 Slate cab IRs", "sample-user-d", "cab", "ir", 6, "cc-by-sa"),
          tone(105, "Cobalt 100 Wall", "sample-user-a", "amp", "nam", 5, "cc-by", true),
          tone(106, "Plate and Spring spaces", "sample-user-e", "space", "ir", 4, "cc0", false, false),
          tone(107, "Maple Edge of Breakup", "sample-user-b", "full-rig", "nam", 3, ""),
          tone(108, "Germanium fuzz", "sample-user-f", "pedal", "nam", 2, "cc-by-nc-sa")};
}

// A flat colour from a name, the same every run.
juce::Colour hueOf(const juce::String& name, float saturation, float brightness) {
  return juce::Colour::fromHSV(static_cast<float>(static_cast<juce::uint32>(name.hashCode()) % 360u) / 360.0f, saturation, brightness, 1.0f);
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
  Downloaded out;
  // A capture downloaded before is in the library's TONE3000 folder: reused.
  if (setup.engine != nullptr) {
    const juce::String known = setup.engine->read("nam.downloaded", object({{"tone", tone.id}, {"capture", capture.id}}))["path"].toString();
    if (known.isNotEmpty() && juce::File(known).existsAsFile()) {
      out.file = juce::File(known);
      reply(std::move(out));
      return;
    }
  }
  ++downloads;
  const juce::File temp = juce::File::createTempFile(".nam");
  if (!setup.capture.existsAsFile() || !setup.capture.copyFileTo(temp)) {
    out.error = "The download could not be saved.";
  } else if (setup.engine == nullptr) {
    out.file = temp;
  } else {
    const EngineReply kept = setup.engine->run(
        "nam.keepDownload", object({{"file", temp.getFullPathName()},
                                    {"capture", capture.id},
                                    {"tone3000", Tone3000Credit::fromTone(tone, capture.name).toAttribution()}}));
    temp.deleteFile();
    if (kept.ok)
      out.file = juce::File(kept.value["path"].toString());
    else
      out.error = "The download could not be saved: " + kept.errorMessage;
  }
  reply(std::move(out));
}

void FakeTone3000Feed::image(const juce::String& url, bool avatar, std::function<void(juce::Image)> reply) {
  if (!url.startsWith("testbed:")) {
    reply({});
    return;
  }
  const int side = avatar ? 64 : 256;
  juce::Image out(juce::Image::ARGB, side, side, true);
  {
    juce::Graphics g(out);
    const auto box = out.getBounds().toFloat();
    const float unit = static_cast<float>(side);
    if (avatar) {
      // A head and shoulders over a flat colour.
      g.fillAll(hueOf(url, 0.45f, 0.55f));
      g.setColour(hueOf(url + "x", 0.25f, 0.92f));
      g.fillEllipse(box.getCentreX() - unit * 0.18f, unit * 0.2f, unit * 0.36f, unit * 0.36f);
      g.fillEllipse(box.getCentreX() - unit * 0.34f, unit * 0.62f, unit * 0.68f, unit * 0.6f);
    } else {
      // Cover art: a dark field, a disc and a band, coloured by the tone.
      g.fillAll(hueOf(url, 0.5f, 0.22f));
      g.setColour(hueOf(url + "a", 0.65f, 0.78f));
      g.fillEllipse(box.reduced(unit * 0.2f).translated(unit * 0.08f, -unit * 0.04f));
      g.setColour(hueOf(url + "b", 0.4f, 0.4f));
      g.fillRect(box.withTop(unit * 0.66f));
    }
  }
  reply(out);
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
    } else if (key == "t3kload") {
      // Loaded as if downloaded from TONE3000: the tone it came from goes with it.
      const juce::var attribution = juce::JSON::parse(
          R"({"id":101,"title":"Harbor Crunch pack","url":"https://www.tone3000.com/tones/101","gear":"amp","format":"nam",)"
          R"("license":"cc-by","image":"testbed:art/101/amp","capture":"Crunch, gain 5",)"
          R"("user":{"username":"sample-user-a","avatar_url":"testbed:avatar/sample-user-a","is_verified":true}})");
      const EngineReply reply = client.run("chain.addToneFromFiles", object({{"paths", juce::Array<juce::var>{scratch.getChildFile(parts[1]).getFullPathName()}},
                                                                             {"row", 0},
                                                                             {"slot", 0},
                                                                             {"tone3000", attribution}}));
      if (!reply.ok)
        return step + ": " + reply.errorMessage;
    } else if (key == "gone") {
      scratch.getChildFile(parts[1]).deleteFile();
    } else if (key == "root") {
      const EngineReply reply = client.run("roots.add", object({{"path", scratch.getChildFile(parts[1]).getFullPathName()}}));
      if (!reply.ok)
        return step + ": " + reply.errorMessage;
    } else if (key == "download") {
      // As if downloaded on the TONE3000 page: kept in the library's TONE3000 folder.
      static int nextId = 500;
      const juce::String fixture = arg(3).isNotEmpty() ? arg(3) : juce::String("a2-amp-test");
      const juce::String text = fixture == "old" ? juce::String(kLstm)
                                                  : namVariant(fixtures.getChildFile(fixture + ".nam"), arg(4), arg(5));
      const juce::File temp = scratch.getChildFile("download-" + juce::String(nextId) + ".nam");
      if (text.isEmpty() || !temp.replaceWithText(text))
        return "cannot write " + step;
      Tone3000Credit credit;
      credit.id = nextId;
      credit.title = arg(1);
      credit.capture = arg(2);
      credit.gear = "amp";
      credit.format = "nam";
      credit.licence = "cc-by";
      credit.username = "sample-user-a";
      const EngineReply reply = client.run("nam.keepDownload", object({{"file", temp.getFullPathName()},
                                                                       {"capture", nextId * 10},
                                                                       {"tone3000", credit.toAttribution()}}));
      ++nextId;
      if (!reply.ok)
        return step + ": " + reply.errorMessage;
    } else if (key == "pause") {
      juce::Thread::sleep(5);
    } else if (key.isNotEmpty()) {
      return "unknown step " + step;
    }
  }
  return {};
}

}  // namespace pedaleira::ui::testbed
