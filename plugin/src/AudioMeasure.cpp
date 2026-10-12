#include "AudioMeasure.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace AudioMeasure {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Welch segment for the spectral balance: 8192 samples is about 6 Hz per bin
// at 48 kHz, enough to tell the sub band from the bass band.
constexpr int kSpectrumOrder = 13;

// The distortion fit looks at no more than this much of a clip, which keeps
// its FFT at 2^21 points.
constexpr double kMaxFitSeconds = 20.0;

int nextPowerOfTwoOrder(int minimumSize) {
  int order = 1;
  while ((1 << order) < minimumSize && order < 30)
    ++order;
  return order;
}

// One second-order section, direct form 1, in double precision.
struct Biquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

  double process(double x) {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};

// The two K-weighting stages of BS.1770 for any sample rate, from the analog
// prototypes the standard's 48 kHz coefficients come from (the same
// derivation libebur128 uses): a high shelf of about +4 dB above 1.7 kHz for
// the head, then a 38 Hz high-pass.
Biquad kWeightingShelf(double sampleRate) {
  const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
  const double k = std::tan(kPi * f0 / sampleRate);
  const double vh = std::pow(10.0, gainDb / 20.0);
  const double vb = std::pow(vh, 0.4996667741545416);
  const double a0 = 1.0 + k / q + k * k;
  Biquad s;
  s.b0 = (vh + vb * k / q + k * k) / a0;
  s.b1 = 2.0 * (k * k - vh) / a0;
  s.b2 = (vh - vb * k / q + k * k) / a0;
  s.a1 = 2.0 * (k * k - 1.0) / a0;
  s.a2 = (1.0 - k / q + k * k) / a0;
  return s;
}

Biquad kWeightingHighPass(double sampleRate) {
  const double f0 = 38.13547087602444, q = 0.5003270373238773;
  const double k = std::tan(kPi * f0 / sampleRate);
  const double a0 = 1.0 + k / q + k * k;
  Biquad s;
  s.b0 = 1.0;
  s.b1 = -2.0;
  s.b2 = 1.0;
  s.a1 = 2.0 * (k * k - 1.0) / a0;
  s.a2 = (1.0 - k / q + k * k) / a0;
  return s;
}

// Per-bin power of each channel, averaged over Hann-windowed segments and
// over the channels (so a wide stereo signal is not thinned by cancellation,
// as it would be in a mono sum).
std::vector<double> averagedPowerSpectrum(const juce::AudioBuffer<float>& audio, int order) {
  const int size = 1 << order;
  const int hop = size / 2;
  std::vector<double> power(static_cast<size_t>(size / 2 + 1), 0.0);
  const int frames = audio.getNumSamples();
  if (frames <= 0 || audio.getNumChannels() <= 0)
    return power;

  juce::dsp::FFT fft(order);
  std::vector<float> window(static_cast<size_t>(size));
  for (int i = 0; i < size; ++i)
    window[static_cast<size_t>(i)] =
        static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / size));
  std::vector<float> work(static_cast<size_t>(size) * 2);

  int segments = 0;
  for (int ch = 0; ch < audio.getNumChannels(); ++ch) {
    const float* x = audio.getReadPointer(ch);
    // A buffer shorter than one segment still gets one, zero padded.
    for (int start = 0; start == 0 || start + size <= frames; start += hop) {
      std::fill(work.begin(), work.end(), 0.0f);
      const int count = std::min(size, frames - start);
      for (int i = 0; i < count; ++i)
        work[static_cast<size_t>(i)] = x[start + i] * window[static_cast<size_t>(i)];
      fft.performRealOnlyForwardTransform(work.data(), true);
      for (int bin = 0; bin <= size / 2; ++bin) {
        const double re = work[static_cast<size_t>(bin) * 2];
        const double im = work[static_cast<size_t>(bin) * 2 + 1];
        power[static_cast<size_t>(bin)] += re * re + im * im;
      }
      ++segments;
    }
  }
  if (segments > 0)
    for (double& p : power)
      p /= segments;
  return power;
}

