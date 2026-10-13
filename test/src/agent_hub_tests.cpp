// AgentHub: what stands between a caller that is not the user and the rig.
// Every command has a permission class; each switch of the policy does what
// it says; a request to change the rig being played waits for approval and
// runs exactly once when approved; a sandbox rig is a rig of its own, so
// editing it leaves the live rig's state and audio bit for bit alone; and a
// path outside the allowed folders is refused before the command sees it.
//
// The tests drive the hub the way the command server does, through
// AgentHub::run with a client session (command_server_tests.cpp covers the
// socket in front of it).

#include "SandboxRig.h"
#include "agent_test_helpers.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#if !JUCE_WINDOWS
#include <sys/stat.h>
#endif

namespace {

juce::String permissionOf(AgentRig& rig, const char* command) {
  return rig.commands.registry().describe(command)["permission"].toString();
}

int tonesIn(const juce::var& state) {
  int count = 0;
  if (const auto* lane = state["chain"]["chain"].getArray())
    for (const auto& item : *lane)
      count += item["kind"].toString() == "tone" ? 1 : 0;
  return count;
}

juce::String codeOf(const CommandResult& result) {
  return result.ok ? juce::String("ok") : result.errorCode;
}

bool sameBytes(const juce::MemoryBlock& a, const juce::MemoryBlock& b) { return a == b; }

}  // namespace

// ####################
// PERMISSION CLASSES
// ####################
TEST(AgentPermissions, EveryCommandDeclaresAClass) {
  AgentRig rig;
  const juce::var all = rig.commands.registry().describe();
  ASSERT_GT(all.size(), 100);

  std::map<juce::String, int> perClass;
  for (int i = 0; i < all.size(); ++i) {
    const juce::String name = all[i]["name"].toString();
    const juce::String permission = all[i]["permission"].toString();
    EXPECT_NE(permission, "unclassified")
        << name << " has no permission class: give its registration a CommandPermission";
    EXPECT_TRUE(juce::StringArray({"read", "edit", "render", "live", "delete", "settings", "local"})
                    .contains(permission))
        << name << ": " << permission;
    ++perClass[permission];

    // A command that only reads cannot be in a class that changes things,
    // and the other way round: the class is checked against what the command
    // says about itself.
    const bool mutates = static_cast<bool>(all[i]["mutates"]);
    if (permission == "read" || permission == "render")
      EXPECT_FALSE(mutates) << name;
    if (permission == "edit" || permission == "live" || permission == "delete" ||
        permission == "settings")
      EXPECT_TRUE(mutates) << name;
  }
  for (const char* expected : {"read", "edit", "render", "live", "delete", "settings", "local"})
    EXPECT_GT(perClass[expected], 0) << expected;

  // The ones the policy's wording is about.
  EXPECT_EQ(permissionOf(rig, "state.get"), "read");
  EXPECT_EQ(permissionOf(rig, "preset.list"), "read");
  EXPECT_EQ(permissionOf(rig, "assets.list"), "read");
  EXPECT_EQ(permissionOf(rig, "chain.addBlock"), "edit");
  EXPECT_EQ(permissionOf(rig, "preset.save"), "edit");
  EXPECT_EQ(permissionOf(rig, "sandbox.create"), "edit");
  EXPECT_EQ(permissionOf(rig, "render.clip"), "render");
  EXPECT_EQ(permissionOf(rig, "render.compare"), "render");
  EXPECT_EQ(permissionOf(rig, "render.probe"), "render");
  EXPECT_EQ(permissionOf(rig, "sandbox.applyToLive"), "live");
  EXPECT_EQ(permissionOf(rig, "guitar.record.start"), "live");
  EXPECT_EQ(permissionOf(rig, "preset.delete"), "delete");
  EXPECT_EQ(permissionOf(rig, "guitar.delete"), "delete");
  EXPECT_EQ(permissionOf(rig, "footswitch.setMode"), "settings");
  EXPECT_EQ(permissionOf(rig, "plugins.scan"), "settings");
  for (const char* localOnly :
       {"approvals.list", "approvals.resolve", "agents.list", "agents.disconnect",
        "agents.activity", "permissions.set", "server.setEnabled", "server.status", "roots.add",
        "roots.remove"})
    EXPECT_EQ(permissionOf(rig, localOnly), "local") << localOnly;
}

TEST(AgentPermissions, ASandboxRegistryHoldsOnlyRigCommands) {
  AgentRig rig;
  ChainTestProcessor other;
  EngineCommands rigOnly(other, EngineCommands::Scope::rig);
  EXPECT_FALSE(rigOnly.hasAgents());

  const juce::StringArray appNames = rig.commands.registry().names();
  const juce::StringArray rigNames = rigOnly.registry().names();
  for (const auto& name : rigNames)
    EXPECT_TRUE(appNames.contains(name)) << name;
  // The application's own commands are not a rig's.
  for (const char* appOnly : {"commands.describe", "sandbox.create", "render.clip", "guitar.list",
                              "assets.list", "agents.list", "approvals.resolve"}) {
    EXPECT_TRUE(appNames.contains(appOnly)) << appOnly;
    EXPECT_FALSE(rigNames.contains(appOnly)) << appOnly;
  }
  EXPECT_TRUE(rigNames.contains("chain.addBlock"));
  EXPECT_TRUE(rigNames.contains("state.get"));
}

TEST(AgentPermissions, DefaultsAndPersistence) {
  AgentRig rig;
  const AgentPolicy defaults = rig.hub.policy();
  EXPECT_TRUE(defaults.read);
  EXPECT_TRUE(defaults.edit);
  EXPECT_TRUE(defaults.render);
  EXPECT_FALSE(defaults.live);
  EXPECT_FALSE(defaults.destroy);
  EXPECT_FALSE(defaults.settings);
  // The server is off until someone switches it on.
  EXPECT_FALSE(AgentSettings::serverEnabled());
  EXPECT_EQ(rig.hub.server(), nullptr);
  EXPECT_FALSE(static_cast<bool>(rig.mustLocal("server.status")["running"]));

  const juce::var changed =
      rig.mustLocal("permissions.set", R"({"live":true,"render":false,"delete":true})");
  EXPECT_TRUE(static_cast<bool>(changed["live"]));
  EXPECT_FALSE(static_cast<bool>(changed["render"]));
  EXPECT_TRUE(static_cast<bool>(changed["delete"]));
  EXPECT_TRUE(static_cast<bool>(changed["read"]));  // left out: unchanged

  // Another instance (the next start of the application) reads the same.
  ChainTestProcessor next;
  EngineCommands nextCommands(next);
  const AgentPolicy restored = nextCommands.agents().policy();
  EXPECT_TRUE(restored.live);
  EXPECT_FALSE(restored.render);
  EXPECT_TRUE(restored.destroy);
  EXPECT_FALSE(restored.settings);
  EXPECT_TRUE(restored.read);
  EXPECT_TRUE(restored.edit);
}

