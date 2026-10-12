// PedaleiraMatch: the effect-matching measurement kit on the command line.
// A fixed test signal is played through a reference unit and through one of
// our effects; the two recordings are analysed into the same numbers and the
// numbers are compared (docs/matching.md). Nothing here knows what is inside
// the unit under test: it is measured from the audio it returns.
//
//   PedaleiraMatch signals --out <dir> [--rate <hz>]
//   PedaleiraMatch analyse --in <recording.wav> [--out <analysis.json>] [--rate <hz>]
//   PedaleiraMatch compare --reference <a.json|a.wav> --candidate <b.json|b.wav>
//                          [--out <comparison.json>] [--markdown <file.md>]
//                          [--rate <hz>] [--strict]
//   PedaleiraMatch devices
//   PedaleiraMatch record --device <name> [--type <device type>] --out-ch <a[,b]>
//                         --in-ch <c[,d]> --name <file stem> [--dir match/ref]
//                         [--rate 48000] [--buffer 256]
//   PedaleiraMatch session --plan <plan.txt> --device <name> [--type <device type>]
//                          --out-ch <a[,b]> --in-ch <c[,d]> [--dir match/ref]
//                          [--rate 48000] [--buffer 256] [--overwrite]
//
// signals writes pedaleira-test-signal-v<N>.wav (mono, 32-bit float) and
// pedaleira-test-signal-v<N>.json (the manifest of its segments) into the
// folder. analyse aligns a recording of that signal on its two sync markers
// and prints the analysis as JSON (or writes it to --out). compare takes two
// analyses, or two recordings (analysed first), and prints the comparison as
// JSON; --markdown also writes it as a table a person can read.
//
// --rate is the rate the test signal is made at (default 48000 for signals,
// the recording's own rate for analyse and compare); a recording at another
// rate must be resampled first. Our side of a comparison is rendered with
// PedaleiraRender (--run commands.json --in the signal --out ours.wav
// --tail 4).
//
// devices lists the audio devices with their channels and sample rates. record
// plays the test signal on the given output channels (1-based) of a device and
// records the given input channels into <dir>/<name>.wav, then checks the take
// (both sync markers, no clipping, latency, plausible level). session does that
// for every line of a plan file, prompting the player between takes. Both work
// at 48 kHz only (match_record.cpp, MatchRecorder.h).
//
// Diagnostics go to stderr. Exit code 0 on success, 1 with a one-line error
// otherwise; with --strict, compare exits 2 when any judged metric is out of
// tolerance; record and session exit 3 when a take failed its check.
//
// Build: cmake --build build --target PedaleiraMatch
#include "EffectMatch.h"
#include "match_record.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct MatchToolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A bad command line, as opposed to an analysis that failed.
struct UsageError : MatchToolError {
  using MatchToolError::MatchToolError;
};

constexpr const char* kUsage =
    "usage: PedaleiraMatch signals --out <dir> [--rate <hz, default 48000>]\n"
    "       PedaleiraMatch analyse --in <recording.wav> [--out <analysis.json>] [--rate <hz>]\n"
    "       PedaleiraMatch compare --reference <a.json|a.wav> --candidate <b.json|b.wav>\n"
    "                              [--out <comparison.json>] [--markdown <file.md>]\n"
    "                              [--rate <hz>] [--strict]\n"
    "       PedaleiraMatch devices\n"
    "       PedaleiraMatch record --device <name> [--type <device type>] --out-ch <a[,b]>\n"
    "                             --in-ch <c[,d]> --name <file stem> [--dir match/ref]\n"
    "                             [--rate 48000] [--buffer 256]\n"
    "       PedaleiraMatch session --plan <plan.txt> --device <name> [--type <device type>]\n"
    "                              --out-ch <a[,b]> --in-ch <c[,d]> [--dir match/ref]\n"
    "                              [--rate 48000] [--buffer 256] [--overwrite]\n"
    "  signals write the test signal (WAV) and its manifest (JSON) into <dir>\n"
    "  analyse  measure a recording of the test signal; JSON on stdout or in --out\n"
    "  compare  compare two analyses (or recordings, analysed first); JSON on stdout\n"
    "  --rate   the rate the test signal is made at; a recording must be at that rate\n"
    "  --strict compare exits 2 when any judged metric is out of tolerance\n"
    "  devices list audio devices, their channels and sample rates\n"
    "  record   play the test signal and record one take (48 kHz) into <dir>/<name>.wav\n"
    "  session  record the takes of a plan file, one prompt per take\n"
    "  --out-ch, --in-ch  1-based channel numbers; the same signal plays on each --out-ch,\n"
    "                     one or two --in-ch make a mono or stereo take";

struct Options {
  std::string command;
  juce::File outDir, input, output, reference, candidate, markdown;
  double rate = 0.0;
  bool strict = false;
  MatchRecord::Options record;
};