// Complex spectrum of a real signal, zero padded to the FFT size: interleaved
// (re, im) for bins 0 to size / 2.
std::vector<float> spectrumOf(const juce::dsp::FFT& fft, const float* x, int count) {
  const int size = fft.getSize();
  std::vector<float> data(static_cast<size_t>(size) * 2, 0.0f);
  std::copy(x, x + std::min(count, size), data.begin());
  fft.performRealOnlyForwardTransform(data.data(), true);
  return data;
}

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties) {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  for (const auto& [name, value] : properties)
    obj->setProperty(name, value);
  return juce::var(obj.get());
}

// Rounded to a multiple of 1 / perUnit. Dividing last gives the double
// nearest the decimal, which prints short (9.12, not 9.120000000000001).
double tidy(double value, double perUnit) { return std::round(value * perUnit) / perUnit; }

}  // namespace

double amplitudeToDb(double amplitude) {
  return amplitude > 1.0e-6 ? std::max(kFloorDb, 20.0 * std::log10(amplitude)) : kFloorDb;
}

double powerToDb(double power) {
  return power > 1.0e-12 ? std::max(kFloorDb, 10.0 * std::log10(power)) : kFloorDb;
}

double tidyDb(double db) { return tidy(db, 100.0); }

// ####################
// LEVELS
// ####################
double integratedLoudness(const juce::AudioBuffer<float>& audio, double sampleRate) {
  const int frames = audio.getNumSamples();
  const int channels = audio.getNumChannels();
  if (frames <= 0 || channels <= 0 || sampleRate <= 0.0)
    return kFloorDb;

  // 400 ms blocks every 100 ms. A signal shorter than one block is measured
  // as a single block of its own length.
  const int blockLength = std::min(frames, static_cast<int>(std::lround(0.4 * sampleRate)));
  const int step = std::max(1, static_cast<int>(std::lround(0.1 * sampleRate)));

  // Running sum of each channel's K-weighted squares, so a block's mean
  // square is one subtraction.
  std::vector<std::vector<double>> cumulative(static_cast<size_t>(channels));
  for (int ch = 0; ch < channels; ++ch) {
    Biquad shelf = kWeightingShelf(sampleRate);
    Biquad highPass = kWeightingHighPass(sampleRate);
    auto& sums = cumulative[static_cast<size_t>(ch)];
    sums.resize(static_cast<size_t>(frames) + 1);
    sums[0] = 0.0;
    const float* x = audio.getReadPointer(ch);
    for (int i = 0; i < frames; ++i) {
      const double y = highPass.process(shelf.process(static_cast<double>(x[i])));
      sums[static_cast<size_t>(i) + 1] = sums[static_cast<size_t>(i)] + y * y;
    }
  }

  // Channel weights are 1 for left and right; surround weights do not apply
  // to a rig's output.
  std::vector<double> blockPower;
  for (int start = 0; start + blockLength <= frames; start += step) {
    double sum = 0.0;
    for (int ch = 0; ch < channels; ++ch)
      sum += (cumulative[static_cast<size_t>(ch)][static_cast<size_t>(start + blockLength)] -
              cumulative[static_cast<size_t>(ch)][static_cast<size_t>(start)]) /
             blockLength;
    blockPower.push_back(sum);
  }

  const auto loudnessOf = [](double power) {
    return power > 0.0 ? -0.691 + 10.0 * std::log10(power)
                       : -std::numeric_limits<double>::infinity();
  };
  const auto meanAbove = [&](double thresholdLufs, bool& any) {
    double sum = 0.0;
    int count = 0;
    for (const double power : blockPower)
      if (loudnessOf(power) > thresholdLufs) {
        sum += power;
        ++count;
      }
    any = count > 0;
    return count > 0 ? sum / count : 0.0;
  };

  bool any = false;
  const double absoluteGated = meanAbove(-70.0, any);
  if (!any)
    return kFloorDb;
  const double relativeThreshold = loudnessOf(absoluteGated) - 10.0;
  const double gated = meanAbove(std::max(-70.0, relativeThreshold), any);
  if (!any)
    return kFloorDb;
  return std::max(kFloorDb, loudnessOf(gated));
}