TEST(AgentPermissions, EachSwitchGatesItsClass) {
  AgentRig rig;
  const auto client = rig.hub.openSession("agent");
  const auto set = [&](const char* json) { rig.mustLocal("permissions.set", json); };

  // read
  EXPECT_TRUE(rig.as(client, "state.get").ok);
  set(R"({"read":false})");
  EXPECT_EQ(codeOf(rig.as(client, "state.get")), "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "guitar.list")), "permission_denied");
  set(R"({"read":true})");

  // edit: without it not even a sandbox can be made.
  set(R"({"edit":false})");
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.create")), "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "guitar.create", R"({"name":"X","positions":["bridge"]})")),
            "permission_denied");
  EXPECT_TRUE(rig.as(client, "state.get").ok);  // reading is its own switch
  set(R"({"edit":true})");
  EXPECT_TRUE(rig.as(client, "sandbox.create", R"({"from":"empty"})").ok);

  // render
  set(R"({"render":false})");
  EXPECT_EQ(codeOf(rig.as(client, "render.probe", R"({"kind":"noise"})")), "permission_denied");
  set(R"({"render":true})");
  EXPECT_TRUE(rig.as(client, "render.probe", R"({"kind":"noise"})").ok);

  // live: an edit that lands on the rig being played waits for approval
  // while the switch is off, and simply runs once it is on.
  rig.mustAs(client, "sandbox.use", R"({"sandbox":"live"})");
  const juce::uint32 before = rig.proc.getCurrentChainRevision();
  EXPECT_EQ(codeOf(rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})")),
            "needs_approval");
  EXPECT_EQ(rig.proc.getCurrentChainRevision(), before);
  set(R"({"live":true})");
  EXPECT_TRUE(rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})").ok);
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 1);
  set(R"({"live":false})");
  // Reading the live rig never needed it.
  EXPECT_TRUE(rig.as(client, "state.get").ok);

  // delete: refused outright, not queued.
  EXPECT_EQ(codeOf(rig.as(client, "preset.delete", R"({"id":"nope"})")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "guitar.delete", R"({"guitar":"nope"})")), "permission_denied");
  set(R"({"delete":true})");
  // Past the gate now: the answer is the command's own.
  EXPECT_EQ(codeOf(rig.as(client, "guitar.delete", R"({"guitar":"nope"})")), "not_found");
  set(R"({"delete":false})");

  // settings
  EXPECT_EQ(codeOf(rig.as(client, "footswitch.setMode", R"({"mode":"scene"})")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "plugins.scan")), "permission_denied");
  set(R"({"settings":true})");
  EXPECT_TRUE(rig.as(client, "footswitch.setMode", R"({"mode":"scene"})").ok);

  // local: no switch opens these to a client.
  set(R"({"read":true,"edit":true,"render":true,"live":true,"delete":true,"settings":true})");
  for (const char* localOnly : {"approvals.list", "agents.list", "agents.activity",
                                "server.status"})
    EXPECT_EQ(codeOf(rig.as(client, localOnly)), "permission_denied") << localOnly;
  EXPECT_EQ(codeOf(rig.as(client, "permissions.set", R"({"live":true})")), "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "server.setEnabled", R"({"enabled":true})")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "approvals.resolve", R"({"id":"approval-1","approve":true})")),
            "permission_denied");
  // What a client may read about the policy.
  EXPECT_TRUE(rig.as(client, "permissions.get").ok);
  EXPECT_EQ(codeOf(rig.as(client, "no.such.command")), "unknown_command");
}

TEST(AgentPermissions, TheLocalSessionIsNotGated) {
  AgentRig rig;
  rig.mustLocal("permissions.set",
                R"({"read":false,"edit":false,"render":false,"live":false,"delete":false,"settings":false})");
  EXPECT_TRUE(rig.local("state.get").ok);
  EXPECT_TRUE(rig.local("chain.addBlock", R"({"kind":"utility.gain"})").ok);
  EXPECT_TRUE(rig.local("footswitch.setMode", R"({"mode":"stomp"})").ok);
  EXPECT_TRUE(rig.local("agents.list").ok);
  // And the registry called directly, as tests and the render tool do.
  EXPECT_TRUE(rig.commands.registry().run("chain.reset").ok);
}

// ####################
// APPROVALS
// ####################
TEST(AgentApprovals, ARequestWaitsRunsOnceAndReportsBack) {
  AgentRig rig;
  const auto client = rig.hub.openSession("builder");
  const auto other = rig.hub.openSession("bystander");
  int notified = 0;
  rig.hub.onApprovalsChanged = [&] { ++notified; };

  const juce::MemoryBlock before = rig.liveState();
  const CommandResult asked = rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  ASSERT_EQ(asked.errorCode, "needs_approval");
  const juce::String id = asked.toVar()["error"]["approvalId"].toString();
  ASSERT_TRUE(id.isNotEmpty());
  EXPECT_EQ(notified, 1);
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 1);
  // Nothing happened to the rig.
  EXPECT_TRUE(sameBytes(before, rig.liveState()));

  // Asking again for the same thing is the same request.
  const CommandResult again = rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  EXPECT_EQ(again.toVar()["error"]["approvalId"].toString(), id);
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 1);

  // The user's side sees who wants what.
  const juce::var listed = rig.mustLocal("approvals.list");
  ASSERT_EQ(listed["approvals"].size(), 1);
  EXPECT_EQ(listed["approvals"][0]["id"].toString(), id);
  EXPECT_EQ(listed["approvals"][0]["client"].toString(), "builder");
  EXPECT_EQ(listed["approvals"][0]["command"].toString(), "chain.addBlock");
  EXPECT_EQ(listed["approvals"][0]["args"]["kind"].toString(), "utility.gain");

  // The client polls its own request; nobody else's.
  const juce::String statusArgs = "{\"id\":" + agentQuoted(id) + "}";
  EXPECT_EQ(rig.mustAs(client, "approvals.status", statusArgs)["state"].toString(), "pending");
  EXPECT_EQ(codeOf(rig.as(other, "approvals.status", statusArgs)), "not_found");

  // Approved: the command runs as asked, once.
  const juce::var resolved =
      rig.mustLocal("approvals.resolve", "{\"id\":" + agentQuoted(id) + ",\"approve\":true}");
  EXPECT_EQ(resolved["state"].toString(), "approved");
  EXPECT_TRUE(static_cast<bool>(resolved["result"]["ok"]));
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 1);
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 0);
  EXPECT_GE(notified, 2);

  const juce::var status = rig.mustAs(client, "approvals.status", statusArgs);
  EXPECT_EQ(status["state"].toString(), "approved");
  EXPECT_TRUE(static_cast<bool>(status["result"]["ok"]));
  EXPECT_TRUE(status["result"]["result"]["blockId"].toString().isNotEmpty());

  EXPECT_EQ(codeOf(rig.local("approvals.resolve",
                             "{\"id\":" + agentQuoted(id) + ",\"approve\":true}")),
            "failed");
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 1);
  EXPECT_EQ(codeOf(rig.local("approvals.resolve", R"({"id":"approval-99","approve":true})")),
            "not_found");
}

TEST(AgentApprovals, DeniedRequestsChangeNothingAndTheQueueIsBounded) {
  AgentRig rig;
  const auto client = rig.hub.openSession("builder");
  const juce::MemoryBlock before = rig.liveState();

  const CommandResult asked = rig.as(client, "chain.setStereo", R"({"enabled":true})");
  ASSERT_EQ(asked.errorCode, "needs_approval");
  const juce::String id = asked.toVar()["error"]["approvalId"].toString();
  const juce::var denied =
      rig.mustLocal("approvals.resolve", "{\"id\":" + agentQuoted(id) + ",\"approve\":false}");
  EXPECT_EQ(denied["state"].toString(), "denied");
  EXPECT_TRUE(denied["result"].isVoid());
  EXPECT_TRUE(sameBytes(before, rig.liveState()));
  EXPECT_EQ(rig.mustAs(client, "approvals.status", "{\"id\":" + agentQuoted(id) + "}")["state"]
                .toString(),
            "denied");

  // One client cannot pile requests on the user: four waiting at most.
  for (int i = 0; i < 4; ++i)
    EXPECT_EQ(codeOf(rig.as(client, "tempo.set", "{\"bpm\":" + juce::String(100 + i) + "}")),
              "needs_approval");
  EXPECT_EQ(codeOf(rig.as(client, "tempo.set", R"({"bpm":140})")), "permission_denied");
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 4);

  // A client that goes away takes its waiting requests with it.
  rig.hub.closeSession(client);
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 0);
  EXPECT_TRUE(sameBytes(before, rig.liveState()));
}

TEST(AgentApprovals, ApplyingASandboxToTheLiveRigNeedsApproval) {
  AgentRig rig;
  const auto client = rig.hub.openSession("builder");
  rig.mustAs(client, "sandbox.create", R"({"from":"empty"})");
  rig.mustAs(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  rig.mustAs(client, "chain.addBlock", R"({"kind":"delay.simple"})");
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 0);

  const CommandResult asked = rig.as(client, "sandbox.applyToLive");
  ASSERT_EQ(asked.errorCode, "needs_approval");
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 0);
  const juce::String id = asked.toVar()["error"]["approvalId"].toString();

  const juce::var resolved =
      rig.mustLocal("approvals.resolve", "{\"id\":" + agentQuoted(id) + ",\"approve\":true}");
  EXPECT_TRUE(static_cast<bool>(resolved["result"]["ok"])) << juce::JSON::toString(resolved);
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 2);

  // With the switch on, the same command needs nobody.
  rig.mustLocal("permissions.set", R"({"live":true})");
  rig.mustAs(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  EXPECT_TRUE(rig.as(client, "sandbox.applyToLive").ok);
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 3);
}

