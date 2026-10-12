// CommandRegistry: the transport-neutral half of the command layer. These pin
// the contract every client relies on: arguments are checked against the
// declared spec before a handler runs (and the message names the offender),
// results come back in one envelope, the table describes itself, and commands
// can be marshalled onto the message thread from elsewhere.

#include "CommandRegistry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

using P = CommandParam;
using T = CommandParamType;

juce::var parse(const char* json) {
  juce::var value;
  EXPECT_TRUE(juce::JSON::parse(json, value).wasOk()) << json;
  return value;
}

// A registry with one command taking every kind of argument (it answers with
// the arguments it was handed), plus two trivial ones.
struct TestRegistry {
  TestRegistry() {
    registry.add({
        "test.echo",
        "Answers with its validated arguments.",
        {P::required("text", T::string, "Any text."),
         P::optional("level", T::number, "A level.").range(0.0, 1.0),
         P::optional("count", T::integer, "A count.").atLeast(1),
         P::optional("flag", T::boolean, "A flag."),
         P::optional("extra", T::object, "A nested object."),
         P::optional("names", T::array, "Some names.").items(T::string),
         P::optional("mode", T::string, "A mode.").oneOf({"slow", "fast"}),
         P::optional("size", T::integer, "A size.").range(1, 8).withDefault(4)},
        false,
        CommandPermission::read,
        [this](const juce::var& args) {
          ++echoCalls;
          return CommandResult::success(args);
        },
    });
    registry.add({
        "test.bump",
        "Counts how often it ran.",
        {},
        true,
        CommandPermission::edit,
        [this](const juce::var&) {
          ++bumpCalls;
          juce::DynamicObject::Ptr obj = new juce::DynamicObject();
          obj->setProperty("calls", bumpCalls.load());
          return CommandResult::success(juce::var(obj.get()));
        },
    });
    registry.add({
        "another.fail",
        "Always refuses.",
        {},
        true,
        CommandPermission::edit,
        [](const juce::var&) {
          return CommandResult::failure(CommandError::failed, "refused");
        },
    });
  }

  CommandResult echo(const char* argsJson) { return registry.run("test.echo", parse(argsJson)); }

  CommandRegistry registry;
  std::atomic<int> echoCalls{0}, bumpCalls{0};
};

void expectInvalid(const CommandResult& result, const char* mentions) {
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(CommandError::invalidArgs))
      << result.errorMessage.toStdString();
  EXPECT_TRUE(result.errorMessage.contains(mentions))
      << "message does not name " << mentions << ": " << result.errorMessage.toStdString();
}

}  // namespace

TEST(CommandRegistryTest, ValidArgumentsReachTheHandlerNormalised) {
  TestRegistry t;
  const CommandResult result = t.echo(
      R"({"text":"hi","level":0.5,"count":3.0,"flag":true,"extra":{"a":1},"names":["x","y"],"mode":"fast"})");
  ASSERT_TRUE(result.ok) << result.errorMessage.toStdString();
  EXPECT_EQ(t.echoCalls.load(), 1);

  EXPECT_EQ(result.value["text"].toString(), juce::String("hi"));
  EXPECT_DOUBLE_EQ(static_cast<double>(result.value["level"]), 0.5);
  // 3.0 is an integer in JSON terms and arrives as an int.
  EXPECT_TRUE(result.value["count"].isInt());
  EXPECT_EQ(static_cast<int>(result.value["count"]), 3);
  EXPECT_TRUE(static_cast<bool>(result.value["flag"]));
  EXPECT_EQ(static_cast<int>(result.value["extra"]["a"]), 1);
  EXPECT_EQ(result.value["names"].size(), 2);
  // The absent optional with a default is filled in; one without is absent.
  EXPECT_EQ(static_cast<int>(result.value["size"]), 4);

  const CommandResult minimal = t.echo(R"({"text":"only"})");
  ASSERT_TRUE(minimal.ok);
  EXPECT_TRUE(minimal.value["level"].isVoid());
  EXPECT_TRUE(minimal.value["mode"].isVoid());

  // An explicit null is the same as leaving an optional argument out.
  const CommandResult withNull = t.echo(R"({"text":"only","level":null,"size":null})");
  ASSERT_TRUE(withNull.ok) << withNull.errorMessage.toStdString();
  EXPECT_TRUE(withNull.value["level"].isVoid());
  EXPECT_EQ(static_cast<int>(withNull.value["size"]), 4);
}

