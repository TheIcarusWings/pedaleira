#include "CaptureChecks.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace CaptureChecks {

namespace {

constexpr double kSilenceDb = -120.0;

double toDb(double linear) { return linear > 1.0e-6 ? 20.0 * std::log10(linear) : kSilenceDb; }

// juce::String(double, 0) prints every digit, so whole decibels are rounded.
juce::String db(double value, int places = 1) {
  return (places > 0 ? juce::String(value, places) : juce::String(juce::roundToInt(value))) + " dB";
}

Result make(const char* id, const char* name, Status status, const juce::String& value,
            const juce::String& message) {
  Result r;
  r.id = id;
  r.name = name;
  r.status = status;
  r.value = value;
  r.message = message;
  return r;
}

Result withNumber(Result r, double number) {
  r.number = number;
  r.hasNumber = true;
  return r;
}

Status worse(Status a, Status b) { return static_cast<int>(a) > static_cast<int>(b) ? a : b; }

double peakLinear(const std::vector<float>& x) {
  float peak = 0.0f;
  for (const float v : x)
    peak = std::max(peak, std::abs(v));
  return peak;
}

// The level check shared by every channel: the peak against the target band.
Result levelCheck(const char* id, const char* name, const std::vector<float>& x,
                  const Options& o) {
  const double peak = peakDb(x);
  const double low = o.targetDb - o.bandDb, high = o.targetDb + o.bandDb;
  if (peak < -50.0)
    return withNumber(make(id, name, Status::fail, db(peak),
                           "There is almost no signal. Check the cables, the channel and the "
                           "send level."),
                      peak);
  if (peak >= low && peak <= high)
    return withNumber(make(id, name, Status::pass, db(peak),
                           "The peak is within " + db(o.bandDb, 0) + " of the " +
                               db(o.targetDb, 0) + " target."),
                      peak);
  const double distance = peak < low ? low - peak : peak - high;
  const juce::String how = peak < low ? "The level is low. Raise the send level or the input "
                                        "gain, then record again."
                                      : "The level is high. Lower the send level or the input "
                                        "gain, then record again.";
  return withNumber(make(id, name, distance <= o.bandDb ? Status::warn : Status::fail, db(peak),
                         how + " Target " + db(o.targetDb, 0) + " plus or minus " +
                             db(o.bandDb, 0) + "."),
                    peak);
}

// Samples at or above the clip level, and the separate places they are in
// (runs less than a millisecond apart are one place).
Result clippingCheck(const char* id, const char* name, const std::vector<float>& x,
                     const Options& o) {
  const float threshold = static_cast<float>(std::pow(10.0, o.clipDb / 20.0));
  const int gap = std::max(1, static_cast<int>(o.sampleRate / 1000.0));
  int samples = 0, places = 0;
  long long lastClipped = -1000000;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::abs(x[i]) >= threshold) {
      ++samples;
      if (static_cast<long long>(i) - lastClipped > gap)
        ++places;
      lastClipped = static_cast<long long>(i);
    }
  if (samples == 0)
    return withNumber(make(id, name, Status::pass, "none", "No sample reaches " + db(o.clipDb) +
                                                               "."),
                      0.0);
  const juce::String value = juce::String(samples) + (samples == 1 ? " sample" : " samples") +
                             " in " + juce::String(places) + (places == 1 ? " place" : " places");
  return withNumber(
      make(id, name, places <= 2 ? Status::warn : Status::fail, value,
           "The signal reaches " + db(o.clipDb) + " or more, so it is clipped. Lower the send "
           "level or the input gain and record again."),
      samples);
}

Result dcCheck(const char* id, const char* name, const std::vector<float>& x) {
  const double dc = dcOffsetDb(x);
  const Status status = dc < -50.0 ? Status::pass : (dc < -40.0 ? Status::warn : Status::fail);
  return withNumber(make(id, name, status, db(dc),
                         status == Status::pass
                             ? "No offset to speak of."
                             : "The signal sits off zero. A DC offset in the chain (a "
                               "cable, a pedal's input) becomes part of the capture."),
                    dc);
}