// The user is asked about the rig the sandbox is at that moment, and that is
// the rig an approval applies: a client cannot show one thing and have
// another approved.
TEST(AgentApprovals, AnApprovalAppliesTheRigThatWasAskedAbout) {
  AgentRig rig;
  const auto client = rig.hub.openSession("builder");
  const juce::String sandbox =
      rig.mustAs(client, "sandbox.create", R"({"from":"empty"})")["sandbox"].toString();
  rig.mustAs(client, "chain.addBlock", R"({"kind":"utility.gain"})");

  const CommandResult asked = rig.as(client, "sandbox.applyToLive");
  ASSERT_EQ(asked.errorCode, "needs_approval");
  const juce::String id = asked.toVar()["error"]["approvalId"].toString();
  const auto listedRequest = [&](const juce::String& approvalId) {
    const juce::var listed = rig.mustLocal("approvals.list", R"({"all":true})");
    for (int i = 0; i < listed["approvals"].size(); ++i)
      if (listed["approvals"][i]["id"].toString() == approvalId)
        return listed["approvals"][i];
    return juce::var();
  };
  // What the prompt has: which sandbox, and that it is still that rig.
  EXPECT_EQ(listedRequest(id)["applies"]["sandbox"].toString(), sandbox);
  EXPECT_FALSE(static_cast<bool>(listedRequest(id)["applies"]["sandboxChanged"]));
  // Asking again, with nothing changed, is the same request.
  EXPECT_EQ(rig.as(client, "sandbox.applyToLive").toVar()["error"]["approvalId"].toString(), id);
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 1);

  // The switch: with the prompt on the user's screen, the sandbox becomes
  // another rig.
  rig.mustAs(client, "chain.addBlock", R"({"kind":"delay.simple"})");
  rig.mustAs(client, "chain.addBlock", R"({"kind":"delay.simple"})");
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 3);
  EXPECT_TRUE(static_cast<bool>(listedRequest(id)["applies"]["sandboxChanged"]));

  const juce::var resolved =
      rig.mustLocal("approvals.resolve", "{\"id\":" + agentQuoted(id) + ",\"approve\":true}");
  EXPECT_TRUE(static_cast<bool>(resolved["result"]["ok"])) << juce::JSON::toString(resolved);
  EXPECT_EQ(resolved["result"]["result"]["applied"].toString(), sandbox);
  // The one block that was asked about; the two added behind the prompt are
  // not on the rig being played.
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 1);
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 3);
  rig.mustLocal("chain.undo");
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 0);

  // Asking after an edit is a new request for the new rig, and the earlier
  // one for the same sandbox is withdrawn: two prompts that look alike are
  // never both waiting.
  const juce::String first =
      rig.as(client, "sandbox.applyToLive").toVar()["error"]["approvalId"].toString();
  rig.mustAs(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  const juce::String second =
      rig.as(client, "sandbox.applyToLive").toVar()["error"]["approvalId"].toString();
  EXPECT_NE(second, first);
  EXPECT_EQ(listedRequest(first)["state"].toString(), "expired");
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 1);
  EXPECT_EQ(codeOf(rig.local("approvals.resolve",
                             "{\"id\":" + agentQuoted(first) + ",\"approve\":true}")),
            "failed");
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 0);

  // The request does not need the sandbox any more: discarded, it is still
  // the four blocks that were asked about.
  rig.mustAs(client, "sandbox.discard");
  EXPECT_TRUE(static_cast<bool>(listedRequest(second)["applies"]["sandboxChanged"]));
  rig.mustLocal("approvals.resolve", "{\"id\":" + agentQuoted(second) + ",\"approve\":true}");
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 4);
  EXPECT_EQ(rig.mustAs(client, "approvals.status", "{\"id\":" + agentQuoted(second) + "}")["state"]
                .toString(),
            "approved");

  // Nothing to capture: refused before the user is asked.
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.applyToLive")), "invalid_args");
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.applyToLive", R"({"sandbox":"sandbox-99"})")),
            "not_found");
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 0);
}

// ####################
// SANDBOX RIGS
// ####################
// A sandbox is its maker's. To every other client it is not there: it cannot
// be selected, edited, rendered through, applied or discarded, and nothing
// says whether the id exists.
TEST(SandboxRigs, ASandboxBelongsToTheSessionThatMadeIt) {
  AgentRig rig;
  const auto owner = rig.hub.openSession("owner");
  const auto other = rig.hub.openSession("other");
  const juce::String id =
      rig.mustAs(owner, "sandbox.create", R"({"from":"empty","name":"mine"})")["sandbox"].toString();
  rig.mustAs(owner, "chain.addBlock", R"({"kind":"utility.gain"})");
  const juce::String named = "{\"sandbox\":" + agentQuoted(id) + "}";
  const juce::File clip =
      writeTestWav(rig.dirs.library.getChildFile("clip.wav"), makeSine(12000, 440.0, 0.2f));
  const juce::String clipArgs = "\"clips\":[" + agentQuoted(clip.getFullPathName()) + "]";

  const auto refusedEverything = [&](const std::shared_ptr<AgentSession>& session) {
    // Exactly what a sandbox that does not exist answers.
    for (const juce::String& candidate : {id, juce::String("sandbox-999")}) {
      const juce::String args = "{\"sandbox\":" + agentQuoted(candidate) + "}";
      EXPECT_EQ(codeOf(rig.as(session, "sandbox.use", args)), "not_found") << candidate;
      EXPECT_EQ(codeOf(rig.as(session, "sandbox.discard", args)), "not_found") << candidate;
      EXPECT_EQ(codeOf(rig.as(session, "sandbox.applyToLive", args)), "not_found") << candidate;
      EXPECT_EQ(codeOf(rig.as(session, "render.clip",
                              "{" + clipArgs + ",\"sandbox\":" + agentQuoted(candidate) + "}")),
                "not_found")
          << candidate;
      EXPECT_EQ(codeOf(rig.as(session, "render.probe",
                              "{\"kind\":\"noise\",\"sandbox\":" + agentQuoted(candidate) + "}")),
                "not_found")
          << candidate;
    }
  };
  refusedEverything(other);
  // With the live permission on the answer is the same: it is not the gate
  // that keeps a stranger out.
  rig.mustLocal("permissions.set", R"({"live":true})");
  refusedEverything(other);
  rig.mustLocal("permissions.set", R"({"live":false})");
  EXPECT_EQ(rig.hub.pendingApprovalCount(), 0);

  // Each sees its own; the user sees them all.
  EXPECT_EQ(rig.mustAs(other, "sandbox.list")["sandboxes"].size(), 0);
  EXPECT_EQ(rig.mustAs(other, "sandbox.list")["selected"].toString(), "live");
  ASSERT_EQ(rig.mustAs(owner, "sandbox.list")["sandboxes"].size(), 1);
  EXPECT_EQ(rig.mustAs(owner, "sandbox.list")["sandboxes"][0]["sandbox"].toString(), id);
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), 1);
  // The other one's rig commands still go to the live rig, where they wait.
  EXPECT_EQ(rig.hub.revisionFor(*other).rig, "live");
  EXPECT_EQ(codeOf(rig.as(other, "chain.addBlock", R"({"kind":"utility.gain"})")), "needs_approval");
  EXPECT_EQ(tonesIn(rig.mustAs(owner, "state.get")), 1);

  // The owner can do all of it.
  rig.mustAs(owner, "sandbox.use", R"({"sandbox":"live"})");
  rig.mustAs(owner, "sandbox.use", named);
  EXPECT_TRUE(rig.as(owner, "render.clip", "{" + clipArgs + "}").ok);
  EXPECT_EQ(codeOf(rig.as(owner, "sandbox.applyToLive", named)), "needs_approval");

  // The private copy a render of the live rig makes is nobody's to address,
  // its own caller and the user included, while it exists.
  CommandWork rendering = rig.hub.begin(*other, "render.clip", agentJson("{" + clipArgs + "}"));
  ASSERT_FALSE(rendering.finished.has_value());
  juce::String copy;
  for (int n = 1; n < 40 && copy.isEmpty(); ++n)
    if (const auto found = rig.hub.findSandbox("sandbox-" + juce::String(n));
        found != nullptr && found->temporary)
      copy = found->id;
  ASSERT_TRUE(copy.isNotEmpty());
  const juce::String copyArgs = "{\"sandbox\":" + agentQuoted(copy) + "}";
  for (const auto& session : {owner, other}) {
    EXPECT_EQ(codeOf(rig.as(session, "sandbox.use", copyArgs)), "not_found");
    EXPECT_EQ(codeOf(rig.as(session, "sandbox.discard", copyArgs)), "not_found");
    EXPECT_EQ(codeOf(rig.as(session, "render.probe",
                            "{\"kind\":\"noise\",\"sandbox\":" + agentQuoted(copy) + "}")),
              "not_found");
    EXPECT_EQ(rig.mustAs(session, "sandbox.list")["sandboxes"].size(), session == owner ? 1 : 0);
  }
  EXPECT_EQ(codeOf(rig.local("sandbox.use", copyArgs)), "not_found");
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), 1);
  EXPECT_TRUE(rendering.work().ok);
  EXPECT_TRUE(pumpUntil([&] { return rig.hub.findSandbox(copy) == nullptr; }, 5000));

  // And the owner alone discards it.
  EXPECT_EQ(rig.mustAs(owner, "sandbox.discard", named)["discarded"].toString(), id);
  EXPECT_EQ(rig.hub.findSandbox(id), nullptr);
}

