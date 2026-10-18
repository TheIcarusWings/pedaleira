#include "TrainerRunner.h"

#include "PresetJson.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kLogLines = 400;
constexpr juce::int64 kMaxNamBytes = 256LL * 1024 * 1024;
constexpr int kInstallCheckSeconds = 90;

juce::var object() { return juce::var(new juce::DynamicObject()); }

// "0.13.0", "v0.13.0", "0.13.0.dev1", "0.13.0rc1" -> {0, 13, 0}; false when
// there are no three leading numbers.
bool parseVersion(const juce::String& text, int (&parts)[3]) {
  juce::String s = text.trim();
  if (s.startsWithIgnoreCase("v"))
    s = s.substring(1);
  auto at = s.getCharPointer();
  for (int part = 0; part < 3; ++part) {
    if (!at.isDigit())
      return false;
    int value = 0;
    while (at.isDigit()) {
      value = value * 10 + static_cast<int>(*at - '0');
      ++at;
    }
    parts[part] = value;
    if (part < 2) {
      if (*at != '.')
        return false;
      ++at;
    }
  }
  return true;
}

// A line read from the trainer: the part after `tag` as JSON, or void.
juce::var payloadAfter(const juce::String& line, const char* tag) {
  const int at = line.indexOf(tag);
  if (at < 0)
    return {};
  juce::var parsed;
  if (juce::JSON::parse(line.substring(at + static_cast<int>(std::strlen(tag))).trim(), parsed)
          .failed())
    return {};
  return parsed;
}

// Where the top-level "metadata" object of a NAM file starts and ends (the
// positions of its braces), found without parsing the weights again.
bool findTopLevelMetadata(const juce::String& text, int& begin, int& end) {
  int depth = 0;
  bool inString = false;
  int stringStart = -1;
  juce::String lastKey;
  const int length = text.length();
  auto chars = text.getCharPointer();
  for (int i = 0; i < length; ++i) {
    const juce::juce_wchar c = chars[i];
    if (inString) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        inString = false;
        if (depth == 1)
          lastKey = text.substring(stringStart + 1, i);
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      stringStart = i;
    } else if (c == '{' || c == '[') {
      if (c == '{' && depth == 1 && lastKey == "metadata") {
        // Walk to the matching brace.
        int inner = 0;
        bool innerString = false;
        for (int j = i; j < length; ++j) {
          const juce::juce_wchar d = chars[j];
          if (innerString) {
            if (d == '\\')
              ++j;
            else if (d == '"')
              innerString = false;
          } else if (d == '"') {
            innerString = true;
          } else if (d == '{') {
            ++inner;
          } else if (d == '}' && --inner == 0) {
            begin = i;
            end = j;
            return true;
          }
        }
        return false;
      }
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return false;
}

}  // namespace

// ####################
// INSTALL AND VERSIONS
// ####################
juce::String TrainerRunner::defaultPython() {
#if JUCE_WINDOWS
  return "python";
#else
  return "python3";
#endif
}

bool TrainerRunner::versionAtLeast(const juce::String& version, const juce::String& minimum) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  if (!parseVersion(version, a) || !parseVersion(minimum, b))
    return false;
  for (int i = 0; i < 3; ++i)
    if (a[i] != b[i])
      return a[i] > b[i];
  return true;
}

const char* TrainerRunner::esrBand(double esr) {
  if (esr < 0.01) return "great";
  if (esr < 0.035) return "notBad";
  if (esr < 0.1) return "mightBeOkay";
  if (esr < 0.3) return "poor";
  return "failed";
}

const char* TrainerRunner::esrBandLabel(double esr) {
  if (esr < 0.01) return "Great";
  if (esr < 0.035) return "Not bad";
  if (esr < 0.1) return "Might be okay";
  if (esr < 0.3) return "Poor";
  return "Failed";
}