Options parseOptions(int argc, char** argv) {
  Options opts;
  if (argc < 2)
    throw UsageError("missing command");
  opts.command = argv[1];
  auto value = [&](int& i) -> juce::String {
    if (i + 1 >= argc)
      throw UsageError(std::string("missing value for ") + argv[i]);
    return juce::String::fromUTF8(argv[++i]);
  };
  auto absolute = [](const juce::String& path) {
    return juce::File::getCurrentWorkingDirectory().getChildFile(path);
  };
  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--out") {
      const juce::File file = absolute(value(i));
      opts.outDir = file;
      opts.output = file;
    } else if (flag == "--in") {
      opts.input = absolute(value(i));
    } else if (flag == "--reference") {
      opts.reference = absolute(value(i));
    } else if (flag == "--candidate") {
      opts.candidate = absolute(value(i));
    } else if (flag == "--markdown") {
      opts.markdown = absolute(value(i));
    } else if (flag == "--rate") {
      const juce::String text = value(i);
      if (!text.containsOnly("0123456789.") || text.isEmpty())
        throw UsageError("bad value for --rate: " + text.toStdString());
      opts.rate = text.getDoubleValue();
    } else if (flag == "--strict") {
      opts.strict = true;
    } else if (flag == "--device") {
      opts.record.device = value(i).toStdString();
    } else if (flag == "--type") {
      opts.record.type = value(i).toStdString();
    } else if (flag == "--out-ch") {
      opts.record.outChannels = value(i).toStdString();
    } else if (flag == "--in-ch") {
      opts.record.inChannels = value(i).toStdString();
    } else if (flag == "--name") {
      opts.record.name = value(i).toStdString();
    } else if (flag == "--dir") {
      opts.record.dir = absolute(value(i));
    } else if (flag == "--plan") {
      opts.record.plan = absolute(value(i));
    } else if (flag == "--buffer") {
      const juce::String text = value(i);
      if (text.isEmpty() || !text.containsOnly("0123456789") || text.getIntValue() < 16 ||
          text.getIntValue() > 16384)
        throw UsageError("bad value for --buffer: " + text.toStdString() + " (16 to 16384 samples)");
      opts.record.bufferSize = text.getIntValue();
    } else if (flag == "--overwrite") {
      opts.record.overwrite = true;
    } else {
      throw UsageError("unknown option: " + flag);
    }
  }
  if (opts.rate > 0.0)
    opts.record.rate = opts.rate;
  if (opts.record.dir == juce::File())
    opts.record.dir = absolute("match/ref");
  return opts;
}

// ####################
// FILES
// ####################
struct Audio {
  juce::AudioBuffer<float> samples;
  double rate = 0.0;
};

Audio readWav(const juce::File& file) {
  if (!file.existsAsFile())
    throw MatchToolError("input file not found: " + file.getFullPathName().toStdString());
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::AudioFormatReader> reader(
      wav.createReaderFor(file.createInputStream().release(), true));
  if (reader == nullptr)
    throw MatchToolError("not a readable WAV file: " + file.getFullPathName().toStdString());
  if (reader->numChannels < 1 || reader->lengthInSamples < 1 || reader->lengthInSamples > (1 << 28))
    throw MatchToolError("the recording is empty or too long: " + file.getFullPathName().toStdString());

  Audio audio;
  audio.rate = reader->sampleRate;
  audio.samples.setSize(static_cast<int>(std::min<unsigned int>(reader->numChannels, 2u)),
                        static_cast<int>(reader->lengthInSamples));
  reader->read(&audio.samples, 0, audio.samples.getNumSamples(), 0, true, true);
  return audio;
}

void writeText(const juce::File& file, const juce::String& text) {
  if (!file.getParentDirectory().createDirectory() || !file.replaceWithText(text))
    throw MatchToolError("cannot write " + file.getFullPathName().toStdString());
}

void writeSignalWav(const juce::File& file, const EffectMatch::TestSignal& signal) {
  // FileOutputStream appends to an existing file.
  file.deleteFile();
  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    throw MatchToolError("cannot write " + file.getFullPathName().toStdString());
  juce::WavAudioFormat wav;
  auto writer = wav.createWriterFor(
      stream, juce::AudioFormatWriterOptions{}
                  .withSampleRate(signal.sampleRate)
                  .withNumChannels(1)
                  .withBitsPerSample(32)
                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  if (writer == nullptr)
    throw MatchToolError("cannot write a WAV at " + std::to_string(signal.sampleRate) + " Hz");
  juce::AudioBuffer<float> buffer(1, static_cast<int>(signal.samples.size()));
  std::copy(signal.samples.begin(), signal.samples.end(), buffer.getWritePointer(0));
  if (!writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()))
    throw MatchToolError("failed writing " + file.getFullPathName().toStdString());
}