double truePeak(const juce::AudioBuffer<float>& audio) {
  // Four times oversampling with a 12-tap-per-phase windowed sinc: phase 0
  // is the samples themselves, the other three sit between them.
  constexpr int kPhases = 4;
  constexpr int kTaps = 12;
  static const auto table = [] {
    std::array<std::array<double, kTaps>, kPhases> t{};
    for (int phase = 0; phase < kPhases; ++phase)
      for (int tap = 0; tap < kTaps; ++tap) {
        // Distance from the interpolated point to tap's sample, in samples.
        const double d = (tap - kTaps / 2 + 1) - static_cast<double>(phase) / kPhases;
        const double sinc = std::abs(d) < 1.0e-12 ? 1.0 : std::sin(kPi * d) / (kPi * d);
        const double window = 0.5 + 0.5 * std::cos(kPi * d / (kTaps / 2));
        t[static_cast<size_t>(phase)][static_cast<size_t>(tap)] =
            std::abs(d) < kTaps / 2 ? sinc * window : 0.0;
      }
    return t;
  }();

  double peak = 0.0;
  const int frames = audio.getNumSamples();
  for (int ch = 0; ch < audio.getNumChannels(); ++ch) {
    const float* x = audio.getReadPointer(ch);
    for (int i = 0; i < frames; ++i) {
      peak = std::max(peak, std::abs(static_cast<double>(x[i])));
      for (int phase = 1; phase < kPhases; ++phase) {
        double sum = 0.0;
        for (int tap = 0; tap < kTaps; ++tap) {
          const int index = i + tap - kTaps / 2 + 1;
          if (index >= 0 && index < frames)
            sum += table[static_cast<size_t>(phase)][static_cast<size_t>(tap)] * x[index];
        }
        peak = std::max(peak, std::abs(sum));
      }
    }
  }
  return peak;
}

Levels measureLevels(const juce::AudioBuffer<float>& audio, double sampleRate) {
  Levels levels;
  const int frames = audio.getNumSamples();
  const int channels = audio.getNumChannels();
  if (frames <= 0 || channels <= 0)
    return levels;

  double peak = 0.0, sumSquares = 0.0;
  for (int ch = 0; ch < channels; ++ch) {
    const float* x = audio.getReadPointer(ch);
    for (int i = 0; i < frames; ++i) {
      const double v = x[i];
      peak = std::max(peak, std::abs(v));
      sumSquares += v * v;
    }
  }
  levels.peakDb = amplitudeToDb(peak);
  levels.truePeakDb = amplitudeToDb(truePeak(audio));
  levels.rmsDb = powerToDb(sumSquares / (static_cast<double>(frames) * channels));
  levels.crestDb = peak > 1.0e-6 ? levels.peakDb - levels.rmsDb : 0.0;
  levels.lufs = integratedLoudness(audio, sampleRate);
  return levels;
}