TrainerRunner::Install TrainerRunner::checkInstall(const juce::String& pythonIn,
                                                   const std::atomic<bool>* cancel) {
  Install install;
  install.python = pythonIn.trim().isEmpty() ? defaultPython() : pythonIn.trim();
  juce::ChildProcess process;
  if (!process.start(juce::StringArray{install.python, "-c", "import nam; print(nam.__version__)"},
                     juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
    install.message = "Python was not found (tried '" + install.python +
                      "'). Install Python 3.9 or newer and then the trainer with "
                      "'pip install neural-amp-modeler', or set the Python executable in the "
                      "capture settings.";
    return install;
  }
  install.pythonFound = true;
  const auto deadline = juce::Time::getMillisecondCounter() +
                        static_cast<juce::uint32>(kInstallCheckSeconds * 1000);
  while (process.isRunning()) {
    if ((cancel != nullptr && cancel->load()) || juce::Time::getMillisecondCounter() > deadline) {
      process.kill();
      install.message = "Checking the trainer took too long or was cancelled.";
      return install;
    }
    juce::Thread::sleep(20);
  }
  const juce::String output = process.readAllProcessOutput();
  const auto lines = juce::StringArray::fromLines(output);
  // The version is the last line that looks like one; warnings may come first.
  for (int i = lines.size(); --i >= 0;) {
    int parts[3];
    if (parseVersion(lines[i], parts)) {
      install.version = lines[i].trim();
      break;
    }
  }
  if (install.version.isEmpty()) {
    if (output.contains("No module named 'nam'") || output.contains("No module named nam"))
      install.message = "The neural-amp-modeler package is not installed for this Python "
                        "(" + install.python + "). Install it with 'pip install "
                        "neural-amp-modeler', version " + juce::String(kMinimumVersion) +
                        " or newer.";
    else
      install.message = "Importing the trainer failed: " +
                        (lines.isEmpty() ? juce::String("no output")
                                         : lines[lines.size() - 1].trim());
    return install;
  }
  install.packageFound = true;
  install.versionOk = versionAtLeast(install.version, kMinimumVersion);
  if (!install.versionOk)
    install.message = "The trainer is version " + install.version + ". Pedaleira needs " +
                      kMinimumVersion + " or newer: older versions do not make NAM A2 files. "
                      "Upgrade with 'pip install --upgrade neural-amp-modeler'.";
  return install;
}

// ####################
// NAM FILES
// ####################
namespace NamFile {

juce::Result read(const juce::File& file, Info& info) {
  info = {};
  if (!file.existsAsFile())
    return juce::Result::fail("no model file at " + file.getFullPathName());
  if (file.getSize() > kMaxNamBytes)
    return juce::Result::fail("the model file is too large to read");
  juce::var parsed;
  // Guarded: this is a file from outside, and the parse is recursive.
  if (const auto result =
          t3k::presetjson::parseJsonGuarded(file.loadFileAsString(), parsed, 64);
      result.failed())
    return juce::Result::fail("the model file is not valid JSON: " + result.getErrorMessage());
  if (parsed.getDynamicObject() == nullptr)
    return juce::Result::fail("the model file is not a NAM file (no JSON object)");
  info.architecture = parsed["architecture"].toString();
  info.version = parsed["version"].toString();
  info.metadata = parsed["metadata"];
  info.sampleRate = static_cast<double>(parsed["sample_rate"]);
  const juce::var training = info.metadata["training"];
  if (const juce::var esr = training["validation_esr"]; esr.isDouble() || esr.isInt()) {
    info.hasEsr = true;
    info.validationEsr = static_cast<double>(esr);
  }
  if (info.architecture.isEmpty())
    return juce::Result::fail("the file has no architecture, so it is not a NAM model");
  return juce::Result::ok();
}

juce::Result requireA2(const Info& info) {
  if (info.architecture == "SlimmableContainer")
    return juce::Result::ok();
  return juce::Result::fail(
      "The trainer produced a '" + info.architecture +
      "' model, not NAM A2 (architecture SlimmableContainer), which is what the capture "
      "wizard trains. "
      "Use neural-amp-modeler " + juce::String(TrainerRunner::kMinimumVersion) +
      " or newer: pip install --upgrade neural-amp-modeler.");
}

bool metadataHas(const Info& info, const juce::var& updates) {
  const auto* object = updates.getDynamicObject();
  if (object == nullptr)
    return true;
  for (const auto& property : object->getProperties()) {
    if (property.value.isVoid())
      continue;
    const juce::var present = info.metadata[property.name];
    if (present.isVoid() || present.isUndefined() || present.toString() != property.value.toString())
      return false;
  }
  return true;
}

juce::Result writeCopyWithMetadata(const juce::File& source, const juce::File& destination,
                                   const juce::var& updates) {
  Info info;
  if (const auto result = read(source, info); result.failed())
    return result;
  const juce::String text = source.loadFileAsString();
  int begin = 0, end = 0;
  if (!findTopLevelMetadata(text, begin, end))
    return juce::Result::fail("the model file has no metadata object to add to");

  juce::var merged;
  if (juce::JSON::parse(text.substring(begin, end + 1), merged).failed() ||
      merged.getDynamicObject() == nullptr)
    return juce::Result::fail("the model file's metadata could not be read");
  if (const auto* object = updates.getDynamicObject())
    for (const auto& property : object->getProperties())
      if (!property.value.isVoid())
        merged.getDynamicObject()->setProperty(property.name, property.value);

  const juce::String rebuilt = text.substring(0, begin) + juce::JSON::toString(merged, true) +
                               text.substring(end + 1);
  // Written beside the destination and moved over it, so a reader never sees
  // half a model.
  if (const auto written = t3k::presetjson::writeFileAtomically(destination, rebuilt);
      written.failed())
    return juce::Result::fail("could not write " + destination.getFullPathName() + ": " +
                              written.getErrorMessage());
  return juce::Result::ok();
}

}  // namespace NamFile

// ####################
// THE RUN
// ####################
TrainerRunner::~TrainerRunner() {
  cancel();
  if (worker.joinable())
    worker.join();
}

bool TrainerRunner::isActive() const { return active.load(); }

juce::String TrainerRunner::state() const {
  const std::lock_guard<std::mutex> lock(mutex);
  return stateName;
}

void TrainerRunner::setState(const char* state, const juce::String& text) {
  const std::lock_guard<std::mutex> lock(mutex);
  stateName = state;
  if (text.isNotEmpty())
    message = text;
}

juce::Result TrainerRunner::start(const Job& job) {
  if (active.load())
    return juce::Result::fail("a training run is already going");
  if (worker.joinable())
    worker.join();
  if (!job.workDir.createDirectory().wasOk() || !job.outDir.createDirectory().wasOk())
    return juce::Result::fail("could not make the training folder");

  // The helper and the job beside it, so a user can run the same thing by hand.
  const juce::File script = job.workDir.getChildFile("train_helper.py");
  if (!script.replaceWithText(helperScriptSource(), false, false, "\n"))
    return juce::Result::fail("could not write the training helper");

  juce::var spec = object();
  auto* o = spec.getDynamicObject();
  o->setProperty("mode", job.mode);
  o->setProperty("input", job.input.getFullPathName());
  o->setProperty("output", job.output.getFullPathName());
  o->setProperty("outdir", job.outDir.getFullPathName());
  o->setProperty("modelName", job.modelName);
  o->setProperty("epochs", job.epochs);
  if (job.latency.has_value())
    o->setProperty("latency", *job.latency);
  o->setProperty("ignoreChecks", job.ignoreChecks);
  o->setProperty("validationSamples", job.validationSamples);
  o->setProperty("metadata", job.metadata);
  if (!job.workDir.getChildFile("job.json").replaceWithText(juce::JSON::toString(spec, false), false, false, "\n"))
    return juce::Result::fail("could not write the training job");

  {
    const std::lock_guard<std::mutex> lock(mutex);
    stateName = job.checkInstall ? "checking" : "running";
    message = job.checkInstall ? "Checking the trainer" : "Starting the trainer";
    error.clear();
    epoch = 0;
    epochs = job.epochs;
    esr = -1.0;
    hasLatency = hasChecks = checksPassed = false;
    latencySamples = 0;
    modelFile.clear();
    architecture.clear();
    log.clear();
    resultLine = juce::var();
    started = juce::Time::getCurrentTime();
    finished = juce::Time();
  }
  cancelRequested.store(false);
  active.store(true);
  worker = std::thread([this, job] { run(job); });
  return juce::Result::ok();
}

void TrainerRunner::cancel() {
  cancelRequested.store(true);
  const std::lock_guard<std::mutex> lock(mutex);
  if (activeProcess != nullptr)
    activeProcess->kill();
}

void TrainerRunner::noteLine(const juce::String& line, int totalEpochs) {
  const juce::String text = line.trim();
  if (text.isEmpty())
    return;
  const std::lock_guard<std::mutex> lock(mutex);
  // The progress bar redraws one line over and over; keeping each draw would
  // push everything else out of the log.
  const bool barRedraw = text.startsWith("Epoch ") && text.contains("|");
  if (!barRedraw) {
    log.push_back(text.substring(0, 400));
    while (static_cast<int>(log.size()) > kLogLines)
      log.pop_front();
  }

  if (const juce::var p = payloadAfter(text, "PEDALEIRA_EPOCH"); p.getDynamicObject() != nullptr) {
    epoch = std::max(epoch, static_cast<int>(p["epoch"]));
    if (static_cast<int>(p["epochs"]) > 0)
      epochs = static_cast<int>(p["epochs"]);
    if (p["esr"].isDouble())
      esr = static_cast<double>(p["esr"]);
    return;
  }
  if (const juce::var p = payloadAfter(text, "PEDALEIRA_RESULT"); p.getDynamicObject() != nullptr) {
    resultLine = p;
    return;
  }
  // The lightning progress bar: "Epoch 12:  37%|...". Its numbering starts at 0.
  if (text.startsWith("Epoch ") && juce::CharacterFunctions::isDigit(text[6])) {
    // Epoch `number` is under way, so `number` are done.
    const int number = text.substring(6).getIntValue();
    epoch = std::max(epoch, std::min(number, totalEpochs));
  }
  if (text.startsWith("Delay based on average is")) {
    latencySamples = text.getTrailingIntValue();
    hasLatency = true;
  }
}

void TrainerRunner::run(Job job) {
  struct Done {
    TrainerRunner& owner;
    ~Done() { owner.active.store(false); }
  } done{*this};

  const juce::String python = job.python.trim().isEmpty() ? defaultPython() : job.python.trim();

  if (job.checkInstall) {
    const Install install = checkInstall(python, &cancelRequested);
    if (cancelRequested.load()) {
      setState("cancelled", "Cancelled");
      return;
    }
    if (!install.versionOk) {
      {
        const std::lock_guard<std::mutex> lock(mutex);
        error = install.message;
        finished = juce::Time::getCurrentTime();
      }
      setState("failed", install.message);
      return;
    }
    setState("running", "Training with neural-amp-modeler " + install.version);
  }

  juce::ChildProcess process;
  // -u: unbuffered, so the trainer's own lines arrive as they are printed.
  const juce::StringArray args{python, "-u",
                               job.workDir.getChildFile("train_helper.py").getFullPathName(),
                               "--job", job.workDir.getChildFile("job.json").getFullPathName(),
                               "--outdir", job.outDir.getFullPathName()};
  if (!process.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
    const juce::String text = "Could not start Python ('" + python + "').";
    {
      const std::lock_guard<std::mutex> lock(mutex);
      error = text;
      finished = juce::Time::getCurrentTime();
    }
    setState("failed", text);
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex);
    activeProcess = &process;
  }
  if (cancelRequested.load())
    process.kill();

  // Lines end in \n, or in \r when a progress bar redraws itself. Read a byte
  // at a time: the read behind this is an fread, which waits for a full
  // buffer, so a larger request would hold progress back until it filled.
  std::string pending;
  for (;;) {
    char c = 0;
    if (process.readProcessOutput(&c, 1) <= 0)
      break;
    if (c == '\n' || c == '\r') {
      noteLine(juce::String::fromUTF8(pending.data(), static_cast<int>(pending.size())),
               job.epochs);
      pending.clear();
    } else if (pending.size() < (1u << 16)) {
      pending.push_back(c);
    }
  }
  if (!pending.empty())
    noteLine(juce::String::fromUTF8(pending.data(), static_cast<int>(pending.size())), job.epochs);

  process.waitForProcessToFinish(10000);
  const int exitCode = static_cast<int>(process.getExitCode());
  {
    const std::lock_guard<std::mutex> lock(mutex);
    activeProcess = nullptr;
  }
  finishFromProcess(job, exitCode, cancelRequested.load());
}