juce::var parseJson(const juce::File& file) {
  if (!file.existsAsFile())
    throw MatchToolError("input file not found: " + file.getFullPathName().toStdString());
  juce::var json;
  const juce::Result result = juce::JSON::parse(file.loadFileAsString(), json);
  if (result.failed())
    throw MatchToolError("not valid JSON: " + file.getFullPathName().toStdString() + " (" +
                         result.getErrorMessage().toStdString() + ")");
  return json;
}

EffectMatch::MatchAnalysis analyseFile(const juce::File& file, double rate) {
  const Audio audio = readWav(file);
  const double signalRate = rate > 0.0 ? rate : audio.rate;
  const EffectMatch::TestSignal signal = EffectMatch::makeTestSignal(signalRate);
  return EffectMatch::analyseRecording(audio.samples, audio.rate, signal);
}

// An analysis from a JSON file or from a recording, by the extension.
EffectMatch::MatchAnalysis loadAnalysis(const juce::File& file, double rate) {
  if (file.hasFileExtension("wav"))
    return analyseFile(file, rate);
  return EffectMatch::analysisFromVar(parseJson(file));
}

// Four decimals is more than the figures are rounded to (EffectMatch toVar),
// and keeps 0.1 from printing as 0.10000000000000001.
juce::String pretty(const juce::var& json) { return juce::JSON::toString(json, false, 4) + "\n"; }

// ####################
// COMMANDS
// ####################
int runSignals(const Options& opts) {
  if (opts.outDir == juce::File())
    throw UsageError("signals needs --out <dir>");
  const double rate = opts.rate > 0.0 ? opts.rate : EffectMatch::kSignalRate;
  const EffectMatch::TestSignal signal = EffectMatch::makeTestSignal(rate);
  if (!opts.outDir.createDirectory())
    throw MatchToolError("cannot create " + opts.outDir.getFullPathName().toStdString());
  const juce::String base = "pedaleira-test-signal-v" + juce::String(signal.version);
  writeSignalWav(opts.outDir.getChildFile(base + ".wav"), signal);
  writeText(opts.outDir.getChildFile(base + ".json"), pretty(EffectMatch::toVar(signal)));
  std::cerr << "wrote " << (base + ".wav").toStdString() << " and " << (base + ".json").toStdString()
            << " (" << signal.samples.size() / rate << " s at " << rate << " Hz)\n";
  return 0;
}

int runAnalyse(const Options& opts) {
  if (opts.input == juce::File())
    throw UsageError("analyse needs --in <recording.wav>");
  const EffectMatch::MatchAnalysis analysis = analyseFile(opts.input, opts.rate);
  const juce::String json = pretty(EffectMatch::toVar(analysis));
  if (opts.output != juce::File())
    writeText(opts.output, json);
  else
    std::cout << json.toStdString();
  return 0;
}

int runCompare(const Options& opts) {
  if (opts.reference == juce::File() || opts.candidate == juce::File())
    throw UsageError("compare needs --reference and --candidate");
  const EffectMatch::MatchAnalysis reference = loadAnalysis(opts.reference, opts.rate);
  const EffectMatch::MatchAnalysis candidate = loadAnalysis(opts.candidate, opts.rate);
  const EffectMatch::MatchComparison comparison = EffectMatch::compareAnalyses(reference, candidate);
  const juce::String json = pretty(EffectMatch::toVar(comparison));
  if (opts.output != juce::File())
    writeText(opts.output, json);
  else
    std::cout << json.toStdString();
  if (opts.markdown != juce::File())
    writeText(opts.markdown, EffectMatch::toMarkdown(comparison));
  std::cerr << "score " << comparison.score << " (" << comparison.passed << " of " << comparison.judged
            << " metrics within tolerance)\n";
  return opts.strict && !comparison.allPass ? 2 : 0;
}

int run(int argc, char** argv) {
  const Options opts = parseOptions(argc, argv);
  if (opts.command == "signals")
    return runSignals(opts);
  if (opts.command == "analyse")
    return runAnalyse(opts);
  if (opts.command == "compare")
    return runCompare(opts);
  if (opts.command == "devices" || opts.command == "record" || opts.command == "session") {
    // Audio devices need JUCE initialised on some platforms (CoreAudio, device
    // change notifications). This creates the message manager but opens no
    // window, and the recorder itself never uses the message thread: waiting for
    // Enter happens on this thread while the audio runs on the device's own.
#if JUCE_MAC
    juce::Process::setDockIconVisible(false);
#endif
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    try {
      if (opts.command == "devices")
        return MatchRecord::runDevices();
      if (opts.command == "record")
        return MatchRecord::runRecord(opts.record);
      return MatchRecord::runSession(opts.record);
    } catch (const std::invalid_argument& e) {
      throw UsageError(e.what());
    }
  }
  throw UsageError("unknown command: " + opts.command);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const UsageError& e) {
    std::cerr << "PedaleiraMatch: " << e.what() << "\n" << kUsage << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "PedaleiraMatch: " << e.what() << "\n";
    return 1;
  }
}