// ####################
// SPECTRUM
// ####################
Spectrum measureSpectrum(const juce::AudioBuffer<float>& audio, double sampleRate) {
  Spectrum spectrum;
  for (auto& value : spectrum.db)
    value = kFloorDb;
  if (audio.getNumSamples() <= 0 || sampleRate <= 0.0)
    return spectrum;

  const std::vector<double> power = averagedPowerSpectrum(audio, kSpectrumOrder);
  const double binHz = sampleRate / (1 << kSpectrumOrder);

  double total = 0.0, weighted = 0.0;
  std::array<double, 7> bandEnergy{};
  for (size_t bin = 1; bin < power.size(); ++bin) {
    const double hz = static_cast<double>(bin) * binHz;
    if (hz < kBands.front().lowHz || hz >= kBands.back().highHz)
      continue;
    total += power[bin];
    weighted += power[bin] * hz;
    for (size_t band = 0; band < kBands.size(); ++band)
      if (hz >= kBands[band].lowHz && hz < kBands[band].highHz) {
        bandEnergy[band] += power[bin];
        break;
      }
  }
  if (total <= 1.0e-18)
    return spectrum;

  for (size_t band = 0; band < kBands.size(); ++band) {
    spectrum.share[band] = bandEnergy[band] / total;
    spectrum.db[band] = powerToDb(spectrum.share[band]);
  }
  spectrum.centroidHz = weighted / total;
  spectrum.lowEndShare = spectrum.share[0] + spectrum.share[1];
  return spectrum;
}

// ####################
// STEREO
// ####################
Stereo measureStereo(const juce::AudioBuffer<float>& audio) {
  Stereo stereo;
  if (audio.getNumChannels() < 2 || audio.getNumSamples() <= 0)
    return stereo;

  const float* l = audio.getReadPointer(0);
  const float* r = audio.getReadPointer(1);
  double ll = 0.0, rr = 0.0, lr = 0.0;
  for (int i = 0; i < audio.getNumSamples(); ++i) {
    ll += static_cast<double>(l[i]) * l[i];
    rr += static_cast<double>(r[i]) * r[i];
    lr += static_cast<double>(l[i]) * r[i];
  }
  if (ll + rr <= 1.0e-12)
    return stereo;

  if (ll > 1.0e-12 && rr > 1.0e-12)
    stereo.correlation = juce::jlimit(-1.0, 1.0, lr / std::sqrt(ll * rr));
  else
    stereo.correlation = 0.0;  // one side silent: nothing in common
  // mid = (l + r) / 2, side = (l - r) / 2.
  const double mid = (ll + rr + 2.0 * lr) / 4.0;
  const double side = (ll + rr - 2.0 * lr) / 4.0;
  stereo.width = mid + side > 0.0 ? std::sqrt(std::max(0.0, side) / (mid + side)) : 0.0;
  stereo.balanceDb = juce::jlimit(-60.0, 60.0, powerToDb(rr) - powerToDb(ll));
  return stereo;
}

// ####################
// TONES, HUM, NOISE
// ####################
double toneLevelDb(const float* samples, int numSamples, double hz, double sampleRate) {
  if (numSamples < 8 || sampleRate <= 0.0 || hz <= 0.0 || hz >= sampleRate / 2.0)
    return kFloorDb;
  // A Hann-windowed single-bin transform at exactly `hz`; twice the result
  // over the window's sum is the component's amplitude.
  std::complex<double> sum{0.0, 0.0};
  double windowSum = 0.0;
  const double step = 2.0 * kPi * hz / sampleRate;
  for (int i = 0; i < numSamples; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (numSamples - 1));
    sum += w * static_cast<double>(samples[i]) *
           std::complex<double>(std::cos(step * i), -std::sin(step * i));
    windowSum += w;
  }
  return amplitudeToDb(2.0 * std::abs(sum) / windowSum);
}