void TrainerRunner::finishFromProcess(const Job& job, int exitCode, bool cancelled) {
  juce::var result;
  juce::StringArray tail;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    result = resultLine;
    for (size_t i = log.size() > 8 ? log.size() - 8 : 0; i < log.size(); ++i)
      tail.add(log[i]);
    finished = juce::Time::getCurrentTime();
  }

  const auto fail = [this](const juce::String& text) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      error = text;
    }
    setState("failed", text);
  };

  if (cancelled) {
    setState("cancelled", "Training cancelled");
    return;
  }

  const bool reportedOk = result.getDynamicObject() != nullptr && static_cast<bool>(result["ok"]);
  if (result.getDynamicObject() != nullptr) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (result["esr"].isDouble())
      esr = static_cast<double>(result["esr"]);
    if (result["latency"].isInt() || result["latency"].isDouble() || result["latency"].isInt64()) {
      latencySamples = static_cast<int>(result["latency"]);
      hasLatency = true;
    }
    if (!result["checksPassed"].isVoid()) {
      hasChecks = true;
      checksPassed = static_cast<bool>(result["checksPassed"]);
    }
  }

  if (!reportedOk) {
    juce::String why = result.getDynamicObject() != nullptr ? result["message"].toString()
                                                            : juce::String();
    if (result["reason"].toString() == "checks")
      why = "The trainer's own data checks failed" +
            (why.isNotEmpty() ? ": " + why : juce::String(".")) +
            " Record again, or train anyway at your own risk.";
    if (why.isEmpty())
      why = "The trainer stopped (exit code " + juce::String(exitCode) + ")" +
            (tail.isEmpty() ? juce::String(".") : ": " + tail[tail.size() - 1]);
    fail(why);
    return;
  }

  // The model: where the helper says it is, else the newest .nam in the folder.
  juce::File model;
  if (const juce::String named = result["modelFile"].toString(); named.isNotEmpty())
    model = juce::File(named);
  if (!model.existsAsFile()) {
    juce::Time newest;
    for (const auto& entry : juce::RangedDirectoryIterator(job.outDir, true, "*.nam")) {
      if (entry.getModificationTime() >= newest) {
        newest = entry.getModificationTime();
        model = entry.getFile();
      }
    }
  }
  if (!model.existsAsFile()) {
    fail("The trainer finished but wrote no model file in " + job.outDir.getFullPathName() + ".");
    return;
  }

  NamFile::Info info;
  if (const auto read = NamFile::read(model, info); read.failed()) {
    fail("The trainer's output could not be read: " + read.getErrorMessage());
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(mutex);
    architecture = info.architecture;
    modelFile = model.getFullPathName();
    // The helper reports the validation ESR; the file's own training record
    // is the fallback.
    if (esr < 0.0 && info.hasEsr)
      esr = info.validationEsr;
  }
  if (const auto a2 = NamFile::requireA2(info); a2.failed()) {
    fail(a2.getErrorMessage());
    return;
  }

  juce::String note = "Training finished";
  {
    const std::lock_guard<std::mutex> lock(mutex);
    epoch = std::max(epoch, epochs);
    if (esr >= 0.0)
      note << ": " << esrBandLabel(esr) << " (ESR " << juce::String(esr, 4) << ")";
  }
  setState("done", note);
}

