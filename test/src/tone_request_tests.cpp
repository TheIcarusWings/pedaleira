// toneRequest.* (plugin/src/ToneRequestCommands.cpp): the user asks for a
// tone, a connected agent claims it, reports steps, proposes the tone in its
// sandbox and answers a refine message; the user applies it or stops it.
#include "AgentHub.h"
#include "agent_test_helpers.h"

#include <gtest/gtest.h>

namespace {

juce::String codeOf(const CommandResult& result) { return result.ok ? juce::String() : result.errorCode; }

}  // namespace

TEST(ToneRequests, AnAgentBuildsProposesAndTheUserApplies) {
  AgentRig rig;
  const auto agent = rig.hub.openSession("tone agent");
  int notified = 0;
  rig.hub.onToneRequestChanged = [&] { ++notified; };

  // Nothing yet; waiting returns at once with nothing.
  juce::var waited = rig.mustAs(agent, "toneRequest.wait", R"({"since":0,"timeoutMs":0})");
  EXPECT_TRUE(waited["request"].isVoid());
  const juce::int64 start = static_cast<juce::int64>(waited["sequence"]);

  // The user asks; a client cannot.
  EXPECT_EQ(codeOf(rig.as(agent, "toneRequest.create", R"({"prompt":"x"})")), "permission_denied");
  const juce::var created = rig.mustLocal(
      "toneRequest.create", R"({"prompt":"Edge of breakup clean with a dotted eighth delay","target":"newPreset"})");
  const juce::String id = created["id"].toString();
  EXPECT_EQ(created["status"].toString(), "waiting");
  EXPECT_EQ(notified, 1);
  EXPECT_EQ(codeOf(rig.local("toneRequest.create", R"({"prompt":"another"})")), "failed")
      << "one request is open at a time";

  // The agent sees it, claims it and reports steps.
  waited = rig.mustAs(agent, "toneRequest.wait", "{\"since\":" + juce::String(start) + ",\"timeoutMs\":0}");
  ASSERT_TRUE(waited["request"].isObject());
  EXPECT_EQ(waited["request"]["prompt"].toString(), "Edge of breakup clean with a dotted eighth delay");
  const juce::int64 seen = static_cast<juce::int64>(waited["sequence"]);
  EXPECT_GT(seen, start);
  EXPECT_EQ(codeOf(rig.as(agent, "toneRequest.progress",
                          "{\"id\":\"" + id + "\",\"text\":\"Picking an amp\"}")),
            "failed")
      << "steps need a claim";
  const juce::var claimed = rig.mustAs(agent, "toneRequest.claim", "{\"id\":\"" + id + "\"}");
  EXPECT_EQ(claimed["status"].toString(), "working");
  EXPECT_EQ(claimed["agent"].toString(), "tone agent");
  const auto other = rig.hub.openSession("someone else");
  EXPECT_EQ(codeOf(rig.as(other, "toneRequest.claim", "{\"id\":\"" + id + "\"}")), "failed");
  rig.mustAs(agent, "toneRequest.progress",
             "{\"id\":\"" + id + "\",\"text\":\"Adding a delay\",\"step\":2,\"of\":5}");
  juce::var state = rig.mustLocal("toneRequest.get")["request"];
  EXPECT_EQ(static_cast<int>(state["step"]), 2);
  EXPECT_EQ(static_cast<int>(state["stepsTotal"]), 5);
  EXPECT_EQ(state["steps"].size(), 1);

  // It builds in its own sandbox and proposes; another client's sandbox or a
  // made-up one is refused.
  const juce::String sandbox = rig.mustAs(agent, "sandbox.create", R"({"from":"empty"})")["sandbox"].toString();
  ASSERT_TRUE(sandbox.isNotEmpty());
  rig.mustAs(agent, "sandbox.use", "{\"sandbox\":\"" + sandbox + "\"}");
  rig.mustAs(agent, "chain.addBlock", R"({"kind":"delay.simple"})");
  EXPECT_EQ(codeOf(rig.as(agent, "toneRequest.propose",
                          "{\"id\":\"" + id + "\",\"sandbox\":\"nope\",\"summary\":\"x\"}")),
            "not_found");
  const juce::var proposed = rig.mustAs(
      agent, "toneRequest.propose",
      "{\"id\":\"" + id + "\",\"sandbox\":\"" + sandbox +
          "\",\"title\":\"Edge Chime\",\"summary\":\"Built a clean tone with a delay.\","
          "\"changes\":[{\"name\":\"Delay mix\",\"before\":\"0%\",\"after\":\"22%\"}]}");
  EXPECT_EQ(proposed["status"].toString(), "ready");
  EXPECT_EQ(proposed["messages"].size(), 1);
  EXPECT_EQ(proposed["messages"][0]["from"].toString(), "agent");

  // The user refines: the request is working again and the agent sees the
  // message as unanswered.
  rig.mustLocal("toneRequest.reply", R"({"text":"A bit darker"})");
  waited = rig.mustAs(agent, "toneRequest.wait", "{\"since\":" + juce::String(seen) + ",\"timeoutMs\":0}");
  EXPECT_EQ(waited["request"]["status"].toString(), "working");
  EXPECT_EQ(static_cast<int>(waited["request"]["unansweredMessages"]), 1);
  rig.mustAs(agent, "toneRequest.propose",
             "{\"id\":\"" + id + "\",\"sandbox\":\"" + sandbox + "\",\"summary\":\"Darker now.\"}");
  EXPECT_EQ(static_cast<int>(rig.mustLocal("toneRequest.get")["request"]["unansweredMessages"]), 0);

  // The user applies: the live rig takes the sandbox's chain, the request is
  // done, and a new one may be made.
  const juce::MemoryBlock liveBefore = rig.liveState();
  const juce::var applied = rig.mustLocal("toneRequest.apply");
  EXPECT_EQ(applied["status"].toString(), "applied");
  EXPECT_FALSE(liveBefore == rig.liveState()) << "the live rig took the sandbox's chain";
  EXPECT_EQ(codeOf(rig.as(agent, "toneRequest.progress",
                          "{\"id\":\"" + id + "\",\"text\":\"late\"}")),
            "failed");
  rig.mustLocal("toneRequest.create", R"({"prompt":"next one"})");
}