// A sandbox whose owner disconnects while a render is running through it
// cannot go at that moment. It goes when the render ends, not with the
// application.
TEST(SandboxRigs, ASandboxOrphanedMidRenderIsDiscardedWhenTheRenderEnds) {
  AgentRig rig;
  const auto owner = rig.hub.openSession("owner");
  const juce::String id =
      rig.mustAs(owner, "sandbox.create", R"({"from":"empty"})")["sandbox"].toString();
  std::weak_ptr<SandboxRig> held = rig.hub.findSandbox(id);
  const juce::File clip =
      writeTestWav(rig.dirs.library.getChildFile("clip.wav"), makeSine(12000, 440.0, 0.2f));

  // The user previews the sandbox (a prompt's "listen first"); the render
  // has its rig reserved and its work still to do.
  CommandWork rendering = rig.hub.begin(
      rig.hub.localSession(), "render.clip",
      agentJson("{\"clips\":[" + agentQuoted(clip.getFullPathName()) + "],\"sandbox\":" +
                agentQuoted(id) + "}"));
  ASSERT_FALSE(rendering.finished.has_value());
  ASSERT_TRUE(held.lock()->rendering.load());

  rig.hub.closeSession(owner);
  ASSERT_NE(rig.hub.findSandbox(id), nullptr) << "discarded underneath a running render";

  const CommandResult rendered = rendering.work();
  EXPECT_TRUE(rendered.ok) << rendered.errorMessage;
  rendering = CommandWork();
  EXPECT_TRUE(pumpUntil([&] { return rig.hub.findSandbox(id) == nullptr; }, 5000))
      << "the orphaned sandbox outlived the render that was holding it";
  EXPECT_TRUE(held.expired());
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), 0);
}

TEST(SandboxRigs, EditingASandboxLeavesTheLiveRigBitIdentical) {
  AgentRig rig;
  // A live rig that does something audible to compare: 6 dB down.
  const juce::String gainId =
      rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})")["blockId"].toString();
  rig.mustLocal("block.setParam",
                "{\"blockId\":" + agentQuoted(gainId) + R"(,"param":"gainDb","value":-6})");
  rig.mustLocal("state.waitUntilLoaded");
  rig.proc.prepareToPlay(rig.rate, rig.block);
  rig.mustLocal("state.waitUntilLoaded");

  const std::vector<float> input = makeNoise(48000, 21, 0.25f);
  rig.processLive(input);  // let every ramp finish
  rig.settleRevision();
  const std::vector<float> audioBefore = rig.processLive(input);
  const juce::MemoryBlock stateBefore = rig.liveState();
  const juce::uint32 revisionBefore = rig.proc.getCurrentChainRevision();

  // An agent makes a sandbox from the live rig and tears into it.
  const auto client = rig.hub.openSession("agent");
  const juce::var made = rig.mustAs(client, "sandbox.create");
  EXPECT_EQ(made["sandbox"].toString(), "sandbox-1");
  EXPECT_TRUE(static_cast<bool>(made["selected"]));
  // It starts as a copy: the same block is there (once it has loaded: a
  // copy builds its blocks in the background, like a project being opened).
  rig.mustAs(client, "state.waitUntilLoaded");
  const juce::var copied = rig.mustAs(client, "state.get");
  ASSERT_EQ(tonesIn(copied), 1);

  rig.mustAs(client, "chain.addBlock", R"({"kind":"delay.simple"})");
  rig.mustAs(client, "chain.addBlock", R"({"kind":"dynamics.compressor"})");
  const juce::String sandboxGain = copied["chain"]["chain"][0]["blockId"].toString();
  rig.mustAs(client, "block.setParam",
             "{\"blockId\":" + agentQuoted(sandboxGain) + R"(,"param":"gainDb","value":12})");
  rig.mustAs(client, "param.set", R"({"id":"outputLevel","value":0.2})");
  rig.mustAs(client, "chain.setStereo", R"({"enabled":true})");
  rig.mustAs(client, "scene.add");
  rig.mustAs(client, "scene.rename", R"({"scene":"B","name":"Solo"})");
  rig.mustAs(client, "tempo.set", R"({"bpm":90})");
  rig.mustAs(client, "preset.save", R"({"name":"From the sandbox"})");
  rig.mustAs(client, "chain.undo");
  rig.mustAs(client, "state.waitUntilLoaded");
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 3);
  // Rendering through it does not reach the live rig either.
  EXPECT_TRUE(rig.as(client, "render.probe", R"({"kind":"response"})").ok);

  // The live rig: same revision, same saved state, same samples.
  EXPECT_EQ(rig.proc.getCurrentChainRevision(), revisionBefore);
  EXPECT_TRUE(sameBytes(stateBefore, rig.liveState()));
  EXPECT_EQ(tonesIn(rig.mustLocal("state.get")), 1);
  const std::vector<float> audioAfter = rig.processLive(input);
  ASSERT_EQ(audioBefore.size(), audioAfter.size());
  for (size_t i = 0; i < audioBefore.size(); ++i)
    ASSERT_EQ(audioBefore[i], audioAfter[i]) << "sample " << i;
  // (And the gain really is in the path, so the comparison means something.)
  EXPECT_NEAR(audioAfter[4800] / input[4800], 0.501f, 0.01f);
}

TEST(SandboxRigs, ApplyCopiesTheSandboxOntoTheLiveRigAsOneUndoStep) {
  AgentRig rig;
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  const juce::MemoryBlock original = rig.liveState();

  rig.mustLocal("sandbox.create", R"({"from":"empty","name":"new tone"})");
  // The local session's rig commands go through the hub like a client's.
  rig.mustLocal("chain.addBlock", R"({"kind":"delay.simple"})");
  rig.mustLocal("chain.addBlock", R"({"kind":"dynamics.compressor"})");
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  rig.mustLocal("param.set", R"({"id":"outputLevel","value":0.25})");
  const juce::var sandboxState = rig.mustLocal("state.get");
  ASSERT_EQ(tonesIn(sandboxState), 3);
  EXPECT_TRUE(sameBytes(original, rig.liveState()));

  const juce::var applied = rig.mustLocal("sandbox.applyToLive");
  EXPECT_EQ(applied["applied"].toString(), "sandbox-1");

  // The live rig now is the sandbox's rig: same blocks in the same order,
  // same preset-managed parameters. (Read from the live registry directly:
  // the local session still has the sandbox selected.)
  const juce::var liveState = rig.commands.registry().run("state.get").value;
  ASSERT_EQ(tonesIn(liveState), 3);
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(liveState["chain"]["chain"][i]["tone"]["title"].toString(),
              sandboxState["chain"]["chain"][i]["tone"]["title"].toString());
  EXPECT_NEAR(static_cast<double>(liveState["params"]["outputLevel"]["value"]), 0.25, 1.0e-4);

  // The sandbox is still there, and the user can take the change back.
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), 1);
  EXPECT_TRUE(rig.commands.registry().run("chain.undo").ok);
  EXPECT_EQ(tonesIn(rig.commands.registry().run("state.get").value), 1);
}