// The noise: how far the loudest peak is above the quietest 100 ms. A guitar
// played without a pause has no quiet stretch, so for playing it only warns.
Result noiseCheck(const char* id, const char* name, const std::vector<float>& x, const Options& o,
                  bool failIsWarn) {
  const double peak = peakDb(x);
  const double floor = quietestStretchDb(x, o.sampleRate);
  const double snr = peak - floor;
  Status status = snr >= 45.0 ? Status::pass : (snr >= 35.0 ? Status::warn : Status::fail);
  if (failIsWarn && status == Status::fail)
    status = Status::warn;
  const juce::String value = db(floor) + ", " + db(snr, 0) + " below the peak";
  const juce::String message =
      status == Status::pass
          ? "The quietest stretch is well below the signal."
          : (failIsWarn ? "The quietest stretch is not far below the signal. Either the playing "
                          "has no pause to measure the noise in, or the noise is high: hum, a "
                          "noisy pedal or a low level all end up in the capture."
                        : "The noise is high next to the signal. Hum, a noisy pedal or a low "
                          "level all end up in the capture.");
  return withNumber(make(id, name, status, value, message), floor);
}

Result latencyResult(const char* id, const char* name, const LatencyEstimate& e,
                     const Options& o, bool signedLag) {
  if (!e.found || e.confidence < 0.05)
    return make(id, name, Status::fail, "not found",
                "The return does not follow what was played. Check the cables, that the send "
                "and return channels are the ones wired, and that the send level is up.");
  const double ms = 1000.0 * e.samples / o.sampleRate;
  const juce::String value = juce::String(e.samples) + " samples, " + juce::String(ms, 1) + " ms";
  const int magnitude = std::abs(e.samples);
  Result r = withNumber(make(id, name, Status::pass, value, ""), e.samples);
  if (magnitude > o.maxLatencySamples) {
    r.status = Status::fail;
    r.message = "The delay is longer than the trainer can calibrate (" +
                juce::String(o.maxLatencySamples) + " samples).";
  } else if (e.confidence < 0.2) {
    r.status = Status::warn;
    r.message = "The match is weak (" + juce::String(e.confidence, 2) +
                "). The device adds a lot of noise or distortion to what was played.";
  } else {
    r.message = signedLag && e.samples < 0 ? "The wet input comes before the dry one."
                                           : "Lined up automatically; the trainer is given this delay.";
  }
  return r;
}

// Start of the window of `window` samples with the most energy.
int loudestWindowStart(const std::vector<float>& x, int window, double rate) {
  const int n = static_cast<int>(x.size());
  if (n <= window)
    return 0;
  const int block = std::max(1, static_cast<int>(rate / 4.0));
  const int blocks = n / block;
  std::vector<double> energy(static_cast<size_t>(blocks), 0.0);
  for (int b = 0; b < blocks; ++b) {
    double sum = 0.0;
    for (int i = b * block; i < (b + 1) * block; ++i)
      sum += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    energy[static_cast<size_t>(b)] = sum;
  }
  const int span = std::max(1, window / block);
  double running = 0.0, best = -1.0;
  int bestStart = 0;
  for (int b = 0; b < blocks; ++b) {
    running += energy[static_cast<size_t>(b)];
    if (b >= span)
      running -= energy[static_cast<size_t>(b - span)];
    if (b >= span - 1 && running > best) {
      best = running;
      bestStart = (b - span + 1) * block;
    }
  }
  return std::min(bestStart, n - window);
}

// How much of `response` (lagging `reference` by `lag`) is left after the
// best gain is taken out: 0 for a scaled copy, 1 for something unrelated.
double residualRatio(const std::vector<float>& reference, const std::vector<float>& response,
                     int lag, int start, int count) {
  double rr = 0.0, rs = 0.0, ss = 0.0;
  for (int i = 0; i < count; ++i) {
    const size_t a = static_cast<size_t>(start + i), b = static_cast<size_t>(start + i + lag);
    if (b >= response.size() || a >= reference.size())
      break;
    rr += static_cast<double>(reference[a]) * reference[a];
    rs += static_cast<double>(reference[a]) * response[b];
    ss += static_cast<double>(response[b]) * response[b];
  }
  if (rr <= 0.0 || ss <= 0.0)
    return 1.0;
  const double gain = rs / rr;
  return std::max(0.0, (ss - 2.0 * gain * rs + gain * gain * rr) / ss);
}

}  // namespace

