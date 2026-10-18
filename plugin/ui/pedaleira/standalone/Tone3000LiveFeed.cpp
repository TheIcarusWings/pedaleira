#include "Tone3000LiveFeed.h"

#include "Processor.h"
#include "T3kConfig.h"
#include "model/Tone.h"
#include "model/ToneQuery.h"
#include "services/HttpClient.h"
#include "services/ImageLoader.h"
#include "services/Tone3000Client.h"
#include "services/UiPrefs.h"

#include <map>
#include <memory>

namespace pedaleira::ui {

namespace {

using t3k::ui::Result;

constexpr int kPageSize = 25;
constexpr int kMostCaptures = 100;
// The preference that says the TONE3000 splash was seen and continued.
const char* const kIntroSeen = "pedaleira.tone3000IntroSeen";

// Every host process shares the preferences file, so the sign-in of another
// build or editor is this one's too; UiPrefs merges writes under the lock.
struct PrefsFile {
  juce::InterProcessLock lock{"TONE3000.ui-preferences"};
  juce::PropertiesFile file{[this] {
    auto options = TONE3000Processor::uiPreferencesOptions();
    options.processLock = &lock;
    options.millisecondsBeforeSaving = -1;
    return options;
  }()};
};

Tone3000Tone toTone(const t3k::ui::Tone& tone) {
  Tone3000Tone out;
  out.id = tone.id;
  out.title = tone.title;
  out.description = tone.description;
  out.gear = tone.gear;
  out.format = tone.format;
  out.url = tone.url;
  out.captures = tone.catalogModelCount();
  out.favourite = tone.isFavorite.value_or(false);
  if (!tone.images.empty())
    out.imageUrl = tone.images.front();
  // TONE3000 shows a creator by username, with the avatar.
  if (tone.user) {
    out.creator = tone.user->username.isNotEmpty() ? tone.user->username : tone.user->name();
    out.creatorName = tone.user->displayName;
    out.creatorVerified = tone.user->isVerified;
    out.avatarUrl = tone.user->avatarUrl;
  }
  // The licence arrives as text or as an object with a name.
  const juce::var licence = tone.raw["license"].isVoid() ? tone.raw["licence"] : tone.raw["license"];
  out.licence = (licence.isObject() ? licence["name"] : licence).toString().trim();
  return out;
}

// A 401 or 403 means the list is the player's own and there is no session.
bool refusedForAccount(const juce::String& error) {
  return error.endsWith(": 401") || error.endsWith(": 403") || error == "token_refresh_failed" || error.contains("sign in");
}

class LiveFeed final : public Tone3000Feed {
public:
  LiveFeed()
      : prefs_(&prefsFile_->file, &prefsFile_->lock),
        client_(http_, prefs_, t3k::ui::config::kApiOrigin, t3k::ui::config::kPublishableKey) {}

  bool signedIn() const override { return client_.authenticated(); }

  juce::String userName() const override {
    if (!signedIn())
      return {};
    const juce::var cached = prefs_.getJson(t3k::ui::UiPrefs::kCachedUser);
    return cached.isObject() ? t3k::ui::User::parse(cached).name() : juce::String();
  }

  void list(Tone3000List which, std::function<void(Listed)> reply) override {
    if (tone3000ListNeedsAccount(which) && !signedIn()) {
      Listed refused;
      refused.needsAccount = true;
      refused.error = "Sign in to TONE3000 to see this list.";
      reply(std::move(refused));
      return;
    }
    const auto done = [reply](Result<juce::var> result) {
      Listed out;
      if (!result) {
        out.error = result.error;
        out.needsAccount = refusedForAccount(result.error);
        reply(std::move(out));
        return;
      }
      out.ok = true;
      if (const auto* rows = (*result)["data"].getArray())
        for (const auto& row : *rows)
          out.tones.push_back(toTone(t3k::ui::Tone::parse(row)));
      reply(std::move(out));
    };
    switch (which) {
      case Tone3000List::trending:
        client_.listTrending({}, done);
        break;
      case Tone3000List::latest:
        // Bearer when there is a session, anonymous without one.
        client_.fetchOptionalAuth("/api/v1/tones/latest?page=1&page_size=" + juce::String(kPageSize), {},
                                  [done](Result<t3k::ui::HttpResponse> r) {
                                    if (!r)
                                      return done(Result<juce::var>::fail(r.error));
                                    if (!r->ok())
                                      return done(Result<juce::var>::fail("listLatest failed: " + juce::String(r->status)));
                                    done(Result<juce::var>::ok(r->json()));
                                  });
        break;
      case Tone3000List::favorites:
      case Tone3000List::downloaded:
      case Tone3000List::created: {
        t3k::ui::ToneQuery query;
        query.profile = which == Tone3000List::favorites    ? t3k::ui::Profile::favorited
                        : which == Tone3000List::downloaded ? t3k::ui::Profile::downloaded
                                                            : t3k::ui::Profile::created;
        client_.listTones(query.requestPath(1, kPageSize), done);
        break;
      }
    }
  }