// The library is the player's. A sandbox is a guest in it: it adds presets,
// never replaces one, and leaves library.json (the player's position and
// recents) alone. What only reorganises the library is not a sandbox's
// command at all, so with a sandbox selected it meets the live side's gate.
TEST(SandboxRigs, ASandboxIsAGuestOfTheLibrary) {
  AgentRig rig;
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  const juce::var mine = rig.mustLocal("preset.save", R"({"name":"Mine"})");
  const juce::String mineId = mine["id"].toString();
  const juce::String setlistId = mine["setlist"].toString();
  EXPECT_EQ(static_cast<int>(mine["slot"]), 1);

  // One library: the setlists and the guitar profiles under the same root.
  const juce::File root = rig.proc.getLibraryRoot();
  EXPECT_EQ(root, rig.dirs.library);
  EXPECT_TRUE(root.getChildFile("Setlists").isDirectory());
  rig.mustLocal("guitar.create", R"({"name":"Tele","positions":["bridge"]})");
  EXPECT_TRUE(root.getChildFile("Guitars").getChildFile("tele").isDirectory());
  EXPECT_EQ(rig.mustLocal("library.get")["root"].toString(), root.getFullPathName());

  const juce::File stateFile = root.getChildFile("library.json");
  ASSERT_TRUE(stateFile.existsAsFile());
  const juce::String stateBefore = stateFile.loadFileAsString();
  const juce::File mineFile = root.getChildFile("Setlists")
                                  .getChildFile(rig.mustLocal("setlist.get")["name"].toString())
                                  .getChildFile("001 Mine.json");
  ASSERT_TRUE(mineFile.existsAsFile()) << mineFile.getFullPathName();
  const juce::String mineBefore = mineFile.loadFileAsString();

  const auto client = rig.hub.openSession("agent");
  rig.mustAs(client, "sandbox.create", R"({"from":"empty"})");

  // It reads the library and loads from it, and none of that is written
  // down: the recents and the position are the player's.
  EXPECT_EQ(rig.mustAs(client, "preset.list")["presets"].size(), 1);
  EXPECT_EQ(rig.mustAs(client, "setlist.list")["current"].toString(), setlistId);
  rig.mustAs(client, "preset.load", "{\"id\":" + agentQuoted(mineId) + "}");
  rig.mustAs(client, "preset.loadSlot", R"({"slot":1})");
  rig.mustAs(client, "setlist.select", R"({"id":"factory"})");
  EXPECT_EQ(rig.mustAs(client, "setlist.list")["current"].toString(), "factory");
  EXPECT_EQ(rig.commands.registry().run("setlist.list").value["current"].toString(), setlistId);
  rig.mustAs(client, "setlist.select", "{\"id\":" + agentQuoted(setlistId) + "}");
  rig.mustAs(client, "state.waitUntilLoaded");
  EXPECT_EQ(stateFile.loadFileAsString(), stateBefore);

  // It does not save over a preset: not by name (however spelled), not by slot.
  rig.mustAs(client, "chain.addBlock", R"({"kind":"delay.simple"})");
  for (const char* args : {R"({"name":"Mine"})", R"({"name":"mine"})"}) {
    const CommandResult refused = rig.as(client, "preset.save", args);
    EXPECT_EQ(codeOf(refused), "failed") << args;
    EXPECT_TRUE(refused.errorMessage.contains("only adds")) << refused.errorMessage;
  }
  EXPECT_EQ(codeOf(rig.as(client, "preset.saveToSlot", R"({"slot":1})")), "failed");
  EXPECT_EQ(codeOf(rig.as(client, "preset.saveToSlot", R"({"slot":1,"name":"Other"})")), "failed");
  EXPECT_EQ(mineFile.loadFileAsString(), mineBefore);
  // It adds one, which the live rig then sees.
  const juce::var added = rig.mustAs(client, "preset.saveToSlot", R"({"slot":5,"name":"Added"})");
  EXPECT_EQ(static_cast<int>(added["slot"]), 5);
  EXPECT_EQ(rig.commands.registry().run("preset.list").value["presets"].size(), 2);
  EXPECT_EQ(stateFile.loadFileAsString(), stateBefore);

  // An export is a file written: inside the allowed folders only, and from
  // a sandbox never over a file that is there.
  const juce::File exported = rig.dirs.scratch.getChildFile("out").getChildFile("tone.json");
  const juce::String exportArgs = "{\"path\":" + agentQuoted(exported.getFullPathName()) + "}";
  rig.mustAs(client, "preset.exportJson", exportArgs);
  EXPECT_TRUE(exported.existsAsFile());
  EXPECT_EQ(codeOf(rig.as(client, "preset.exportJson", exportArgs)), "failed");
  EXPECT_EQ(codeOf(rig.as(client, "preset.exportJson",
                          "{\"path\":" + agentQuoted(mineFile.getFullPathName()) + "}")),
            "failed");
  EXPECT_EQ(codeOf(rig.as(client, "preset.exportJson",
                          "{\"path\":" + agentQuoted(stateFile.getFullPathName()) + "}")),
            "failed");
  EXPECT_EQ(mineFile.loadFileAsString(), mineBefore);
  const juce::File outside = rig.dirs.outside.getChildFile("tone.json");
  EXPECT_EQ(codeOf(rig.as(client, "preset.exportJson",
                          "{\"path\":" + agentQuoted(outside.getFullPathName()) + "}")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "preset.exportJson", R"({"path":"tone.json"})")),
            "permission_denied");
  EXPECT_FALSE(outside.exists());
  EXPECT_EQ(codeOf(rig.as(client, "preset.importJson",
                          "{\"path\":" + agentQuoted(outside.getFullPathName()) + "}")),
            "permission_denied");

  // The commands that reorganise the library are not a sandbox's. With the
  // sandbox selected they still land on the live side, behind its gate:
  // approval for an edit, the delete switch for the trash.
  const auto sandbox = rig.hub.findSandbox("sandbox-1");
  ASSERT_NE(sandbox, nullptr);
  const std::vector<std::pair<const char*, juce::String>> libraryEdits = {
      {"setlist.create", R"({"name":"Agent's"})"},
      {"setlist.rename", "{\"id\":" + agentQuoted(setlistId) + R"(,"name":"Renamed"})"},
      {"setlist.reorder", "{\"id\":" + agentQuoted(setlistId) + R"(,"index":0})"},
      {"preset.rename", "{\"id\":" + agentQuoted(mineId) + R"(,"name":"Renamed"})"},
      {"preset.move", "{\"id\":" + agentQuoted(mineId) + R"(,"delta":1})"},
      {"preset.moveSlot", R"({"from":1,"to":2})"},
      {"preset.duplicateSlot", R"({"slot":1})"},
      {"preset.copyToSetlist", "{\"slot\":1,\"to\":" + agentQuoted(setlistId) + "}"},
      {"preset.importJson", "{\"path\":" + agentQuoted(exported.getFullPathName()) + "}"},
      {"favourite.set", "{\"id\":" + agentQuoted(mineId) + R"(,"favourite":true})"},
      {"library.rescan", R"({"tidy":true})"},
      {"library.importLegacyPresets", "{}"},
  };
  for (const auto& [name, args] : libraryEdits) {
    EXPECT_FALSE(sandbox->commands().registry().contains(name)) << name;
    EXPECT_EQ(permissionOf(rig, name), "edit") << name;
    const CommandResult asked = rig.as(client, name, args);
    EXPECT_EQ(codeOf(asked), "needs_approval") << name << ": " << asked.errorMessage;
    // Turned down, so the queue (four per client) never fills.
    if (asked.errorCode == CommandError::needsApproval)
      rig.mustLocal("approvals.resolve", "{\"id\":" +
                                             agentQuoted(asked.errorDetails["approvalId"].toString()) +
                                             R"(,"approve":false})");
  }
  const std::vector<std::pair<const char*, juce::String>> libraryDeletes = {
      {"preset.delete", "{\"id\":" + agentQuoted(mineId) + "}"},
      {"preset.deleteSlot", R"({"slot":1})"},
      {"setlist.delete", "{\"id\":" + agentQuoted(setlistId) + "}"},
  };
  for (const auto& [name, args] : libraryDeletes) {
    EXPECT_FALSE(sandbox->commands().registry().contains(name)) << name;
    EXPECT_EQ(permissionOf(rig, name), "delete") << name;
    EXPECT_EQ(codeOf(rig.as(client, name, args)), "permission_denied") << name;
  }
  // Nothing of it happened.
  const juce::var slots = rig.commands.registry().run("setlist.get").value["slots"];
  ASSERT_EQ(slots.size(), 2);
  EXPECT_EQ(slots[0]["name"].toString(), "Mine");
  EXPECT_FALSE(static_cast<bool>(slots[0]["favourite"]));
  EXPECT_EQ(slots[1]["name"].toString(), "Added");
  EXPECT_EQ(rig.commands.registry().run("setlist.list").value["setlists"].size(), 2);
  EXPECT_EQ(mineFile.loadFileAsString(), mineBefore);
  EXPECT_EQ(stateFile.loadFileAsString(), stateBefore);

  // Approved, the edit runs on the live side although the client is on its
  // sandbox.
  const CommandResult rename = rig.as(
      client, "preset.rename", "{\"id\":" + agentQuoted(mineId) + R"(,"name":"Renamed"})");
  ASSERT_EQ(codeOf(rename), "needs_approval");
  rig.mustLocal("approvals.resolve",
                "{\"id\":" + agentQuoted(rename.errorDetails["approvalId"].toString()) +
                    R"(,"approve":true})");
  EXPECT_EQ(rig.commands.registry().run("setlist.get").value["slots"][0]["name"].toString(),
            "Renamed");
}