TEST(CommandRegistryTest, UnknownCommand) {
  TestRegistry t;
  const CommandResult result = t.registry.run("test.missing", parse("{}"));
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(CommandError::unknownCommand));
  EXPECT_TRUE(result.errorMessage.contains("test.missing"));
}

TEST(CommandRegistryTest, MissingRequiredArgument) {
  TestRegistry t;
  expectInvalid(t.echo("{}"), "'text'");
  expectInvalid(t.echo(R"({"text":null})"), "'text'");
  // No argument object at all is an empty one.
  expectInvalid(t.registry.run("test.echo"), "'text'");
  EXPECT_EQ(t.echoCalls.load(), 0);
}

TEST(CommandRegistryTest, WrongTypeNamesTheArgument) {
  TestRegistry t;
  expectInvalid(t.echo(R"({"text":5})"), "'text'");
  expectInvalid(t.echo(R"({"text":"a","level":"loud"})"), "'level'");
  expectInvalid(t.echo(R"({"text":"a","level":true})"), "'level'");
  expectInvalid(t.echo(R"({"text":"a","count":2.5})"), "'count'");
  expectInvalid(t.echo(R"({"text":"a","count":"2"})"), "'count'");
  expectInvalid(t.echo(R"({"text":"a","flag":1})"), "'flag'");
  expectInvalid(t.echo(R"({"text":"a","extra":[1]})"), "'extra'");
  expectInvalid(t.echo(R"({"text":"a","names":"x"})"), "'names'");
  expectInvalid(t.echo(R"({"text":"a","names":["x",2]})"), "'names'");
  EXPECT_EQ(t.echoCalls.load(), 0) << "a handler saw invalid input";
}

TEST(CommandRegistryTest, EnumAndBoundsAreEnforced) {
  TestRegistry t;
  expectInvalid(t.echo(R"({"text":"a","mode":"medium"})"), "'mode'");
  expectInvalid(t.echo(R"({"text":"a","level":1.5})"), "'level'");
  expectInvalid(t.echo(R"({"text":"a","level":-0.1})"), "'level'");
  expectInvalid(t.echo(R"({"text":"a","count":0})"), "'count'");
  expectInvalid(t.echo(R"({"text":"a","size":9})"), "'size'");
  EXPECT_EQ(t.echoCalls.load(), 0);

  // The bounds themselves are inside.
  EXPECT_TRUE(t.echo(R"({"text":"a","level":0,"count":1,"size":8,"mode":"slow"})").ok);
  EXPECT_TRUE(t.echo(R"({"text":"a","level":1.0})").ok);
}

TEST(CommandRegistryTest, UnknownArgumentNameAndNonObjectArgs) {
  TestRegistry t;
  expectInvalid(t.echo(R"({"text":"a","volume":3})"), "'volume'");
  expectInvalid(t.registry.run("test.echo", parse(R"(["a"])")), "args");
  expectInvalid(t.registry.run("test.echo", juce::var("text")), "args");
  // A command without parameters still rejects stray arguments.
  expectInvalid(t.registry.run("test.bump", parse(R"({"n":1})")), "'n'");
  EXPECT_EQ(t.echoCalls.load(), 0);
  EXPECT_EQ(t.bumpCalls.load(), 0);
}

TEST(CommandRegistryTest, ResultEnvelope) {
  TestRegistry t;
  const juce::var okVar = t.registry.run("test.bump").toVar();
  EXPECT_TRUE(static_cast<bool>(okVar["ok"]));
  EXPECT_EQ(static_cast<int>(okVar["result"]["calls"]), 1);
  EXPECT_TRUE(okVar["error"].isVoid());

  const juce::var failVar = t.registry.run("another.fail").toVar();
  EXPECT_FALSE(static_cast<bool>(failVar["ok"]));
  EXPECT_EQ(failVar["error"]["code"].toString(), juce::String("failed"));
  EXPECT_EQ(failVar["error"]["message"].toString(), juce::String("refused"));
  EXPECT_TRUE(failVar["result"].isVoid());
}

