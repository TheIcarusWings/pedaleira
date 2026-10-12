#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <vector>

// Measurements of rendered audio, as numbers an agent can aim with instead of
// ears: how loud, how bright, how distorted, how wide, how long the tail.
// Pure functions over buffers: nothing here knows about the processor, and
// the same samples always give the same numbers.
//
// Each definition and what it leaves out is written up in docs/agents.md
// ("Measurements"); the comments here give the short form.
namespace AudioMeasure {

// Readings of silence or of nothing at all bottom out here instead of at
// minus infinity, which JSON cannot carry.
constexpr double kFloorDb = -120.0;

struct Band {
  const char* id;
  double lowHz;
  double highHz;
};

// The fixed bands of the spectral balance, low to high.
constexpr std::array<Band, 7> kBands = {{
    {"sub", 20.0, 80.0},
    {"bass", 80.0, 250.0},
    {"lowMid", 250.0, 500.0},
    {"mid", 500.0, 2000.0},
    {"highMid", 2000.0, 4000.0},
    {"presence", 4000.0, 8000.0},
    {"air", 8000.0, 20000.0},
}};

struct Levels {
  // Highest sample magnitude, dBFS.
  double peakDb = kFloorDb;
  // The same after 4x interpolation, which finds peaks between samples.
  double truePeakDb = kFloorDb;
  // RMS over the whole buffer, all channels pooled, dBFS (a full-scale sine
  // reads -3.01).
  double rmsDb = kFloorDb;
  // peakDb - rmsDb.
  double crestDb = 0.0;
  // Integrated loudness, LUFS (ITU-R BS.1770 K-weighting, gated).
  double lufs = kFloorDb;
};

struct Spectrum {
  // Each band's share of the energy between 20 Hz and 20 kHz, 0 to 1.
  std::array<double, 7> share{};
  // The same in dB relative to the total (10 log10 share).
  std::array<double, 7> db{};
  // Energy-weighted mean frequency between 20 Hz and 20 kHz.
  double centroidHz = 0.0;
  // Share of the energy below 250 Hz (sub + bass).
  double lowEndShare = 0.0;
};

struct Stereo {
  // Normalised left/right correlation, -1 to 1; 1 for mono or silence.
  double correlation = 1.0;
  // Side energy over total energy, as an amplitude ratio: 0 mono, about 0.71
  // for two unrelated channels, 1 for pure out-of-phase.
  double width = 0.0;
  // Right over left energy in dB; 0 is centred.
  double balanceDb = 0.0;
};

struct Distortion {
  // Share of the output a linear filter of the input cannot explain, as an
  // amplitude percentage: 100 x sqrt(residual energy / output energy).
  double percent = 0.0;
  // The level of that residue relative to the output, dB.
  double residualDb = kFloorDb;
  // False when there was too little signal to judge.
  bool valid = false;
};

struct Hum {
  // Level of the 50 Hz and 60 Hz components, dBFS peak.
  double hz50Db = kFloorDb;
  double hz60Db = kFloorDb;
  // 50 or 60 when one of them stands out as mains hum, else 0.
  int mainsHz = 0;
  // The mains fundamental, with its second and third harmonic where those
  // stand out as lines too, dBFS; the floor when no hum was found.
  double levelDb = kFloorDb;
};

Levels measureLevels(const juce::AudioBuffer<float>& audio, double sampleRate);
Spectrum measureSpectrum(const juce::AudioBuffer<float>& audio, double sampleRate);
Stereo measureStereo(const juce::AudioBuffer<float>& audio);
Hum measureHum(const float* samples, int numSamples, double sampleRate);

// Integrated loudness alone (what measureLevels reports as lufs).
double integratedLoudness(const juce::AudioBuffer<float>& audio, double sampleRate);
// Linear peak after 4x interpolation.
double truePeak(const juce::AudioBuffer<float>& audio);

// The amplitude of the component at `hz`, dBFS peak (a sine of amplitude 0.5
// at that frequency reads -6.02).
double toneLevelDb(const float* samples, int numSamples, double hz, double sampleRate);

// The level of the quietest tenth of the buffer's 50 ms windows, dBFS RMS: an
// estimate of the noise under a recording that has pauses in it.
double noiseFloorDb(const float* samples, int numSamples, double sampleRate);

// The best linear, time-invariant account of `output` given `input` (both
// `numSamples` long, time aligned; only the first 20 seconds are used): a
// filter whose response spans no more than an eighth of the signal, at most
// one second.
class LinearFit {
public:
  LinearFit(const float* input, const float* output, int numSamples, double sampleRate);

  Distortion distortion() const { return result; }
  // The fitted filter's gain at `hz`, in dB; the floor where the input had
  // nothing to measure with.
  double gainDbAt(double hz) const;
  // How much of the output at `hz` (within a third of an octave) the fit
  // explains, 0 to 1: near 1 where the rig behaves linearly.
  double coherenceAt(double hz) const;

private:
  Distortion result;
  double rate = 48000.0;
  int fftSize = 0;
  // Interleaved (re, im) spectra, bins 0 to fftSize / 2: the output, and
  // what the fitted filter makes of the input.
  std::vector<float> outputSpectrum, predicted;
  // The input's power per bin, and its sum.
  std::vector<float> inputPower;
  double totalInput = 0.0;
};

// Distortion of one channel of output against one channel of input.
Distortion measureDistortion(const float* input, const float* output, int numSamples,
                             double sampleRate);

// How long the output keeps sounding after the input has ended: the time from
// `inputEnd` to the end of the last 10 ms window that is within 60 dB of the
// output's level while the input was playing. `truncated` is set when the
// buffer ended before the output had fallen that far.
double tailSeconds(const juce::AudioBuffer<float>& output, int inputEnd, double sampleRate,
                   bool* truncated = nullptr);

juce::var toVar(const Levels& levels);
juce::var toVar(const Spectrum& spectrum);
juce::var toVar(const Stereo& stereo);
juce::var toVar(const Distortion& distortion);
juce::var toVar(const Hum& hum);

// A dB value rounded to hundredths, so results print short and compare
// stably.
double tidyDb(double db);
// 20 log10(amplitude), bottoming out at kFloorDb.
double amplitudeToDb(double amplitude);
// 10 log10(power), bottoming out at kFloorDb.
double powerToDb(double power);

}  // namespace AudioMeasure