TEST(ToneRequests, StopAndDisconnectAreHandled) {
  AgentRig rig;
  auto agent = rig.hub.openSession("tone agent");
  const juce::String id = rig.mustLocal("toneRequest.create", R"({"prompt":"Ambient swells"})")["id"].toString();
  rig.mustAs(agent, "toneRequest.claim", "{\"id\":\"" + id + "\"}");

  // The agent goes: the request waits for another.
  rig.hub.closeSession(agent);
  juce::var state = rig.mustLocal("toneRequest.get")["request"];
  EXPECT_EQ(state["status"].toString(), "waiting");
  EXPECT_TRUE(state["agent"].toString().isEmpty());

  const auto next = rig.hub.openSession("second agent");
  rig.mustAs(next, "toneRequest.claim", "{\"id\":\"" + id + "\"}");
  rig.mustLocal("toneRequest.stop");
  EXPECT_EQ(rig.mustLocal("toneRequest.get")["request"]["status"].toString(), "stopped");
  EXPECT_EQ(codeOf(rig.as(next, "toneRequest.progress", "{\"id\":\"" + id + "\",\"text\":\"x\"}")),
            "failed");
  EXPECT_EQ(codeOf(rig.local("toneRequest.apply")), "failed");

  // Refusals on what a request may hold.
  EXPECT_EQ(codeOf(rig.local("toneRequest.create", R"({"prompt":"   "})")), "invalid_args");
  const juce::String tooLong = juce::String::repeatedString("x", 4001);
  EXPECT_EQ(codeOf(rig.local("toneRequest.create", "{\"prompt\":\"" + tooLong + "\"}")), "invalid_args");
}