// Applying a sandbox hands the live rig the sandbox's preset as it is in the
// sandbox: unsaved when it was edited there, and without a load being noted.
TEST(SandboxRigs, ApplyCarriesThePresetAndWhetherItIsSaved) {
  AgentRig rig;
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  const juce::String presetId = rig.mustLocal("preset.save", R"({"name":"Base"})")["id"].toString();
  rig.commands.registry().run("chain.reset");
  const juce::File stateFile = rig.proc.getLibraryRoot().getChildFile("library.json");
  const juce::String stateBefore = stateFile.loadFileAsString();
  const auto liveLibrary = [&] { return rig.commands.registry().run("library.get").value; };
  EXPECT_EQ(liveLibrary()["current"]["presetId"].toString(), "");

  rig.mustLocal("sandbox.create",
                "{\"from\":\"preset\",\"presetId\":" + agentQuoted(presetId) + "}");
  rig.mustLocal("state.waitUntilLoaded");
  rig.mustLocal("sandbox.applyToLive");
  EXPECT_TRUE(rig.commands.registry().run("state.waitUntilLoaded").ok);
  EXPECT_EQ(liveLibrary()["current"]["presetId"].toString(), presetId);
  EXPECT_EQ(rig.commands.registry().run("state.get").value["chain"]["preset"]["name"].toString(),
            "Base");
  EXPECT_FALSE(static_cast<bool>(liveLibrary()["unsaved"]));

  rig.mustLocal("chain.addBlock", R"({"kind":"delay.simple"})");
  rig.mustLocal("sandbox.applyToLive");
  EXPECT_TRUE(rig.commands.registry().run("state.waitUntilLoaded").ok);
  EXPECT_EQ(liveLibrary()["current"]["presetId"].toString(), presetId);
  EXPECT_TRUE(static_cast<bool>(liveLibrary()["unsaved"]));
  EXPECT_EQ(stateFile.loadFileAsString(), stateBefore);

  // Saving it on the live rig settles it.
  EXPECT_TRUE(rig.commands.registry().run("preset.saveToSlot", agentJson(R"({"slot":1})")).ok);
  EXPECT_FALSE(static_cast<bool>(liveLibrary()["unsaved"]));
}

TEST(SandboxRigs, DiscardFreesTheRigAndReturnsToTheLiveOne) {
  AgentRig rig;
  const auto client = rig.hub.openSession("agent");
  const juce::String id = rig.mustAs(client, "sandbox.create", R"({"from":"empty"})")["sandbox"].toString();
  std::weak_ptr<SandboxRig> held = rig.hub.findSandbox(id);
  ASSERT_FALSE(held.expired());
  EXPECT_EQ(rig.hub.revisionFor(*client).rig, id);
  EXPECT_EQ(rig.mustAs(client, "sandbox.list")["selected"].toString(), id);

  // A sandbox that is rendering is neither edited nor discarded.
  held.lock()->rendering.store(true);
  EXPECT_EQ(codeOf(rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})")), "unavailable");
  EXPECT_TRUE(rig.as(client, "state.get").ok);
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.discard")), "unavailable");
  held.lock()->rendering.store(false);

  EXPECT_EQ(rig.mustAs(client, "sandbox.discard")["discarded"].toString(), id);
  EXPECT_TRUE(held.expired()) << "the sandbox's processor is still alive";
  EXPECT_EQ(rig.hub.findSandbox(id), nullptr);
  EXPECT_EQ(rig.mustAs(client, "sandbox.list")["sandboxes"].size(), 0);
  EXPECT_EQ(rig.mustAs(client, "sandbox.list")["selected"].toString(), "live");
  EXPECT_EQ(rig.hub.revisionFor(*client).rig, "live");
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.discard")), "invalid_args");
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.use", "{\"sandbox\":" + agentQuoted(id) + "}")),
            "not_found");

  // At most four at a time.
  for (int i = 0; i < AgentHub::kMaxSandboxes; ++i)
    EXPECT_TRUE(rig.as(client, "sandbox.create", R"({"from":"empty","use":false})").ok);
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.create", R"({"from":"empty"})")), "failed");
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), AgentHub::kMaxSandboxes);

  // A connection's sandboxes go when it does.
  rig.hub.closeSession(client);
  EXPECT_EQ(rig.mustLocal("sandbox.list")["sandboxes"].size(), 0);
}

TEST(SandboxRigs, FromAPresetAndSelection) {
  AgentRig rig;
  rig.mustLocal("chain.addBlock", R"({"kind":"utility.gain"})");
  rig.mustLocal("chain.addBlock", R"({"kind":"delay.simple"})");
  const juce::String presetId = rig.mustLocal("preset.save", R"({"name":"Two blocks"})")["id"].toString();
  rig.mustLocal("chain.reset");

  const auto client = rig.hub.openSession("agent");
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.create", R"({"from":"preset"})")), "invalid_args");
  EXPECT_EQ(codeOf(rig.as(client, "sandbox.create", R"({"from":"preset","presetId":"nope"})")),
            "not_found");
  const juce::var made = rig.mustAs(
      client, "sandbox.create", "{\"from\":\"preset\",\"presetId\":" + agentQuoted(presetId) + "}");
  rig.mustAs(client, "state.waitUntilLoaded");
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 2);
  // The sandbox shares the preset store: it sees the preset, and what it
  // saves the live rig can load.
  EXPECT_EQ(rig.mustAs(client, "preset.list")["presets"].size(), 1);
  rig.mustAs(client, "preset.save", R"({"name":"Saved in the sandbox"})");
  EXPECT_EQ(rig.commands.registry().run("preset.list").value["presets"].size(), 2);

  // Back on the live rig, reads show the live rig and edits wait for approval.
  rig.mustAs(client, "sandbox.use", R"({"sandbox":"live"})");
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 0);
  EXPECT_EQ(codeOf(rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})")),
            "needs_approval");
  rig.mustAs(client, "sandbox.use", "{\"sandbox\":" + agentQuoted(made["sandbox"].toString()) + "}");
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 2);
}