juce::StringArray TrainerRunner::lastLines(int count) const {
  const std::lock_guard<std::mutex> lock(mutex);
  juce::StringArray lines;
  const size_t from = log.size() > static_cast<size_t>(count) ? log.size() - static_cast<size_t>(count) : 0;
  for (size_t i = from; i < log.size(); ++i)
    lines.add(log[i]);
  return lines;
}

juce::var TrainerRunner::status() const {
  const std::lock_guard<std::mutex> lock(mutex);
  juce::var out = object();
  auto* o = out.getDynamicObject();
  o->setProperty("state", stateName);
  o->setProperty("epoch", epoch);
  o->setProperty("epochs", epochs);
  const double progress = epochs > 0 ? std::min(1.0, static_cast<double>(epoch) / epochs) : 0.0;
  o->setProperty("progress", stateName == "done" ? 1.0 : progress);
  const juce::Time end = finished.toMilliseconds() > 0 ? finished : juce::Time::getCurrentTime();
  const double elapsed = started.toMilliseconds() > 0
                             ? static_cast<double>((end - started).inSeconds())
                             : 0.0;
  o->setProperty("elapsedSeconds", std::round(elapsed * 10.0) / 10.0);
  if (stateName == "running" && epoch > 0 && epoch < epochs)
    o->setProperty("secondsLeft", std::round(elapsed / epoch * (epochs - epoch)));
  if (esr >= 0.0) {
    o->setProperty("esr", esr);
    o->setProperty("esrBand", esrBand(esr));
    o->setProperty("esrBandLabel", esrBandLabel(esr));
  }
  if (hasLatency)
    o->setProperty("latencySamples", latencySamples);
  if (hasChecks)
    o->setProperty("trainerChecksPassed", checksPassed);
  if (modelFile.isNotEmpty())
    o->setProperty("modelFile", modelFile);
  if (architecture.isNotEmpty())
    o->setProperty("architecture", architecture);
  o->setProperty("message", message);
  if (error.isNotEmpty())
    o->setProperty("error", error);
  juce::Array<juce::var> lines;
  const size_t from = log.size() > 12 ? log.size() - 12 : 0;
  for (size_t i = from; i < log.size(); ++i)
    lines.add(log[i]);
  o->setProperty("log", lines);
  return out;
}