Hum measureHum(const float* samples, int numSamples, double sampleRate) {
  Hum hum;
  // A Hann window's main lobe is 4 / T wide; under a second 50 and 60 Hz
  // cannot be told from what lies between them.
  if (samples == nullptr || sampleRate <= 0.0 || numSamples < static_cast<int>(sampleRate))
    return hum;

  hum.hz50Db = toneLevelDb(samples, numSamples, 50.0, sampleRate);
  hum.hz60Db = toneLevelDb(samples, numSamples, 60.0, sampleRate);

  // Mains hum is a line: it stands well above what is a few Hz either side
  // of it and above the other mains frequency. Guitar notes near 50 or 60 Hz
  // do not hold still enough to pass both tests over a whole take.
  const auto stands = [&](double hz, double levelDb, double otherDb) {
    const double beside = std::max(toneLevelDb(samples, numSamples, hz - 4.0, sampleRate),
                                   toneLevelDb(samples, numSamples, hz + 4.0, sampleRate));
    return levelDb > kFloorDb + 1.0 && levelDb - beside >= 12.0 && levelDb - otherDb >= 6.0;
  };
  if (stands(50.0, hum.hz50Db, hum.hz60Db))
    hum.mainsHz = 50;
  else if (stands(60.0, hum.hz60Db, hum.hz50Db))
    hum.mainsHz = 60;

  if (hum.mainsHz != 0) {
    // The fundamental, and the second and third harmonic where they stand
    // out as lines too: 100 to 180 Hz is where guitar notes live, and a note
    // is not hum.
    double power = std::pow(10.0, (hum.mainsHz == 50 ? hum.hz50Db : hum.hz60Db) / 10.0);
    for (int harmonic = 2; harmonic <= 3; ++harmonic) {
      const double hz = hum.mainsHz * harmonic;
      const double db = toneLevelDb(samples, numSamples, hz, sampleRate);
      const double beside = std::max(toneLevelDb(samples, numSamples, hz - 4.0, sampleRate),
                                     toneLevelDb(samples, numSamples, hz + 4.0, sampleRate));
      if (db - beside >= 12.0)
        power += std::pow(10.0, db / 10.0);
    }
    hum.levelDb = powerToDb(power);
  }
  return hum;
}

double noiseFloorDb(const float* samples, int numSamples, double sampleRate) {
  const int window = std::max(1, static_cast<int>(std::lround(0.05 * sampleRate)));
  std::vector<double> powers;
  for (int start = 0; start + window <= numSamples; start += window) {
    double sum = 0.0;
    for (int i = 0; i < window; ++i)
      sum += static_cast<double>(samples[start + i]) * samples[start + i];
    powers.push_back(sum / window);
  }
  if (powers.empty())
    return kFloorDb;
  std::sort(powers.begin(), powers.end());
  const size_t count = std::max<size_t>(1, powers.size() / 10);
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i)
    sum += powers[i];
  return powerToDb(sum / static_cast<double>(count));
}

