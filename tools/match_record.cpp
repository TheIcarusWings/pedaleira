#include "match_record.h"

#include "MatchRecorder.h"
#include "MatchTake.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace MatchRecord {

namespace {

constexpr double kRate = EffectMatch::kSignalRate;
// Seconds recorded after the signal ends (tails and the closing marker).
constexpr double kTailSeconds = 2.0;
// Let the device run before the take starts, so the first buffers are not
// the ones the signal begins on.
constexpr int kSettleMs = 300;

std::string text(const juce::String& s) { return s.toStdString(); }

std::string joinNames(const juce::StringArray& names, int limit = 6) {
  std::string out;
  for (int i = 0; i < names.size(); ++i) {
    if (i == limit) {
      out += ", ... (" + std::to_string(names.size() - limit) + " more)";
      break;
    }
    out += (i > 0 ? ", " : "") + std::string("'") + text(names[i]) + "'";
  }
  return out;
}

// ####################
// DEVICES
// ####################
std::string rateList(const juce::Array<double>& rates) {
  std::string out;
  for (const double r : rates)
    out += (out.empty() ? "" : ", ") + std::to_string(static_cast<int>(std::lround(r)));
  return out.empty() ? "none reported" : out;
}

void printChannels(const char* label, const juce::StringArray& names) {
  std::cout << "    " << label << " (" << names.size() << "):";
  if (names.isEmpty()) {
    std::cout << " none\n";
    return;
  }
  std::cout << "\n";
  for (int i = 0; i < names.size(); ++i)
    std::cout << "      " << (i + 1) << "  " << text(names[i]) << "\n";
}

// The names of the devices of `type` in either direction, once each.
juce::StringArray allDeviceNames(juce::AudioIODeviceType& type) {
  juce::StringArray names = type.getDeviceNames(false);
  names.addArray(type.getDeviceNames(true));
  names.removeDuplicates(false);
  return names;
}

// ####################
// OPENING A DEVICE
// ####################
// Case-insensitive: the full name if there is one, else the only name that
// contains `wanted`. Empty when there is none; throws when it is ambiguous.
juce::String matchName(const juce::StringArray& names, const juce::String& wanted) {
  for (const auto& name : names)
    if (name.equalsIgnoreCase(wanted))
      return name;
  juce::StringArray hits;
  for (const auto& name : names)
    if (name.containsIgnoreCase(wanted))
      hits.add(name);
  if (hits.size() > 1)
    throw std::runtime_error("'" + text(wanted) + "' matches more than one device: " +
                             joinNames(hits) + ". Give more of the name.");
  return hits.isEmpty() ? juce::String() : hits[0];
}

struct Route {
  std::vector<int> outputs;  // 1-based, as given
  std::vector<int> inputs;
};

// An open device with the callback's channel indices worked out. Nothing is
// restored on the machine: the manager is given its setup directly and never
// reads or writes saved state.
class OpenDevice {
 public:
  OpenDevice(const Options& options, const Route& route) {
    if (std::abs(options.rate - kRate) > 0.5)
      throw std::runtime_error("PedaleiraMatch records at 48000 Hz only (the test signal is made "
                               "for it), not " + std::to_string(static_cast<int>(options.rate)) +
                               " Hz. Set the interface to 48 kHz.");
    if (options.device.empty())
      throw std::invalid_argument("missing --device (see 'PedaleiraMatch devices')");

    auto& types = manager_.getAvailableDeviceTypes();
    for (auto* type : types)
      type->scanForDevices();

    // Pick the type: the one asked for, else the first that has the device in
    // both directions.
    juce::AudioIODeviceType* chosen = nullptr;
    juce::String outName, inName;
    const juce::String wanted = juce::String(options.device);
    if (!options.type.empty()) {
      for (auto* type : types)
        if (type->getTypeName().equalsIgnoreCase(juce::String(options.type)))
          chosen = type;
      if (chosen == nullptr) {
        juce::StringArray typeNames;
        for (auto* type : types)
          typeNames.add(type->getTypeName());
        throw std::runtime_error("no audio device type '" + options.type + "'. Types here: " +
                                 joinNames(typeNames));
      }
      outName = matchName(chosen->getDeviceNames(false), wanted);
      inName = matchName(chosen->getDeviceNames(true), wanted);
    } else {
      for (auto* type : types) {
        const juce::String o = matchName(type->getDeviceNames(false), wanted);
        const juce::String i = matchName(type->getDeviceNames(true), wanted);
        if (o.isNotEmpty() && i.isNotEmpty()) {
          chosen = type;
          outName = o;
          inName = i;
          break;
        }
      }
    }
    if (chosen == nullptr || outName.isEmpty() || inName.isEmpty())
      throw std::runtime_error("no audio device '" + options.device +
                               "' with both inputs and outputs" +
                               (options.type.empty() ? "" : " in type '" + options.type + "'") +
                               ". 'PedaleiraMatch devices' lists what is here.");
    typeName_ = text(chosen->getTypeName());

    // Look at the device before opening it: the rate and the channels asked for
    // have to exist.
    {
      std::unique_ptr<juce::AudioIODevice> probe(chosen->createDevice(outName, inName));
      if (probe == nullptr)
        throw std::runtime_error("cannot look at the device '" + text(outName) + "'");
      const bool hasRate = std::any_of(probe->getAvailableSampleRates().begin(),
                                       probe->getAvailableSampleRates().end(),
                                       [](double r) { return std::abs(r - kRate) < 0.5; });
      if (!hasRate)
        throw std::runtime_error("'" + text(outName) + "' does not run at 48000 Hz (it offers " +
                                 rateList(probe->getAvailableSampleRates()) +
                                 "). Set the interface to 48 kHz.");
      const int outCount = probe->getOutputChannelNames().size();
      const int inCount = probe->getInputChannelNames().size();
      for (const int c : route.outputs)
        if (c > outCount)
          throw std::runtime_error("--out-ch " + std::to_string(c) + ": the device has " +
                                   std::to_string(outCount) + " output channels");
      for (const int c : route.inputs)
        if (c > inCount)
          throw std::runtime_error("--in-ch " + std::to_string(c) + ": the device has " +
                                   std::to_string(inCount) + " input channels");
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = outName;
    setup.inputDeviceName = inName;
    setup.sampleRate = kRate;
    setup.bufferSize = options.bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels = MatchTake::channelMask(route.inputs);
    setup.outputChannels = MatchTake::channelMask(route.outputs);

    // Select the type first (no device is opened for it: no channels needed),
    // then give the manager the exact setup. No saved state in or out.
    manager_.setCurrentAudioDeviceType(chosen->getTypeName(), false);
    const juce::String error = manager_.initialise(0, 0, nullptr, false, {}, &setup);
    auto* device = manager_.getCurrentAudioDevice();
    if (error.isNotEmpty() || device == nullptr)
      throw std::runtime_error("cannot open '" + text(outName) + "'" +
                               (error.isEmpty() ? "" : ": " + text(error.trim())) +
                               ". Another program may be using it.");
    if (std::abs(device->getCurrentSampleRate() - kRate) > 0.5)
      throw std::runtime_error("the device opened at " +
                               std::to_string(static_cast<int>(device->getCurrentSampleRate())) +
                               " Hz, not 48000. Set the interface to 48 kHz.");

    const juce::BigInteger activeOut = device->getActiveOutputChannels();
    const juce::BigInteger activeIn = device->getActiveInputChannels();
    for (const int c : route.outputs) {
      const int index = MatchRecorder::activeChannelIndex(activeOut, c - 1);
      if (index < 0)
        throw std::runtime_error("the device did not open output channel " + std::to_string(c));
      outputs_.push_back(index);
    }
    for (const int c : route.inputs) {
      const int index = MatchRecorder::activeChannelIndex(activeIn, c - 1);
      if (index < 0)
        throw std::runtime_error("the device did not open input channel " + std::to_string(c));
      inputs_.push_back(index);
    }

    std::cerr << "opened '" << text(device->getName()) << "' (" << typeName_ << ") at 48000 Hz, buffer "
              << device->getCurrentBufferSizeSamples() << " samples, latency "
              << device->getOutputLatencyInSamples() << " out / " << device->getInputLatencyInSamples()
              << " in samples\n";
  }

  ~OpenDevice() { manager_.closeAudioDevice(); }

  juce::AudioDeviceManager& manager() { return manager_; }
  const std::vector<int>& outputs() const { return outputs_; }
  const std::vector<int>& inputs() const { return inputs_; }
  int xruns() { return manager_.getXRunCount(); }

 private:
  juce::AudioDeviceManager manager_;
  std::string typeName_;
  std::vector<int> outputs_, inputs_;
};

// ####################
// ONE TAKE
// ####################
struct Outcome {
  bool recorded = false;
  bool passed = false;
};

class Taker {
 public:
  explicit Taker(const Options& options)
      : route_{MatchTake::parseChannelList(options.outChannels, 8, "--out-ch"),
               MatchTake::parseChannelList(options.inChannels, 2, "--in-ch")},
        signal_(EffectMatch::makeTestSignal(kRate)) {
    device_ = std::make_unique<OpenDevice>(options, route_);
    recorder_.prepare(signal_.samples, device_->outputs(), device_->inputs(),
                      static_cast<int>(kTailSeconds * kRate));
    device_->manager().addAudioCallback(&recorder_);
    juce::Thread::sleep(kSettleMs);
  }