const char* statusName(Status status) {
  switch (status) {
    case Status::pass: return "pass";
    case Status::warn: return "warn";
    case Status::fail: return "fail";
  }
  return "fail";
}

// ####################
// REPORT
// ####################
const Result* Report::find(const juce::String& id) const {
  for (const auto& r : results)
    if (r.id == id)
      return &r;
  return nullptr;
}

int Report::count(Status status) const {
  int n = 0;
  for (const auto& r : results)
    n += r.status == status ? 1 : 0;
  return n;
}

Report Report::fromResults(std::vector<Result> results) {
  Report report;
  report.results = std::move(results);
  for (const auto& r : report.results)
    report.verdict = worse(report.verdict, r.status);
  return report;
}

juce::var Report::toVar() const {
  juce::DynamicObject::Ptr obj = new juce::DynamicObject();
  obj->setProperty("verdict", statusName(verdict));
  obj->setProperty("passed", count(Status::pass));
  obj->setProperty("warnings", count(Status::warn));
  obj->setProperty("failures", count(Status::fail));
  if (latencyKnown) {
    obj->setProperty("latencySamples", latencySamples);
    obj->setProperty("latencyMs", std::round(latencyMs * 100.0) / 100.0);
    obj->setProperty("latencyConfidence", std::round(latencyConfidence * 1000.0) / 1000.0);
  }
  juce::Array<juce::var> list;
  for (const auto& r : results) {
    juce::DynamicObject::Ptr item = new juce::DynamicObject();
    item->setProperty("id", r.id);
    item->setProperty("name", r.name);
    item->setProperty("status", statusName(r.status));
    item->setProperty("value", r.value);
    item->setProperty("message", r.message);
    if (r.hasNumber)
      item->setProperty("number", r.number);
    list.add(juce::var(item.get()));
  }
  obj->setProperty("checks", list);
  return juce::var(obj.get());
}

// ####################
// LEVELS
// ####################
double peakDb(const std::vector<float>& x) { return toDb(peakLinear(x)); }

double rmsDb(const std::vector<float>& x) {
  if (x.empty())
    return kSilenceDb;
  double sum = 0.0;
  for (const float v : x)
    sum += static_cast<double>(v) * v;
  return toDb(std::sqrt(sum / static_cast<double>(x.size())));
}

double dcOffsetDb(const std::vector<float>& x) {
  if (x.empty())
    return kSilenceDb;
  double sum = 0.0;
  for (const float v : x)
    sum += v;
  return toDb(std::abs(sum / static_cast<double>(x.size())));
}

double quietestStretchDb(const std::vector<float>& x, double sampleRate) {
  const size_t window = std::max<size_t>(1, static_cast<size_t>(sampleRate / 10.0));
  if (x.size() < window)
    return rmsDb(x);
  double quietest = 1.0e30;
  for (size_t start = 0; start + window <= x.size(); start += window) {
    double sum = 0.0;
    for (size_t i = start; i < start + window; ++i)
      sum += static_cast<double>(x[i]) * x[i];
    quietest = std::min(quietest, sum / static_cast<double>(window));
  }
  return toDb(std::sqrt(quietest));
}