// ####################
// LINEAR FIT
// ####################
LinearFit::LinearFit(const float* input, const float* output, int numSamples, double sampleRate)
    : rate(sampleRate) {
  const int count =
      std::min(numSamples, static_cast<int>(std::lround(kMaxFitSeconds * sampleRate)));
  if (input == nullptr || output == nullptr || count < 1024 || sampleRate <= 0.0)
    return;

  // Too quiet on either side (under -100 dBFS RMS) to say anything.
  double inputEnergy = 0.0, outputEnergy = 0.0;
  for (int i = 0; i < count; ++i) {
    inputEnergy += static_cast<double>(input[i]) * input[i];
    outputEnergy += static_cast<double>(output[i]) * output[i];
  }
  const double quiet = 1.0e-10 * count;
  if (inputEnergy <= quiet || outputEnergy <= quiet)
    return;

  // Twice the signal, so the fitted filter's response has room after the
  // input ends and nothing wraps around.
  const int order = nextPowerOfTwoOrder(count * 2);
  fftSize = 1 << order;
  const int bins = fftSize / 2 + 1;
  juce::dsp::FFT fft(order);

  const std::vector<float> x = spectrumOf(fft, input, count);
  outputSpectrum = spectrumOf(fft, output, count);

  inputPower.resize(static_cast<size_t>(bins));
  double peakInputPower = 0.0, totalOutput = 0.0;
  for (int bin = 0; bin < bins; ++bin) {
    const double re = x[static_cast<size_t>(bin) * 2], im = x[static_cast<size_t>(bin) * 2 + 1];
    inputPower[static_cast<size_t>(bin)] = static_cast<float>(re * re + im * im);
    peakInputPower = std::max(peakInputPower, re * re + im * im);
    totalInput += re * re + im * im;
    const double yr = outputSpectrum[static_cast<size_t>(bin) * 2];
    const double yi = outputSpectrum[static_cast<size_t>(bin) * 2 + 1];
    totalOutput += yr * yr + yi * yi;
  }
  if (peakInputPower <= 0.0 || totalOutput <= 0.0)
    return;

  // Step one: output over input, bin by bin, wherever the input has
  // something to divide by (within 40 dB of its strongest bin). Where it has
  // not, the ratio would be noise over noise, so the filter is carried
  // across the gap in a straight line from the bins either side: nothing is
  // known about it there, and nothing much is predicted there either, the
  // input being all but absent.
  const double usable = 1.0e-4 * peakInputPower;
  std::vector<float> response(static_cast<size_t>(fftSize) * 2, 0.0f);
  int lastUsable = -1;
  const auto fillGap = [&](int from, int to) {
    // Bins strictly between `from` and `to`; either end may be missing.
    const int first = from + 1, last = to - 1;
    for (int bin = first; bin <= last; ++bin) {
      double re = 0.0, im = 0.0;
      if (from >= 0 && to < bins) {
        const double t = static_cast<double>(bin - from) / (to - from);
        re = (1.0 - t) * response[static_cast<size_t>(from) * 2] + t * response[static_cast<size_t>(to) * 2];
        im = (1.0 - t) * response[static_cast<size_t>(from) * 2 + 1] +
             t * response[static_cast<size_t>(to) * 2 + 1];
      } else {
        const int known = from >= 0 ? from : to;
        re = response[static_cast<size_t>(known) * 2];
        im = response[static_cast<size_t>(known) * 2 + 1];
      }
      response[static_cast<size_t>(bin) * 2] = static_cast<float>(re);
      response[static_cast<size_t>(bin) * 2 + 1] = static_cast<float>(im);
    }
  };
  for (int bin = 0; bin < bins; ++bin) {
    if (inputPower[static_cast<size_t>(bin)] <= usable)
      continue;
    const std::complex<double> xin(x[static_cast<size_t>(bin) * 2],
                                   x[static_cast<size_t>(bin) * 2 + 1]);
    const std::complex<double> yout(outputSpectrum[static_cast<size_t>(bin) * 2],
                                    outputSpectrum[static_cast<size_t>(bin) * 2 + 1]);
    const std::complex<double> h = yout / xin;
    response[static_cast<size_t>(bin) * 2] = static_cast<float>(h.real());
    response[static_cast<size_t>(bin) * 2 + 1] = static_cast<float>(h.imag());
    fillGap(lastUsable, bin);
    lastUsable = bin;
  }
  fillGap(lastUsable, bins);
  // A real signal's spectrum is real at zero and at half the sample rate.
  response[1] = 0.0f;
  response[static_cast<size_t>(bins - 1) * 2 + 1] = 0.0f;

  // Step two: that ratio explains everything, noise and distortion
  // included. What makes it a model of the linear part is cutting its
  // impulse response to a span far shorter than the signal: an eighth of it
  // after time zero (at most a second, at least 4096 samples) and a quarter
  // of that before (the output is latency compensated, but a phase shift at
  // a single frequency is not a delay, and needs both sides). Each side is
  // flat over three quarters and faded over the rest. A rig whose response
  // fits inside (an amp, a cab, a short delay) is modelled exactly; what
  // comes later (a long delay, a reverb tail) is not and reads as residue.
  fft.performRealOnlyInverseTransform(response.data());
  const int after =
      juce::jlimit(4096, std::max(4096, static_cast<int>(sampleRate)), count / 8);
  const int before = after / 4;
  const auto side = [](int lag, int length) {
    const int flat = length * 3 / 4;
    if (lag <= flat)
      return 1.0;
    if (lag < length)
      return 0.5 + 0.5 * std::cos(kPi * (lag - flat) / (length - flat));
    return 0.0;
  };
  for (int i = 0; i < fftSize; ++i) {
    const double gain = i <= fftSize / 2 ? side(i, after) : side(fftSize - i, before);
    response[static_cast<size_t>(i)] *= static_cast<float>(gain);
  }
  std::fill(response.begin() + fftSize, response.end(), 0.0f);
  fft.performRealOnlyForwardTransform(response.data(), true);

  predicted.resize(static_cast<size_t>(bins) * 2);
  double residual = 0.0;
  for (int bin = 0; bin < bins; ++bin) {
    const std::complex<double> h(response[static_cast<size_t>(bin) * 2],
                                 response[static_cast<size_t>(bin) * 2 + 1]);
    const std::complex<double> xin(x[static_cast<size_t>(bin) * 2],
                                   x[static_cast<size_t>(bin) * 2 + 1]);
    const std::complex<double> yout(outputSpectrum[static_cast<size_t>(bin) * 2],
                                    outputSpectrum[static_cast<size_t>(bin) * 2 + 1]);
    const std::complex<double> estimate = h * xin;
    predicted[static_cast<size_t>(bin) * 2] = static_cast<float>(estimate.real());
    predicted[static_cast<size_t>(bin) * 2 + 1] = static_cast<float>(estimate.imag());
    residual += std::norm(yout - estimate);
  }

  result.valid = true;
  const double ratio = juce::jlimit(0.0, 1.0, residual / totalOutput);
  result.percent = 100.0 * std::sqrt(ratio);
  result.residualDb = powerToDb(ratio);
}