  ~Taker() { device_->manager().removeAudioCallback(&recorder_); }

  // Plays and records once, writes `file`, checks it. Prints the result.
  Outcome take(const juce::File& file) {
    const int xrunsBefore = device_->xruns();
    recorder_.start();
    const double seconds = recorder_.totalSamples() / kRate;
    const auto result = waitWithProgress(seconds);
    if (result == MatchRecorder::Wait::deviceStopped)
      throw std::runtime_error("the audio device stopped during the take");
    if (result == MatchRecorder::Wait::timeout)
      throw std::runtime_error("the take did not finish in time: the device is not running its "
                               "callbacks");

    MatchTake::writeTakeWav(file, recorder_.recording(), kRate);
    MatchTake::Check check = MatchTake::checkTake(recorder_.recording(), kRate, signal_);
    const int xruns = device_->xruns() - xrunsBefore;
    if (xruns > 0 && xrunsBefore >= 0) {
      check.problems.push_back(std::to_string(xruns) +
                               " buffer under or overruns during the take: use a larger --buffer");
      check.passed = false;
    }

    std::cout << "wrote " << text(file.getFullPathName()) << "\n";
    std::cout << "  peak " << juce::String(check.peakDb, 1) << " dBFS";
    if (check.markersFound)
      std::cout << ", both sync markers found, latency " << juce::String(check.latencyMs, 2)
                << " ms, level " << juce::String(check.levelGainDb, 1) << " dB at 1 kHz";
    std::cout << "\n";
    for (const auto& problem : check.problems)
      std::cout << "  PROBLEM: " << problem << "\n";
    std::cout << (check.passed ? "  check passed\n" : "  check FAILED\n");
    return {true, check.passed};
  }