  void captures(const Tone3000Tone& tone, std::function<void(Captures)> reply) override {
    // The captures of a tone are behind the session (the free tier's model
    // list is the player's).
    if (!signedIn()) {
      Captures refused;
      refused.needsAccount = true;
      refused.error = "Sign in to TONE3000 to add its captures.";
      reply(std::move(refused));
      return;
    }
    const int architecture = tone.format.equalsIgnoreCase("nam") ? t3k::ui::config::kArchitecture : -1;
    client_.listModels(tone.id, kMostCaptures, architecture, [reply](Result<juce::var> result) {
      Captures out;
      if (!result) {
        out.error = result.error;
        out.needsAccount = refusedForAccount(result.error);
        reply(std::move(out));
        return;
      }
      out.ok = true;
      if (const auto* rows = (*result)["data"].getArray())
        for (const auto& row : *rows) {
          const t3k::ui::Model model = t3k::ui::Model::parse(row);
          if (model.modelUrl.isNotEmpty())
            out.captures.push_back({model.id, model.name, model.modelUrl});
        }
      reply(std::move(out));
    });
  }

  void download(const Tone3000Tone& tone, const Tone3000Capture& capture, std::function<void(Downloaded)> reply) override {
    // The HTTP layer hands a body back as text, which keeps a .nam (JSON) and
    // would damage a .wav; IRs are downloaded on tone3000.com.
    if (!tone.format.equalsIgnoreCase("nam")) {
      Downloaded refused;
      refused.error = "Impulse responses can not be added from here yet. Download it on tone3000.com and load the file.";
      reply(std::move(refused));
      return;
    }
    client_.getAccessToken([self = juce::WeakReference<LiveFeed>(this), tone, capture, reply](Result<juce::String> token) {
      if (self == nullptr)
        return;
      t3k::ui::HttpRequest request;
      request.url = juce::URL(capture.url);
      request.timeoutMs = 60000;
      if (token)
        request.headers.set("Authorization", "Bearer " + *token);
      self->http_.send(std::move(request), [tone, capture, reply](t3k::ui::HttpResponse response) {
        Downloaded out;
        if (!response.ok() || response.body.isEmpty()) {
          out.error = response.failed() ? juce::String("TONE3000 could not be reached.")
                                        : "TONE3000 refused the download (" + juce::String(response.status) + ").";
          reply(std::move(out));
          return;
        }
        // Named after the tone and the capture, so the recent list reads well.
        juce::String name = tone.title + " - " + capture.name;
        name = name.replaceCharacters("/\\:*?\"<>|", "---------").trim();
        const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Pedaleira-TONE3000");
        const juce::File file = folder.getChildFile(name + ".nam");
        if (!folder.createDirectory() || !file.replaceWithData(response.body.toRawUTF8(), response.body.getNumBytesAsUTF8())) {
          out.error = "The download could not be saved.";
          reply(std::move(out));
          return;
        }
        out.file = file;
        reply(std::move(out));
      });
    });
  }

  void image(const juce::String& url, bool avatar, std::function<void(juce::Image)> reply) override {
    if (url.isEmpty()) {
      reply({});
      return;
    }
    // The loader calls back while its request lives; each is kept until it has
    // answered, then let go after the call returns.
    const juce::uint64 key = ++nextImage_;
    auto& request = images_[key];
    request = std::make_unique<t3k::ui::ImageLoader::Request>();
    juce::WeakReference<LiveFeed> self(this);
    loader_.load(url, avatar ? t3k::ui::ImageLoader::kAvatarSide : t3k::ui::ImageLoader::kArtSide, *request,
                 [self, key, reply](const juce::Image& image) {
                   reply(image);
                   juce::MessageManager::callAsync([self, key] {
                     if (self != nullptr)
                       self->images_.erase(key);
                   });
                 });
  }

  bool introSeen() const override { return prefs_.getBool(kIntroSeen, false); }
  void markIntroSeen() override { prefs_.setBool(kIntroSeen, true); }

private:
  juce::SharedResourcePointer<PrefsFile> prefsFile_;
  t3k::ui::HttpClient http_;
  t3k::ui::UiPrefs prefs_;
  t3k::ui::Tone3000Client client_;
  t3k::ui::ImageLoader loader_;
  std::map<juce::uint64, std::unique_ptr<t3k::ui::ImageLoader::Request>> images_;
  juce::uint64 nextImage_ = 0;

  JUCE_DECLARE_WEAK_REFERENCEABLE(LiveFeed)
};

}  // namespace

std::unique_ptr<Tone3000Feed> makeTone3000LiveFeed() { return std::make_unique<LiveFeed>(); }

}  // namespace pedaleira::ui