double LinearFit::gainDbAt(double hz) const {
  if (fftSize == 0 || hz <= 0.0 || hz >= rate / 2.0)
    return kFloorDb;
  // The fitted filter's power gain over a twelfth of an octave either side:
  // what it predicts there over what went in. A single bin of a noise probe
  // is too ragged to read.
  const double binHz = rate / fftSize;
  const int low = std::max(1, static_cast<int>(std::floor(hz * std::pow(2.0, -1.0 / 12.0) / binHz)));
  const int high = std::min(fftSize / 2, static_cast<int>(std::ceil(hz * std::pow(2.0, 1.0 / 12.0) / binHz)));
  double predictedPower = 0.0, inputPowerSum = 0.0;
  for (int bin = low; bin <= high; ++bin) {
    const double pr = predicted[static_cast<size_t>(bin) * 2];
    const double pi = predicted[static_cast<size_t>(bin) * 2 + 1];
    predictedPower += pr * pr + pi * pi;
    inputPowerSum += inputPower[static_cast<size_t>(bin)];
  }
  // Nothing went in around this frequency (less than a millionth of an even
  // share of the input): there is no gain to speak of.
  const double evenShare = totalInput * (high - low + 1) / (fftSize / 2 + 1);
  if (inputPowerSum <= 1.0e-6 * evenShare || inputPowerSum <= 0.0)
    return kFloorDb;
  return powerToDb(predictedPower / inputPowerSum);
}

double LinearFit::coherenceAt(double hz) const {
  if (fftSize == 0 || hz <= 0.0 || hz >= rate / 2.0)
    return 0.0;
  const double binHz = rate / fftSize;
  const int low = std::max(1, static_cast<int>(std::floor(hz * std::pow(2.0, -1.0 / 6.0) / binHz)));
  const int high = std::min(fftSize / 2, static_cast<int>(std::ceil(hz * std::pow(2.0, 1.0 / 6.0) / binHz)));
  double residual = 0.0, total = 0.0;
  for (int bin = low; bin <= high; ++bin) {
    const double yr = outputSpectrum[static_cast<size_t>(bin) * 2];
    const double yi = outputSpectrum[static_cast<size_t>(bin) * 2 + 1];
    const double dr = yr - predicted[static_cast<size_t>(bin) * 2];
    const double di = yi - predicted[static_cast<size_t>(bin) * 2 + 1];
    total += yr * yr + yi * yi;
    residual += dr * dr + di * di;
  }
  return total > 0.0 ? juce::jlimit(0.0, 1.0, 1.0 - residual / total) : 0.0;
}