// ####################
// DELAY
// ####################
LatencyEstimate estimateLatency(const std::vector<float>& reference,
                                const std::vector<float>& response, int maxLag, int window,
                                int referenceStart) {
  LatencyEstimate best;
  if (referenceStart < 0 || referenceStart >= static_cast<int>(reference.size()) ||
      referenceStart >= static_cast<int>(response.size()))
    return best;
  const int n = std::min(window, static_cast<int>(reference.size()) - referenceStart);
  const int respLen =
      std::min(n + std::max(0, maxLag), static_cast<int>(response.size()) - referenceStart);
  if (n < 256 || respLen < 256)
    return best;
  const int lagLimit = std::min(std::max(0, maxLag), respLen - 1);

  int order = 8;
  while ((1 << order) < n + lagLimit + 1)
    ++order;
  const int size = 1 << order;
  using Complex = juce::dsp::Complex<float>;
  std::vector<Complex> a(static_cast<size_t>(size)), b(static_cast<size_t>(size)),
      product(static_cast<size_t>(size)), out(static_cast<size_t>(size));
  for (int i = 0; i < n; ++i)
    a[static_cast<size_t>(i)] = Complex(reference[static_cast<size_t>(referenceStart + i)], 0.0f);
  for (int i = 0; i < respLen; ++i)
    b[static_cast<size_t>(i)] = Complex(response[static_cast<size_t>(referenceStart + i)], 0.0f);
  juce::dsp::FFT fft(order);
  std::vector<Complex> fa(static_cast<size_t>(size)), fb(static_cast<size_t>(size));
  fft.perform(a.data(), fa.data(), false);
  fft.perform(b.data(), fb.data(), false);
  for (size_t i = 0; i < fa.size(); ++i)
    product[i] = std::conj(fa[i]) * fb[i];
  fft.perform(product.data(), out.data(), true);

  int bestLag = 0;
  float bestValue = -1.0f;
  for (int lag = 0; lag <= lagLimit; ++lag) {
    const float v = std::abs(out[static_cast<size_t>(lag)].real());
    if (v > bestValue) {
      bestValue = v;
      bestLag = lag;
    }
  }

  // The normalised correlation at that lag, in the time domain.
  double dot = 0.0, rr = 0.0, ss = 0.0;
  for (int i = 0; i < n; ++i) {
    const double r = reference[static_cast<size_t>(referenceStart + i)];
    const size_t at = static_cast<size_t>(referenceStart + i + bestLag);
    const double s = at < response.size() ? response[at] : 0.0;
    dot += r * s;
    rr += r * r;
    ss += s * s;
  }
  if (rr <= 0.0 || ss <= 0.0)
    return best;
  best.found = true;
  best.samples = bestLag;
  best.confidence = std::min(1.0, std::abs(dot) / std::sqrt(rr * ss));
  return best;
}

// ####################
// RE-AMP
// ####################
Report checkReamp(const std::vector<float>& played, const std::vector<float>& returned,
                  const Options& o) {
  std::vector<Result> results;

  // Length: the trainer takes a recording as long as the input file, or up
  // to a second longer; never shorter.
  {
    const double delta = (static_cast<double>(returned.size()) - static_cast<double>(played.size())) /
                         o.sampleRate;
    Result r = withNumber(make("length", "Length", Status::pass, "matches",
                               "The recording is as long as the signal that was played."),
                          delta);
    if (delta < -1.0e-9) {
      r.status = Status::fail;
      r.value = juce::String(-delta, 2) + " s short";
      r.message = "The recording is shorter than the signal. It was cut off; record again.";
    } else if (delta > 1.0) {
      r.status = Status::fail;
      r.value = juce::String(delta, 2) + " s long";
      r.message = "The recording is more than a second longer than the signal, which the "
                  "trainer refuses.";
    } else if (delta > 1.0e-9) {
      r.status = Status::warn;
      r.value = juce::String(delta, 2) + " s long";
      r.message = "The recording is a little longer than the signal; the trainer accepts up "
                  "to a second.";
    }
    results.push_back(r);
  }

  results.push_back(levelCheck("level", "Peak level", returned, o));
  results.push_back(clippingCheck("clipping", "Clipping", returned, o));
  results.push_back(noiseCheck("noise", "Noise floor", returned, o, false));

  const LatencyEstimate lag =
      estimateLatency(played, returned, o.searchLagSamples,
                      static_cast<int>(o.latencyWindowSeconds * o.sampleRate));
  results.push_back(latencyResult("latency", "Latency", lag, o, false));

  results.push_back(dcCheck("dc", "DC offset", returned));

  Report report = Report::fromResults(std::move(results));
  if (lag.found) {
    report.latencyKnown = true;
    report.latencySamples = lag.samples;
    report.latencyMs = 1000.0 * lag.samples / o.sampleRate;
    report.latencyConfidence = lag.confidence;
  }
  return report;
}