TEST(CommandRegistryTest, DescribeListsEveryCommandSortedWithItsSpec) {
  TestRegistry t;
  const juce::var all = t.registry.describe();
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0]["name"].toString(), juce::String("another.fail"));
  EXPECT_EQ(all[1]["name"].toString(), juce::String("test.bump"));
  EXPECT_EQ(all[2]["name"].toString(), juce::String("test.echo"));

  const juce::var bump = all[1];
  EXPECT_EQ(bump["summary"].toString(), juce::String("Counts how often it ran."));
  EXPECT_TRUE(static_cast<bool>(bump["mutates"]));
  EXPECT_EQ(bump["params"].size(), 0);

  const juce::var echo = t.registry.describe("test.echo");
  EXPECT_FALSE(static_cast<bool>(echo["mutates"]));
  ASSERT_EQ(echo["params"].size(), 8);

  const juce::var text = echo["params"][0];
  EXPECT_EQ(text["name"].toString(), juce::String("text"));
  EXPECT_EQ(text["type"].toString(), juce::String("string"));
  EXPECT_TRUE(static_cast<bool>(text["required"]));
  EXPECT_EQ(text["description"].toString(), juce::String("Any text."));
  EXPECT_TRUE(text["enum"].isVoid());
  EXPECT_TRUE(text["min"].isVoid());

  const juce::var level = echo["params"][1];
  EXPECT_EQ(level["type"].toString(), juce::String("number"));
  EXPECT_FALSE(static_cast<bool>(level["required"]));
  EXPECT_DOUBLE_EQ(static_cast<double>(level["min"]), 0.0);
  EXPECT_DOUBLE_EQ(static_cast<double>(level["max"]), 1.0);

  const juce::var count = echo["params"][2];
  EXPECT_EQ(count["type"].toString(), juce::String("integer"));
  EXPECT_EQ(static_cast<int>(count["min"]), 1);
  EXPECT_TRUE(count["max"].isVoid());

  EXPECT_EQ(echo["params"][3]["type"].toString(), juce::String("boolean"));
  EXPECT_EQ(echo["params"][4]["type"].toString(), juce::String("object"));
  EXPECT_EQ(echo["params"][5]["type"].toString(), juce::String("array"));
  EXPECT_EQ(echo["params"][5]["items"].toString(), juce::String("string"));

  const juce::var mode = echo["params"][6];
  ASSERT_EQ(mode["enum"].size(), 2);
  EXPECT_EQ(mode["enum"][0].toString(), juce::String("slow"));
  EXPECT_EQ(mode["enum"][1].toString(), juce::String("fast"));

  EXPECT_EQ(static_cast<int>(echo["params"][7]["default"]), 4);

  EXPECT_TRUE(t.registry.describe("test.missing").isVoid());
  EXPECT_TRUE(t.registry.contains("test.echo"));
  EXPECT_FALSE(t.registry.contains("test.missing"));

  // The description is itself JSON a transport can ship as is.
  juce::var reparsed;
  EXPECT_TRUE(juce::JSON::parse(juce::JSON::toString(all), reparsed).wasOk());
  EXPECT_EQ(reparsed.size(), 3);
}

TEST(CommandRegistryTest, MarkdownReferenceHasASectionPerCommand) {
  TestRegistry t;
  const juce::String markdown = t.registry.describeMarkdown("Test reference", "Made by a test.");
  EXPECT_TRUE(markdown.startsWith("# Test reference\n"));
  EXPECT_TRUE(markdown.contains("Made by a test."));
  EXPECT_TRUE(markdown.contains("\n## another.fail\n"));
  EXPECT_TRUE(markdown.contains("\n## test.bump\n"));
  EXPECT_TRUE(markdown.contains("\n## test.echo\n"));
  EXPECT_TRUE(markdown.contains("| `mode` | string | no | one of `slow`, `fast` | A mode. |"));
  EXPECT_TRUE(markdown.contains("| `level` | number | no | 0 to 1 | A level. |"));
  EXPECT_TRUE(markdown.contains("| `names` | array of string | no |  | Some names. |"));
  EXPECT_TRUE(markdown.contains("| `size` | integer | no | 1 to 8; default `4` | A size. |"));
  EXPECT_TRUE(markdown.contains("No parameters."));
  EXPECT_TRUE(markdown.endsWith("\n"));
  EXPECT_FALSE(markdown.contains("\r"));
}