Distortion measureDistortion(const float* input, const float* output, int numSamples,
                             double sampleRate) {
  return LinearFit(input, output, numSamples, sampleRate).distortion();
}

// ####################
// TAIL
// ####################
double tailSeconds(const juce::AudioBuffer<float>& output, int inputEnd, double sampleRate,
                   bool* truncated) {
  if (truncated != nullptr)
    *truncated = false;
  const int frames = output.getNumSamples();
  const int channels = output.getNumChannels();
  inputEnd = juce::jlimit(0, frames, inputEnd);
  if (frames <= 0 || channels <= 0 || sampleRate <= 0.0 || inputEnd <= 0)
    return 0.0;

  const auto power = [&](int start, int end) {
    double sum = 0.0;
    for (int ch = 0; ch < channels; ++ch) {
      const float* x = output.getReadPointer(ch);
      for (int i = start; i < end; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    }
    return end > start ? sum / (static_cast<double>(end - start) * channels) : 0.0;
  };

  const double programme = power(0, inputEnd);
  if (programme <= 1.0e-12)
    return 0.0;
  // 60 dB below the level while the input played, and never below -110 dBFS,
  // where rounding noise lives.
  const double threshold = std::max(programme * 1.0e-6, 1.0e-11);

  const int window = std::max(1, static_cast<int>(std::lround(0.01 * sampleRate)));
  int lastLoudEnd = inputEnd;
  for (int start = inputEnd; start < frames; start += window) {
    const int end = std::min(frames, start + window);
    if (power(start, end) >= threshold)
      lastLoudEnd = end;
  }
  if (truncated != nullptr)
    *truncated = lastLoudEnd >= frames && frames > inputEnd;
  return static_cast<double>(lastLoudEnd - inputEnd) / sampleRate;
}

// ####################
// JSON
// ####################
juce::var toVar(const Levels& levels) {
  return object({
      {"peakDb", tidyDb(levels.peakDb)},
      {"truePeakDb", tidyDb(levels.truePeakDb)},
      {"rmsDb", tidyDb(levels.rmsDb)},
      {"crestDb", tidyDb(levels.crestDb)},
      {"lufs", tidyDb(levels.lufs)},
  });
}

juce::var toVar(const Spectrum& spectrum) {
  juce::DynamicObject::Ptr bands = new juce::DynamicObject();
  juce::DynamicObject::Ptr shares = new juce::DynamicObject();
  for (size_t band = 0; band < kBands.size(); ++band) {
    bands->setProperty(kBands[band].id, tidyDb(spectrum.db[band]));
    shares->setProperty(kBands[band].id, tidy(spectrum.share[band], 10000.0));
  }
  return object({
      {"bandsDb", juce::var(bands.get())},
      {"bandShare", juce::var(shares.get())},
      {"centroidHz", tidy(spectrum.centroidHz, 10.0)},
      {"lowEndShare", tidy(spectrum.lowEndShare, 10000.0)},
  });
}

juce::var toVar(const Stereo& stereo) {
  return object({
      {"correlation", tidy(stereo.correlation, 10000.0)},
      {"width", tidy(stereo.width, 10000.0)},
      {"balanceDb", tidyDb(stereo.balanceDb)},
  });
}

juce::var toVar(const Distortion& distortion) {
  return object({
      {"percent", tidy(distortion.percent, 1000.0)},
      {"residualDb", tidyDb(distortion.residualDb)},
      {"valid", distortion.valid},
  });
}

juce::var toVar(const Hum& hum) {
  return object({
      {"hz50Db", tidyDb(hum.hz50Db)},
      {"hz60Db", tidyDb(hum.hz60Db)},
      {"mainsHz", hum.mainsHz},
      {"levelDb", tidyDb(hum.levelDb)},
  });
}

}  // namespace AudioMeasure