// ####################
// DRY AND WET
// ####################
Report checkDryWet(const std::vector<float>& dry, const std::vector<float>& wet,
                   const Options& o) {
  std::vector<Result> results;

  // Length: the playing time, and both takes the same.
  {
    const double seconds = static_cast<double>(std::min(dry.size(), wet.size())) / o.sampleRate;
    const double mismatch =
        std::abs(static_cast<double>(dry.size()) - static_cast<double>(wet.size())) / o.sampleRate;
    Result r = withNumber(make("length", "Length", Status::pass,
                               juce::String(seconds, 1) + " s", "Long enough to learn from."),
                          seconds);
    if (mismatch > 1.0) {
      r.status = Status::fail;
      r.message = "The two takes differ in length by more than a second.";
    } else if (seconds < o.failSeconds) {
      r.status = Status::fail;
      r.message = "Far too short. Play for two to three minutes.";
    } else if (seconds < o.minSeconds) {
      r.status = Status::warn;
      r.message = "Shorter than the two to three minutes that capture the gear well.";
    }
    results.push_back(r);
  }

  results.push_back(levelCheck("level.dry", "Dry level", dry, o));
  results.push_back(levelCheck("level.wet", "Wet level", wet, o));
  results.push_back(clippingCheck("clipping.dry", "Dry clipping", dry, o));
  results.push_back(clippingCheck("clipping.wet", "Wet clipping", wet, o));
  results.push_back(noiseCheck("noise.dry", "Dry noise floor", dry, o, true));
  results.push_back(noiseCheck("noise.wet", "Wet noise floor", wet, o, true));

  // The delay between the two: wet behind dry, or (negative) before it.
  const int window = static_cast<int>(o.latencyWindowSeconds * o.sampleRate);
  const int maxLag = std::min(o.searchLagSamples, 9000);
  LatencyEstimate lag;
  if (!dry.empty() && !wet.empty()) {
    const int start = loudestWindowStart(dry, window, o.sampleRate);
    const LatencyEstimate forward = estimateLatency(dry, wet, maxLag, window, start);
    const int startWet = loudestWindowStart(wet, window, o.sampleRate);
    LatencyEstimate backward = estimateLatency(wet, dry, maxLag, window, startWet);
    backward.samples = -backward.samples;
    lag = (backward.found && (!forward.found || backward.confidence > forward.confidence + 0.02))
              ? backward
              : forward;
    results.push_back(latencyResult("latency", "Dry to wet delay", lag, o, true));

    // Signal on both, and the wet not a copy of the dry.
    const bool signal = peakDb(dry) > -50.0 && peakDb(wet) > -50.0;
    if (signal && lag.found) {
      const int count = std::min(window, static_cast<int>(std::min(dry.size(), wet.size())) / 2);
      double ratio = 1.0;
      if (lag.samples >= 0)
        ratio = residualRatio(dry, wet, lag.samples, start, count);
      else
        ratio = residualRatio(wet, dry, -lag.samples, startWet, count);
      Status status = ratio < 5.0e-4 ? Status::fail : (ratio < 5.0e-3 ? Status::warn : Status::pass);
      results.push_back(withNumber(
          make("copy", "Wet differs from dry", status, "residual " + db(toDb(std::sqrt(ratio)), 0),
               status == Status::pass
                   ? "The wet signal carries something the dry one does not."
                   : "The wet input is almost a copy of the dry one. Is the amp on, and is the "
                     "wet input really the one after the gear?"),
          ratio));
    } else if (!signal) {
      results.push_back(make("copy", "Wet differs from dry", Status::fail, "no signal",
                             "One of the inputs has no signal, so the two cannot be compared."));
    }
  }

  results.push_back(dcCheck("dc.dry", "Dry DC offset", dry));
  results.push_back(dcCheck("dc.wet", "Wet DC offset", wet));

  Report report = Report::fromResults(std::move(results));
  if (lag.found) {
    report.latencyKnown = true;
    report.latencySamples = lag.samples;
    report.latencyMs = 1000.0 * lag.samples / o.sampleRate;
    report.latencyConfidence = lag.confidence;
  }
  return report;
}

}  // namespace CaptureChecks
