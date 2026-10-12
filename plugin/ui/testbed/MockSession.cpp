#include "MockSession.h"

#include <juce_events/juce_events.h>

#include <algorithm>

namespace t3k::ui::testbed {

MockSession::MockSession(const juce::var& scenario, const juce::var& fixtures)
    : authenticated_(static_cast<bool>(scenario.getProperty("auth", false))),
      offline_(static_cast<bool>(scenario.getProperty("offlineAfterLoad", false))),
      gatedPage_(fixtures["gatedPage"]),
      user_(fixtures["user"]),
      apiTones_(fixtures["apiTones"]),
      api_(scenario["api"]) {
  const auto query = scenario["query"].toString();
  if (query.contains("t3k-nav-error")) {
    flow_.phase = AuthFlow::Phase::error;
    flow_.error = "Could not reach TONE3000. Check your internet connection and try again.";
  } else if (query.contains("code=")) {
    flow_.phase = AuthFlow::Phase::returning;
  }
  // The suite's browserLanding: a canceled browse-intent return opens the
  // in-plugin browser on arrival. Posted: the root subscribes after we exist.
  const auto intent = scenario["sessionStorage"]["t3k.loginIntent"].toString();
  if (query.contains("canceled=true") && intent == "browse")
    juce::MessageManager::callAsync([self = juce::WeakReference<MockSession>(this)] {
      if (self != nullptr && self->onAuthenticated) self->onAuthenticated();
    });
}

MockSession::~MockSession() { masterReference.clear(); }

void MockSession::setFlow(AuthFlow::Phase phase, juce::String error) {
  flow_ = {};
  flow_.phase = phase;
  flow_.error = std::move(error);
  notifyAuthFlowChanged();
}

void MockSession::clearAuthError() { setFlow(AuthFlow::Phase::idle); }

std::optional<User> MockSession::user() const {
  if (!authenticated_ || !user_.isObject()) return std::nullopt;
  return User::parse(user_);
}

juce::var MockSession::apiTone(int toneId) const {
  const auto* tones = apiTones_.getArray();
  if (tones == nullptr || tones->isEmpty()) return {};
  for (const auto& t : *tones)
    if (static_cast<int>(t["id"]) == toneId) return t;
  return tones->getFirst();
}

juce::var MockSession::override(const char* group) const {
  return api_.isObject() ? api_[group] : juce::var();
}

template <typename T>
void MockSession::answer(const char* group, Reply<T> reply, std::function<Result<T>()> fallback) {
  const auto spec = override(group);
  if (spec.isString() && spec.toString() == "stall") return;  // never replies
  auto result = spec.isString() && spec.toString() == "error"
                    ? Result<T>::fail("Request failed (500).")
                    : fallback();
  juce::MessageManager::callAsync([cb = std::move(reply), r = std::move(result)]() mutable {
    cb(std::move(r));
  });
}

void MockSession::getTone(int toneId, Reply<Tone> reply) {
  answer<Tone>("tone", std::move(reply), [this, toneId] {
    const auto spec = override("tone");
    return Result<Tone>::ok(Tone::parse(spec.isObject() ? spec : apiTone(toneId)));
  });
}

void MockSession::listToneModels(int toneId, const juce::String&, Reply<std::vector<Model>> reply) {
  answer<std::vector<Model>>("models", std::move(reply), [this, toneId] {
    std::vector<Model> models;
    const auto spec = override("models");
    if (spec.isObject()) {
      if (const auto* rows = spec["data"].getArray())
        for (const auto& m : *rows) models.push_back(Model::parse(m));
      return Result<std::vector<Model>>::ok(std::move(models));
    }
    // Pad the picker with the active tone's real sibling models, like the
    // suite's /models handler.
    const auto tone = apiTone(toneId);
    const auto* own = tone["models"].getArray();
    if (own == nullptr || own->isEmpty()) return Result<std::vector<Model>>::ok(std::move(models));
    for (const auto& m : *own) models.push_back(Model::parse(m));
    const auto base = own->getFirst();
    const char* extra[] = {"AC30-6 TB Brl 2", "AC30-6 TB Brl 4", "AC30-6 TB Nrm 2", "AC30-6 TB Nrm 3"};
    int i = 1;
    for (const auto* name : extra) {
      auto copy = juce::var(base.getDynamicObject()->clone().release());
      copy.getDynamicObject()->setProperty("id", static_cast<int>(base["id"]) + i++);
      copy.getDynamicObject()->setProperty("name", name);
      models.push_back(Model::parse(copy));
    }
    return Result<std::vector<Model>>::ok(std::move(models));
  });
}

void MockSession::listTones(const ToneQuery& query, int page, int pageSize, Reply<TonePage> reply) {
  requests.push_back(query.requestPath(page, pageSize));
  if (query.isTrending()) {
    // The default view: the same ten-tone feed the signed-out preview shows.
    feed(query.gear, [cb = std::move(reply)](Result<std::vector<Tone>> r) {
      if (!r) return cb(Result<TonePage>::fail(r.error));
      TonePage page;
      page.data = std::move(*r.value);
      cb(Result<TonePage>::ok(std::move(page)));
    });
    return;
  }
  // The suite's fixed gated page, whatever the page asked for, narrowed by
  // the gear the stream endpoints take.
  answer<TonePage>("gated", std::move(reply), [this, query] {
    const auto spec = override("gated");
    auto gated = TonePage::parse(spec.isObject() ? spec : gatedPage_);
    if (query.gear.isNotEmpty()) {
      std::erase_if(gated.data, [&](const Tone& tone) { return !tone.gear.equalsIgnoreCase(query.gear); });
      gated.page = 1;
      gated.totalPages = 1;
    }
    return Result<TonePage>::ok(std::move(gated));
  });
}

void MockSession::listTrending(const juce::String& gear, Reply<std::vector<Tone>> reply) {
  requests.push_back(ToneQuery{gear, Profile::none}.requestPath(1, 0));
  feed(gear, std::move(reply));
}

void MockSession::feed(const juce::String& gear, Reply<std::vector<Tone>> reply) {
  answer<std::vector<Tone>>("trending", std::move(reply), [this, gear] {
    constexpr size_t kFeedSize = 10;  // the endpoint's fixed cap
    std::vector<Tone> out;
    const auto spec = override("trending");
    const auto* rows = spec.isObject() ? spec["data"].getArray() : apiTones_.getArray();
    if (rows == nullptr) return Result<std::vector<Tone>>::ok(std::move(out));
    for (const auto& row : *rows) {
      auto tone = Tone::parse(row);
      if (gear.isNotEmpty() && !tone.gear.equalsIgnoreCase(gear)) continue;
      out.push_back(std::move(tone));
      if (out.size() == kFeedSize) break;
    }
    return Result<std::vector<Tone>>::ok(std::move(out));
  });
}

void MockSession::selectTone(int toneId, Done done) {
  getTone(toneId, [self = juce::WeakReference<MockSession>(this), fin = std::move(done)](Result<Tone> tone) {
    if (self == nullptr) return;
    if (!tone) return fin(tone.error);
    if (self->onToneSelected) self->onToneSelected(*tone);
    fin({});
  });
}

void MockSession::ensureNativeAuth(Done done) {
  juce::MessageManager::callAsync([cb = std::move(done)] { cb({}); });
}

void MockSession::login(LoginIntent intent) {
  intent_ = intent;
  const auto authorize = override("authorize").toString();
  if (authorize == "stall" || authorize == "browser-failed") {
    // The browser never comes back: the flow sits leaving with what the
    // screen needs from it.
    setFlow(AuthFlow::Phase::leaving);
    flow_.authorizeUrl =
        "https://www.tone3000.com/api/v1/oauth/authorize?response_type=code&client_id=pk_test&redirect_uri=http"
        "%3A%2F%2F127.0.0.1%3A49152%2Fcallback&code_challenge=mock&code_challenge_method=S256&state=mock";
    if (authorize == "browser-failed") flow_.browserProblem = "Your browser couldn't be opened.";
    notifyAuthFlowChanged();
    return;
  }
  finishSignIn();
}

void MockSession::finishSignIn() {
  setFlow(AuthFlow::Phase::idle);
  authenticated_ = true;
  notifySessionChanged();
  if (intent_ == LoginIntent::browse && onAuthenticated) onAuthenticated();
}

void MockSession::cancelFlow() {
  if (flow_.phase == AuthFlow::Phase::leaving || flow_.phase == AuthFlow::Phase::returning)
    setFlow(AuthFlow::Phase::idle);
}

void MockSession::startDeviceFlow() {
  if (flow_.phase != AuthFlow::Phase::leaving) return;
  flow_.device = AuthFlow::Device{};
  notifyAuthFlowChanged();
  const auto spec = override("device").toString();
  if (spec == "stall") return;
  juce::MessageManager::callAsync([self = juce::WeakReference<MockSession>(this), spec] {
    if (self == nullptr || !self->flow_.device) return;
    auto& device = *self->flow_.device;
    if (spec == "error") {
      device.state = AuthFlow::Device::State::failed;
      device.error = "Couldn't get a code from TONE3000.";
    } else if (spec == "expired") {
      device.state = AuthFlow::Device::State::failed;
      device.error = "That code expired.";
    } else {
      device.state = AuthFlow::Device::State::waiting;
      device.userCode = "BCDF-GHJK";
      device.verificationUri = "https://www.tone3000.com/activate";
      device.verificationUriComplete = "https://www.tone3000.com/activate?user_code=BCDF-GHJK";
    }
    self->notifyAuthFlowChanged();
    if (spec == "approve")
      juce::MessageManager::callAsync([self] {
        if (self != nullptr && self->flow_.device) self->finishSignIn();
      });
  });
}

void MockSession::logout() {
  authenticated_ = false;
  notifySessionChanged();
}

void MockSession::probeSecureConnection(std::function<void(Probe)> reply) {
  juce::MessageManager::callAsync([cb = std::move(reply)] { cb(Probe::inconclusive); });
}

void MockSession::fetchPluginVersion(Reply<juce::var> reply) {
  // Only scenarios that set `api.version` have a version endpoint at all.
  answer<juce::var>("version", std::move(reply), [this] {
    const auto spec = override("version");
    return spec.isObject() ? Result<juce::var>::ok(spec) : Result<juce::var>::fail("Not found (404).");
  });
}

}  // namespace t3k::ui::testbed