TEST(CommandRegistryTest, RunJsonRoundTrip) {
  TestRegistry t;
  juce::var reply = parse(
      t.registry.runJson(R"({"command":"test.echo","args":{"text":"hello","count":2}})").toRawUTF8());
  EXPECT_TRUE(static_cast<bool>(reply["ok"]));
  EXPECT_EQ(reply["result"]["text"].toString(), juce::String("hello"));
  EXPECT_EQ(static_cast<int>(reply["result"]["count"]), 2);

  // No "args" key is an empty argument object; extra top-level keys (a
  // transport's request id) are ignored.
  reply = parse(t.registry.runJson(R"({"id":7,"command":"test.bump"})").toRawUTF8());
  EXPECT_TRUE(static_cast<bool>(reply["ok"]));
  EXPECT_EQ(static_cast<int>(reply["result"]["calls"]), 1);

  reply = parse(t.registry.runJson(R"({"command":"test.echo","args":{}})").toRawUTF8());
  EXPECT_FALSE(static_cast<bool>(reply["ok"]));
  EXPECT_EQ(reply["error"]["code"].toString(), juce::String("invalid_args"));
  EXPECT_TRUE(reply["error"]["message"].toString().contains("'text'"));

  reply = parse(t.registry.runJson(R"({"command":"nope"})").toRawUTF8());
  EXPECT_EQ(reply["error"]["code"].toString(), juce::String("unknown_command"));

  // Requests that are not a command object at all.
  for (const char* bad : {"{not json", "[1,2]", R"({"args":{}})", R"({"command":5})", ""}) {
    reply = parse(t.registry.runJson(bad).toRawUTF8());
    EXPECT_FALSE(static_cast<bool>(reply["ok"])) << bad;
    EXPECT_EQ(reply["error"]["code"].toString(), juce::String("invalid_args")) << bad;
  }

  // One line, so a line-delimited transport can frame replies by newline.
  EXPECT_FALSE(t.registry.runJson(R"({"command":"test.bump"})").containsChar('\n'));
}

TEST(CommandRegistryTest, RunFromAnyThreadMarshalsOntoTheMessageThread) {
  TestRegistry t;

  // On the message thread it is a plain run.
  EXPECT_TRUE(t.registry.runFromAnyThread("test.bump", {}, 1000).ok);
  EXPECT_EQ(t.bumpCalls.load(), 1);

  std::atomic<bool> done{false};
  CommandResult result;
  std::thread worker([&] {
    result = t.registry.runFromAnyThread("test.echo", parse(R"({"text":"from afar"})"), 10000);
    done.store(true);
  });
  // The test's main thread is the message thread; the command runs when it
  // dispatches.
  const auto deadline = juce::Time::getMillisecondCounter() + 10000;
  while (!done.load() && juce::Time::getMillisecondCounter() < deadline)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
  worker.join();

  ASSERT_TRUE(result.ok) << result.errorMessage.toStdString();
  EXPECT_EQ(result.value["text"].toString(), juce::String("from afar"));
  EXPECT_EQ(t.echoCalls.load(), 1);
}

TEST(CommandRegistryTest, RunFromAnyThreadTimesOutWithoutRunningLater) {
  TestRegistry t;

  // Nothing dispatches while the worker waits, as with a message thread
  // stuck in a long call.
  CommandResult result;
  std::thread worker([&] { result = t.registry.runFromAnyThread("test.bump", {}, 50); });
  worker.join();

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.errorCode, juce::String(CommandError::unavailable));

  // The caller was told it did not happen, so it must not happen once the
  // message thread comes back.
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  EXPECT_EQ(t.bumpCalls.load(), 0);
}