// ####################
// THE HELPER SCRIPT
// ####################
// Python, started by the runner. It uses only the trainer's public entry points
// (nam.train.core.train for a re-amp with the standard input file; the full
// trainer for a dry/wet pair) and reports on its output in lines the runner
// reads:
//   PEDALEIRA_EPOCH  {"epoch": n, "epochs": m, "esr": x}
//   PEDALEIRA_RESULT {"ok": bool, "esr": x, "latency": n, "checksPassed": bool,
//                     "modelFile": path, "reason": s, "message": s}
juce::String TrainerRunner::helperScriptSource() {
  return R"PY(# Pedaleira capture training helper. Written by Pedaleira; do not edit.
# Usage: python train_helper.py --job job.json --outdir DIR
# Needs neural-amp-modeler 0.13.0 or newer (its standard training makes NAM A2).
import argparse
import json
import re
import sys
from pathlib import Path


def say(tag, payload):
    print(tag + " " + json.dumps(payload), flush=True)


def install_progress(modules, epochs):
    """Reports each finished epoch with the validation ESR when there is one."""
    try:
        import pytorch_lightning as pl
    except Exception:
        return

    class Progress(pl.Callback):
        def on_train_epoch_end(self, trainer, module):
            esr = trainer.callback_metrics.get("ESR")
            say("PEDALEIRA_EPOCH", {
                "epoch": int(trainer.current_epoch) + 1,
                "epochs": int(epochs),
                "esr": float(esr) if esr is not None else None,
            })

    for module, name in modules:
        original = getattr(module, name, None)
        if original is None:
            continue

        def patched(*args, original_fn=original, **kwargs):
            callbacks = original_fn(*args, **kwargs)
            callbacks.append(Progress())
            return callbacks

        setattr(module, name, patched)


def train_reamp(job, outdir, user_metadata):
    from nam.train import core
    from nam.train import metadata as nam_metadata

    install_progress([(core, "get_callbacks")], job["epochs"])
    name = job["modelName"]
    out = core.train(
        job["input"],
        job["output"],
        str(outdir),
        epochs=int(job["epochs"]),
        latency=job.get("latency"),
        silent=True,
        save_plot=False,
        modelname=name,
        ignore_checks=bool(job.get("ignoreChecks", False)),
        local=False,
        user_metadata=user_metadata,
    )
    data = out.metadata.data
    latency = None
    if data is not None and data.latency is not None:
        latency = data.latency.manual
        if latency is None:
            latency = data.latency.calibration.recommended
    checks = None if data is None or data.checks is None else bool(data.checks.passed)
    if out.model is None:
        say("PEDALEIRA_RESULT", {
            "ok": False, "reason": "checks", "latency": latency, "checksPassed": checks,
            "message": "the recording did not pass the trainer's data checks",
        })
        return 3
    out.model.net.export(
        outdir,
        basename=name,
        user_metadata=user_metadata,
        other_metadata={nam_metadata.TRAINING_KEY: out.metadata.model_dump()},
    )
    model_file = Path(outdir, name + ".nam")
    say("PEDALEIRA_RESULT", {
        "ok": True, "esr": out.metadata.validation_esr, "latency": latency,
        "checksPassed": checks, "modelFile": str(model_file),
    })
    return 0


def train_dry_wet(job, outdir):
    # A dry/wet pair is not the standard input file, so the trainer's standard
    # path would not recognise it: this goes through the full trainer with
    # configuration made the way the standard path makes it.
    from nam.train import core, full
    from nam.train._version import Version

    install_progress([(full, "_create_callbacks")], job["epochs"])
    latency = int(job.get("latency") or 0)
    data, model, learning = core._get_configs(
        Version(3, 0, 0), job["input"], job["output"], latency, int(job["epochs"]), 8192, 16
    )
    validation = int(job.get("validationSamples", 480000))
    data["train"] = {"ny": 8192, "stop_samples": -validation, "require_input_pre_silence": False}
    data["validation"] = {"ny": None, "start_samples": -validation,
                          "require_input_pre_silence": False}
    run_dir = Path(outdir, "run")
    run_dir.mkdir(parents=True, exist_ok=True)
    full.main(data, model, learning, run_dir, no_show=True, make_plots=False)
    produced = sorted(run_dir.rglob("*.nam"), key=lambda p: p.stat().st_mtime)
    if not produced:
        say("PEDALEIRA_RESULT", {"ok": False, "reason": "export",
                                 "message": "the full trainer wrote no model"})
        return 4
    target = Path(outdir, job["modelName"] + ".nam")
    target.write_bytes(produced[-1].read_bytes())
    # The full trainer does not return its validation ESR; the best checkpoints
    # carry it in their names.
    esrs = []
    for ckpt in run_dir.rglob("*.ckpt"):
        found = re.search(r"ESR=([0-9.eE+-]+)", ckpt.name)
        if found:
            try:
                esrs.append(float(found.group(1).rstrip(".")))
            except ValueError:
                pass
    say("PEDALEIRA_RESULT", {
        "ok": True, "esr": min(esrs) if esrs else None, "latency": latency,
        "checksPassed": None, "modelFile": str(target),
    })
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--job", required=True)
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()
    job = json.load(open(args.job))
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    try:
        import nam  # noqa: F401
        from nam.models.metadata import UserMetadata
    except Exception as error:
        say("PEDALEIRA_RESULT", {"ok": False, "reason": "import", "message": str(error)})
        return 2
    wanted = {k: v for k, v in (job.get("metadata") or {}).items() if v is not None}
    user_metadata = UserMetadata(**wanted)
    try:
        if job["mode"] == "dryWet":
            return train_dry_wet(job, outdir)
        return train_reamp(job, outdir, user_metadata)
    except Exception as error:
        say("PEDALEIRA_RESULT", {"ok": False, "reason": "error", "message": repr(error)})
        raise


if __name__ == "__main__":
    sys.exit(main())
)PY";
}