// ####################
// ALLOWED FOLDERS
// ####################
TEST(AgentPaths, IsInsideResolvesDotsAndLinks) {
  AgentTestDirs dirs;
  const juce::File inside = dirs.library.getChildFile("amps").getChildFile("a.nam");
  EXPECT_TRUE(inside.getParentDirectory().createDirectory().wasOk());
  EXPECT_TRUE(inside.replaceWithText("x"));
  EXPECT_TRUE(LibraryPaths::isInside(inside, dirs.library));
  // The root is the fence, not something inside it, however it is spelled.
  EXPECT_FALSE(LibraryPaths::isInside(dirs.library, dirs.library));
  EXPECT_FALSE(LibraryPaths::isInside(juce::File(dirs.library.getFullPathName() + "/amps/.."),
                                      dirs.library));
  EXPECT_TRUE(LibraryPaths::isInside(inside.getParentDirectory(), dirs.library));
  // A file that does not exist yet is judged by where it would be.
  EXPECT_TRUE(LibraryPaths::isInside(dirs.library.getChildFile("new").getChildFile("b.wav"),
                                     dirs.library));
  EXPECT_FALSE(LibraryPaths::isInside(dirs.outside.getChildFile("a.nam"), dirs.library));
  EXPECT_FALSE(LibraryPaths::isInside(dirs.dir, dirs.library));
  // A sibling whose name merely starts the same is not inside.
  const juce::File sibling = dirs.dir.getChildFile("LibraryOther");
  EXPECT_TRUE(sibling.createDirectory().wasOk());
  EXPECT_FALSE(LibraryPaths::isInside(sibling.getChildFile("a.nam"), dirs.library));
  // Dots are resolved before the comparison.
  EXPECT_FALSE(LibraryPaths::isInside(
      juce::File(dirs.library.getFullPathName() + "/amps/../../Outside/a.nam"), dirs.library));
  EXPECT_FALSE(LibraryPaths::isInside(juce::File(), dirs.library));

#if !JUCE_WINDOWS
  // A link inside the library that points out of it is outside.
  const juce::File secret = dirs.outside.getChildFile("secret.wav");
  EXPECT_TRUE(secret.replaceWithText("x"));
  const juce::File link = dirs.library.getChildFile("link.wav");
  ASSERT_TRUE(secret.createSymbolicLink(link, true));
  EXPECT_FALSE(LibraryPaths::isInside(link, dirs.library));
  const juce::File folderLink = dirs.library.getChildFile("out");
  ASSERT_TRUE(dirs.outside.createSymbolicLink(folderLink, true));
  EXPECT_FALSE(LibraryPaths::isInside(folderLink.getChildFile("secret.wav"), dirs.library));
#endif

  EXPECT_EQ(LibraryPaths::slug("Les Paul #2"), "les-paul-2");
  EXPECT_EQ(LibraryPaths::slug("../../etc"), "etc");
  EXPECT_EQ(LibraryPaths::slug("???"), "item");
}

TEST(AgentPaths, ClientsMayOnlyNameFilesInAllowedFolders) {
  AgentRig rig;
  const auto client = rig.hub.openSession("agent");
  rig.mustAs(client, "sandbox.create", R"({"from":"empty"})");

  const juce::File inLibrary = rig.dirs.library.getChildFile("amp.nam");
  const juce::File outside = rig.dirs.outside.getChildFile("amp.nam");
  ASSERT_TRUE(testFile("a2-amp-test.nam").copyFileTo(inLibrary));
  ASSERT_TRUE(testFile("a2-amp-test.nam").copyFileTo(outside));
  const auto add = [&](const juce::String& path) {
    return rig.as(client, "chain.addToneFromFiles", "{\"paths\":[" + agentQuoted(path) + "]}");
  };

  // Inside the library: the command runs.
  EXPECT_TRUE(add(inLibrary.getFullPathName()).ok);
  // Outside, by any spelling: refused before the engine looks at the file.
  for (const juce::String& path :
       {outside.getFullPathName(),
        rig.dirs.library.getFullPathName() + "/../Outside/amp.nam", juce::String("amp.nam"),
        juce::String("/etc/hosts")}) {
    const CommandResult refused = add(path);
    EXPECT_EQ(refused.errorCode, "permission_denied") << path;
  }
  EXPECT_EQ(tonesIn(rig.mustAs(client, "state.get")), 1);

  // Every path-taking command is covered, not only that one.
  const juce::String outsideWav = agentQuoted(rig.dirs.outside.getChildFile("ref.wav").getFullPathName());
  writeTestWav(rig.dirs.outside.getChildFile("ref.wav"), makeSine(48000, 440.0, 0.2f));
  EXPECT_EQ(codeOf(rig.as(client, "render.clip", "{\"clips\":[" + outsideWav + "]}")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "render.compare",
                          "{\"reference\":" + outsideWav + ",\"clips\":[" + outsideWav + "]}")),
            "permission_denied");
  rig.mustAs(client, "guitar.create", R"({"name":"G","positions":["bridge"]})");
  EXPECT_EQ(codeOf(rig.as(client, "guitar.importClip",
                          R"({"guitar":"g","position":"bridge","take":"soft","path":)" +
                              outsideWav + "}")),
            "permission_denied");
  rig.mustLocal("permissions.set", R"({"settings":true})");
  EXPECT_EQ(codeOf(rig.as(client, "plugins.addFolder",
                          "{\"path\":" + agentQuoted(rig.dirs.outside.getFullPathName()) + "}")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "plugins.scan",
                          "{\"folders\":[" + agentQuoted(rig.dirs.outside.getFullPathName()) + "]}")),
            "permission_denied");
  EXPECT_EQ(codeOf(rig.as(client, "plugins.scan",
                          "{\"files\":[" + agentQuoted(outside.getFullPathName()) + "]}")),
            "permission_denied");

  // The roots a client can ask about, and only the user can widen.
  const juce::var roots = rig.mustAs(client, "roots.list");
  EXPECT_GE(roots["roots"].size(), 3);
  EXPECT_EQ(roots["added"].size(), 0);
  EXPECT_EQ(codeOf(rig.as(client, "roots.add",
                          "{\"path\":" + agentQuoted(rig.dirs.outside.getFullPathName()) + "}")),
            "permission_denied");
  rig.mustLocal("roots.add", "{\"path\":" + agentQuoted(rig.dirs.outside.getFullPathName()) + "}");
  EXPECT_EQ(rig.mustAs(client, "roots.list")["added"].size(), 1);
  EXPECT_TRUE(add(outside.getFullPathName()).ok);
  rig.mustLocal("roots.remove", "{\"path\":" + agentQuoted(rig.dirs.outside.getFullPathName()) + "}");
  EXPECT_EQ(codeOf(add(outside.getFullPathName())), "permission_denied");

  // The local session, which is the user, is not confined.
  EXPECT_TRUE(rig.local("chain.addToneFromFiles",
                        "{\"paths\":[" + agentQuoted(outside.getFullPathName()) + "]}")
                  .ok);
  rig.mustLocal("state.waitUntilLoaded");
}