 private:
  MatchRecorder::Wait waitWithProgress(double seconds) {
    const int timeoutMs = static_cast<int>((seconds + 10.0) * 1000.0);
    std::cerr << "recording " << juce::String(seconds, 0) << " s, do not touch the unit ..." << std::flush;
    int waited = 0;
    MatchRecorder::Wait result = MatchRecorder::Wait::timeout;
    while (waited < timeoutMs) {
      result = recorder_.waitUntilFinished(1000);
      if (result != MatchRecorder::Wait::timeout)
        break;
      waited += 1000;
      std::cerr << "\rrecording " << juce::String(seconds, 0) << " s, "
                << juce::String(recorder_.progress() * 100.0, 0) << "% ..." << std::flush;
    }
    std::cerr << "\r" << std::string(60, ' ') << "\r";
    return result;
  }

  Route route_;
  EffectMatch::TestSignal signal_;
  MatchRecorder recorder_;
  std::unique_ptr<OpenDevice> device_;
};

juce::File takeFile(const Options& options, const std::string& stem) {
  return options.dir.getChildFile(juce::String(stem) + ".wav");
}

// ####################
// SESSION
// ####################
bool readLine(std::string& line) {
  std::cout << std::flush;
  return static_cast<bool>(std::getline(std::cin, line));
}

std::string lowered(const std::string& s) {
  return juce::String(s).trim().toLowerCase().toStdString();
}

}  // namespace

int runDevices() {
  juce::AudioDeviceManager manager;
  int found = 0;
  for (auto* type : manager.getAvailableDeviceTypes()) {
    type->scanForDevices();
    const juce::StringArray names = allDeviceNames(*type);
    if (names.isEmpty())
      continue;
    std::cout << "Type: " << text(type->getTypeName()) << "\n";
    for (const auto& name : names) {
      const juce::String outName = type->getDeviceNames(false).contains(name) ? name : juce::String();
      const juce::String inName = type->getDeviceNames(true).contains(name) ? name : juce::String();
      std::unique_ptr<juce::AudioIODevice> device(type->createDevice(outName, inName));
      ++found;
      std::cout << "  Device: " << text(name) << "\n";
      if (device == nullptr) {
        std::cout << "    cannot be looked at (in use?)\n";
        continue;
      }
      printChannels("outputs", device->getOutputChannelNames());
      printChannels("inputs", device->getInputChannelNames());
      std::cout << "    sample rates: " << rateList(device->getAvailableSampleRates()) << "\n";
      if (!std::any_of(device->getAvailableSampleRates().begin(), device->getAvailableSampleRates().end(),
                       [](double r) { return std::abs(r - kRate) < 0.5; }))
        std::cout << "    (does not offer 48000 Hz: not usable for matching)\n";
    }
  }
  if (found == 0)
    std::cout << "no audio devices found\n";
  return 0;
}

int runRecord(const Options& options) {
  if (options.name.empty())
    throw std::invalid_argument("record needs --name <file stem>");
  if (options.outChannels.empty() || options.inChannels.empty())
    throw std::invalid_argument("record needs --out-ch and --in-ch (1-based channel numbers)");
  const std::string stem = options.name;
  if (!MatchTake::isPlainStem(stem))
    throw std::invalid_argument("bad --name '" + stem +
                                "': letters, digits, '.', '-' and '_' only, no folders");

  Taker taker(options);
  const Outcome outcome = taker.take(takeFile(options, stem));
  return outcome.passed ? 0 : kExitCheckFailed;
}

int runSession(const Options& options) {
  if (options.plan == juce::File() || !options.plan.existsAsFile())
    throw std::invalid_argument("session needs --plan <plan.txt> (a file that exists)");
  if (options.outChannels.empty() || options.inChannels.empty())
    throw std::invalid_argument("session needs --out-ch and --in-ch (1-based channel numbers)");
  std::vector<MatchTake::PlanTake> takes;
  try {
    takes = MatchTake::parsePlan(options.plan.loadFileAsString());
  } catch (const MatchTake::PlanError& e) {
    throw std::runtime_error(text(options.plan.getFileName()) + ", " + e.what());
  }
  if (takes.empty())
    throw std::runtime_error("the plan has no takes");

  enum class State { pending, passed, failed, existing, skipped };
  std::vector<State> states(takes.size(), State::pending);
  for (size_t i = 0; i < takes.size(); ++i)
    if (!options.overwrite && takeFile(options, takes[i].stem).existsAsFile())
      states[i] = State::existing;

  Taker taker(options);
  std::cout << takes.size() << " takes in " << text(options.plan.getFileName()) << ". At each prompt: "
            << "Enter records, s skips, r redoes the previous take, q quits.\n";

  bool quit = false;
  size_t i = 0;
  while (i < takes.size() && !quit) {
    if (states[i] == State::existing) {
      std::cout << "[" << (i + 1) << "/" << takes.size() << "] " << takes[i].stem
                << ": already recorded, skipped (use --overwrite to record again)\n";
      ++i;
      continue;
    }
    if (states[i] == State::passed) {  // after going back, do not ask again
      ++i;
      continue;
    }
    std::cout << "\n[" << (i + 1) << "/" << takes.size() << "] " << takes[i].stem << "\n"
              << "Set the unit to: " << takes[i].instruction << "\n"
              << "Enter to record, s to skip, r to redo the previous, q to quit > ";
    std::string answer;
    if (!readLine(answer)) {
      std::cout << "\n";
      break;
    }
    const std::string choice = lowered(answer);
    if (choice == "q") {
      quit = true;
    } else if (choice == "s") {
      states[i++] = State::skipped;
    } else if (choice == "r") {
      if (i == 0) {
        std::cout << "there is no previous take\n";
        continue;
      }
      --i;
      states[i] = State::pending;
      std::cout << "going back to " << takes[i].stem << "\n";
    } else if (choice.empty()) {
      // A take that fails its check is offered again until it passes or the
      // player declines; a declined one is set aside so that the next run
      // records it again.
      for (;;) {
        const juce::File file = takeFile(options, takes[i].stem);
        const Outcome outcome = taker.take(file);
        if (outcome.passed) {
          states[i] = State::passed;
          break;
        }
        std::cout << "Redo this take? [Y/n] > ";
        std::string again;
        const bool got = readLine(again);
        if (got && lowered(again) != "n" && lowered(again) != "no")
          continue;
        const juce::File rejected = options.dir.getChildFile(juce::String(takes[i].stem) + ".rejected.wav");
        rejected.deleteFile();
        file.moveFileTo(rejected);
        std::cout << "kept as " << text(rejected.getFileName()) << "\n";
        states[i] = State::failed;
        if (!got)
          quit = true;
        break;
      }
      ++i;
    } else {
      std::cout << "unknown answer '" << answer << "'\n";
    }
  }

  // The summary.
  int passed = 0, failed = 0, existing = 0, skipped = 0, left = 0;
  std::cout << "\nSummary (" << text(options.dir.getFullPathName()) << ")\n";
  for (size_t k = 0; k < takes.size(); ++k) {
    const char* label = "not recorded";
    switch (states[k]) {
      case State::passed: label = "recorded, check passed"; ++passed; break;
      case State::failed: label = "FAILED the check (kept as .rejected.wav)"; ++failed; break;
      case State::existing: label = "already there"; ++existing; break;
      case State::skipped: label = "skipped"; ++skipped; break;
      case State::pending: ++left; break;
    }
    std::cout << "  " << takes[k].stem << ": " << label << "\n";
  }
  std::cout << passed << " recorded, " << failed << " failed, " << existing << " already there, " << skipped
            << " skipped, " << left << " not reached\n";
  return failed > 0 ? kExitCheckFailed : 0;
}

}  // namespace MatchRecord