// The edges of the fence: a root is not inside itself, the folder a command
// writes beside the file it was given is held to the fence too, and a list
// of plugin files takes whole paths only.
TEST(AgentPaths, RootsSiblingFoldersAndPluginFiles) {
  AgentRig rig;
  const auto client = rig.hub.openSession("agent");
  rig.mustAs(client, "sandbox.create", R"({"from":"empty"})");
  rig.mustAs(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  const auto exportTo = [&](const std::shared_ptr<AgentSession>& session, const juce::File& file) {
    const juce::String args = "{\"path\":" + agentQuoted(file.getFullPathName()) + "}";
    return session != nullptr ? rig.as(session, "preset.exportJson", args)
                              : rig.local("preset.exportJson", args);
  };

  // A root named as the file: it is the fence, not a place inside it.
  EXPECT_EQ(codeOf(exportTo(client, rig.dirs.library)), "permission_denied");
  EXPECT_EQ(codeOf(exportTo(client, rig.dirs.scratch)), "permission_denied");
  EXPECT_TRUE(rig.dirs.library.isDirectory());

  // An ordinary export, inside.
  const juce::File exports = rig.dirs.library.getChildFile("exports");
  ASSERT_TRUE(exports.createDirectory().wasOk());
  EXPECT_TRUE(exportTo(client, exports.getChildFile("plain.json")).ok);

#if !JUCE_WINDOWS
  // An Assets folder beside the target that is a link out of the allowed
  // folders: the export would put the model files wherever it points.
  const juce::File linked = rig.dirs.library.getChildFile("linked");
  ASSERT_TRUE(linked.createDirectory().wasOk());
  ASSERT_TRUE(rig.dirs.outside.createSymbolicLink(linked.getChildFile("Assets"), true));
  const CommandResult through = exportTo(client, linked.getChildFile("preset.json"));
  EXPECT_EQ(through.errorCode, "permission_denied");
  EXPECT_TRUE(through.errorMessage.contains("Assets")) << through.errorMessage;
  EXPECT_FALSE(linked.getChildFile("preset.json").exists());
  EXPECT_EQ(rig.dirs.outside.getNumberOfChildFiles(juce::File::findFilesAndDirectories), 0);
  // Reading is held to the same: an import does not fetch assets through it.
  EXPECT_EQ(codeOf(rig.as(client, "preset.importJson",
                          "{\"path\":" +
                              agentQuoted(linked.getChildFile("preset.json").getFullPathName()) + "}")),
            "permission_denied");
  // The engine itself does not write through such a link either, whoever
  // asks and wherever it points (here: inside the library).
  const juce::File inward = rig.dirs.library.getChildFile("inward");
  ASSERT_TRUE(inward.createDirectory().wasOk());
  ASSERT_TRUE(exports.createSymbolicLink(inward.getChildFile("Assets"), true));
  const int before = exports.getNumberOfChildFiles(juce::File::findFilesAndDirectories);
  for (const auto& session : {client, std::shared_ptr<AgentSession>()}) {
    const CommandResult refused = exportTo(session, inward.getChildFile("preset.json"));
    EXPECT_EQ(refused.errorCode, "failed");
    EXPECT_TRUE(refused.errorMessage.contains("symbolic link")) << refused.errorMessage;
  }
  EXPECT_FALSE(inward.getChildFile("preset.json").exists());
  EXPECT_EQ(exports.getNumberOfChildFiles(juce::File::findFilesAndDirectories), before);
#endif

  // plugins.scan's files: whole paths inside the allowed folders. A name
  // relative to wherever the application runs, or a path that climbs, never
  // reaches the scan helper.
  rig.mustLocal("permissions.set", R"({"settings":true})");
  const auto scan = [&](const std::shared_ptr<AgentSession>& session, const juce::String& entry) {
    const juce::String args = "{\"files\":[" + agentQuoted(entry) + "]}";
    return session != nullptr ? rig.as(session, "plugins.scan", args) : rig.local("plugins.scan", args);
  };
  const juce::String inLibrary = rig.dirs.library.getFullPathName();
  EXPECT_EQ(codeOf(scan(client, "Evil.vst3")), "permission_denied");
  EXPECT_EQ(codeOf(scan(client, "../../Evil.vst3")), "permission_denied");
  EXPECT_EQ(codeOf(scan(client, "plugins/Evil.vst3")), "permission_denied");
  EXPECT_EQ(codeOf(scan(client, "AudioUnit:Effects/aufx,dely,appl")), "permission_denied");
  EXPECT_EQ(codeOf(scan(client, inLibrary + "/../Outside/Evil.vst3")), "permission_denied");
  // Lands inside, but says it with a climb: refused all the same.
  const CommandResult climbing = scan(client, inLibrary + "/exports/../Evil.vst3");
  EXPECT_EQ(climbing.errorCode, "invalid_args");
  EXPECT_TRUE(climbing.errorMessage.contains("..")) << climbing.errorMessage;
  // The local caller is not confined to the folders, but the same holds for
  // what it may hand the helper.
  EXPECT_EQ(codeOf(scan(nullptr, "Evil.vst3")), "invalid_args");
  EXPECT_EQ(codeOf(scan(nullptr, rig.dirs.outside.getFullPathName() + "/../Evil.vst3")),
            "invalid_args");
}

// The scratch folder is the one place a client can have files written. It is
// the user's own (not a name in the shared temp directory that anybody could
// have made first), private, and never a link.
TEST(AgentPaths, TheScratchFolderIsPrivateAndNeverALink) {
  AgentRig rig;
  const juce::File scratch = rig.dirs.dir.getChildFile("NewScratch").getChildFile("Scratch");
  LibraryPaths::setScratchDirForTesting(scratch);
  juce::String problem;
  EXPECT_EQ(LibraryPaths::scratchDir(&problem), scratch);
  EXPECT_EQ(problem, "");
  EXPECT_TRUE(scratch.isDirectory());
#if !JUCE_WINDOWS
  struct stat status {};
  ASSERT_EQ(::stat(scratch.getFullPathName().toRawUTF8(), &status), 0);
  EXPECT_EQ(status.st_mode & 0777, 0700);

  // Somebody put a link there, to a folder they would like written to.
  ASSERT_TRUE(scratch.deleteRecursively());
  ASSERT_TRUE(rig.dirs.outside.createSymbolicLink(scratch, true));
  EXPECT_EQ(LibraryPaths::scratchDir(&problem), juce::File());
  EXPECT_TRUE(problem.contains("symbolic link")) << problem;
  // It is not an allowed folder then, and neither is where it points.
  for (const auto& root : rig.hub.allowedRoots()) {
    EXPECT_NE(root, scratch);
    EXPECT_NE(root, rig.dirs.outside);
  }
  // A render that was to write its audio there says why it will not.
  const juce::File clip =
      writeTestWav(rig.dirs.library.getChildFile("clip.wav"), makeSine(12000, 440.0, 0.2f));
  const CommandResult rendered =
      rig.local("render.clip", "{\"clips\":[" + agentQuoted(clip.getFullPathName()) +
                                   "],\"writeAudio\":true}");
  EXPECT_EQ(rendered.errorCode, "failed");
  EXPECT_TRUE(rendered.errorMessage.contains("symbolic link")) << rendered.errorMessage;
  EXPECT_EQ(rig.dirs.outside.getNumberOfChildFiles(juce::File::findFilesAndDirectories), 0);
  // Without the audio it renders as ever.
  EXPECT_TRUE(rig.local("render.clip", "{\"clips\":[" + agentQuoted(clip.getFullPathName()) + "]}").ok);
#endif
}

// ####################
// ACTIVITY
// ####################
TEST(AgentActivity, TheLastCommandsAreKeptWithTheirOutcome) {
  AgentRig rig;
  const auto client = rig.hub.openSession("  logger\n\t");
  EXPECT_EQ(client->name, "logger");
  rig.as(client, "state.revision");
  rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  rig.as(client, "agents.list");
  rig.as(client, "no.such.command");
  rig.mustAs(client, "sandbox.create", R"({"from":"empty"})");
  rig.as(client, "chain.addBlock", R"({"kind":"utility.gain"})");
  rig.as(client, "block.setParam", R"({"blockId":"nope","param":"mix","value":0.5})");

  const juce::var log = rig.mustLocal("agents.activity", R"({"limit":200})")["activity"];
  std::vector<std::pair<juce::String, juce::String>> seen;
  for (int i = 0; i < log.size(); ++i)
    if (log[i]["client"].toString() == "logger")
      seen.emplace_back(log[i]["command"].toString(), log[i]["result"].toString());
  ASSERT_EQ(seen.size(), 7u);
  EXPECT_EQ(seen[0], (std::pair<juce::String, juce::String>("state.revision", "ok")));
  EXPECT_EQ(seen[1], (std::pair<juce::String, juce::String>("chain.addBlock", "needs_approval")));
  EXPECT_EQ(seen[2], (std::pair<juce::String, juce::String>("agents.list", "permission_denied")));
  EXPECT_EQ(seen[3], (std::pair<juce::String, juce::String>("no.such.command", "unknown_command")));
  EXPECT_EQ(seen[4], (std::pair<juce::String, juce::String>("sandbox.create", "ok")));
  EXPECT_EQ(seen[5], (std::pair<juce::String, juce::String>("chain.addBlock", "ok")));
  EXPECT_EQ(seen[6], (std::pair<juce::String, juce::String>("block.setParam", "not_found")));
  // Which rig an edit landed on, and its arguments in short.
  for (int i = 0; i < log.size(); ++i)
    if (log[i]["command"].toString() == "block.setParam") {
      EXPECT_EQ(log[i]["rig"].toString(), "sandbox-1");
      EXPECT_TRUE(log[i]["args"].toString().contains("nope"));
      EXPECT_LE(log[i]["args"].toString().length(), 160);
    }

  // A ring of 200: older entries fall off.
  for (int i = 0; i < 230; ++i)
    rig.as(client, "state.revision");
  EXPECT_EQ(rig.mustLocal("agents.activity", R"({"limit":200})")["activity"].size(), 200);
  EXPECT_EQ(rig.mustLocal("agents.activity")["activity"].size(), 50);
  // 237 requests, of which three were refused before any command ran.
  EXPECT_EQ(client->commandCount, 234);
}
