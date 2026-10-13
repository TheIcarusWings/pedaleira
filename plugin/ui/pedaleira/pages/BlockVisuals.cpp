// The pictures at the top of a block's settings, one family per category
// (BlockStyle.h). Each draws from the block's parameters only, plus the live
// levels and readings where the engine reports them.
#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "../widgets/Knob.h"
#include "BlockStyle.h"

#include <cmath>
#include <complex>

namespace pedaleira::ui {

// ####################
// MATH
// ####################
namespace blockmath {
namespace {
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double magnitudeDb(double hz, double b0, double b1, double b2, double a0, double a1, double a2) {
  const double w = 2.0 * kPi * juce::jlimit(1.0, kRate * 0.499, hz) / kRate;
  const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
  const std::complex<double> num = b0 + b1 * z1 + b2 * z2, den = a0 + a1 * z1 + a2 * z2;
  return 20.0 * std::log10(juce::jmax(1.0e-9, std::abs(num) / juce::jmax(1.0e-12, std::abs(den))));
}
}  // namespace

double peakDb(double hz, double centre, double gainDb, double q) {
  const double a = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * kPi * centre / kRate;
  const double alpha = std::sin(w0) / (2.0 * juce::jmax(0.05, q)), c = std::cos(w0);
  return magnitudeDb(hz, 1.0 + alpha * a, -2.0 * c, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * c, 1.0 - alpha / a);
}

double lowShelfDb(double hz, double centre, double gainDb) {
  const double a = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * kPi * centre / kRate;
  const double c = std::cos(w0), beta = 2.0 * std::sqrt(a) * (std::sin(w0) / 2.0 * std::sqrt(2.0));
  return magnitudeDb(hz, a * ((a + 1.0) - (a - 1.0) * c + beta), 2.0 * a * ((a - 1.0) - (a + 1.0) * c),
                     a * ((a + 1.0) - (a - 1.0) * c - beta), (a + 1.0) + (a - 1.0) * c + beta,
                     -2.0 * ((a - 1.0) + (a + 1.0) * c), (a + 1.0) + (a - 1.0) * c - beta);
}

double highShelfDb(double hz, double centre, double gainDb) {
  const double a = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * kPi * centre / kRate;
  const double c = std::cos(w0), beta = 2.0 * std::sqrt(a) * (std::sin(w0) / 2.0 * std::sqrt(2.0));
  return magnitudeDb(hz, a * ((a + 1.0) + (a - 1.0) * c + beta), -2.0 * a * ((a - 1.0) + (a + 1.0) * c),
                     a * ((a + 1.0) + (a - 1.0) * c - beta), (a + 1.0) - (a - 1.0) * c + beta,
                     2.0 * ((a - 1.0) - (a + 1.0) * c), (a + 1.0) - (a - 1.0) * c - beta);
}

double highPassDb(double hz, double corner, int order) {
  if (order <= 0)
    return 0.0;
  return -10.0 * std::log10(1.0 + std::pow(corner / juce::jmax(1.0, hz), 2.0 * order));
}

double lowPassDb(double hz, double corner, int order) {
  if (order <= 0)
    return 0.0;
  return -10.0 * std::log10(1.0 + std::pow(hz / juce::jmax(1.0, corner), 2.0 * order));
}

double compressorOut(double inDb, double thresholdDb, double ratio, double kneeDb) {
  const double over = inDb - thresholdDb, r = juce::jmax(1.0, ratio);
  if (kneeDb > 0.0 && std::abs(over) <= kneeDb * 0.5) {
    const double x = over + kneeDb * 0.5;
    return inDb + (1.0 / r - 1.0) * x * x / (2.0 * kneeDb);
  }
  return over <= 0.0 ? inDb : thresholdDb + over / r;
}
}  // namespace blockmath

namespace {

using juce::Rectangle;

void paintGrid(juce::Graphics& g, Rectangle<float> plot, int columns, int rows) {
  g.setColour(colour::line.withAlpha(0.7f));
  for (int c = 1; c < columns; ++c) {
    const float x = plot.getX() + plot.getWidth() * static_cast<float>(c) / static_cast<float>(columns);
    g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
  }
  for (int r = 1; r < rows; ++r) {
    const float y = plot.getY() + plot.getHeight() * static_cast<float>(r) / static_cast<float>(rows);
    g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
  }
}

void smallText(juce::Graphics& g, const juce::String& text, Rectangle<float> area, juce::Justification just,
               juce::Colour tint = colour::muted) {
  g.setFont(type::mono(10.0f, type::Weight::regular));
  g.setColour(tint);
  g.drawText(text, area.toNearestInt(), just, false);
}

void strokeCurve(juce::Graphics& g, const juce::Path& path, juce::Colour tint, float width = 2.0f) {
  g.setColour(tint);
  g.strokePath(path, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// ####################
// EQ AND SWEEP
// ####################
class EqVisual final : public BlockVisual {
public:
  explicit EqVisual(bool sweepMode) : sweep(sweepMode) {}

protected:
  juce::String heading() const override { return sweep ? "Sweep" : "Response"; }

  struct Band {
    enum Kind { peak, lowShelf, highShelf, highPass, lowPass } kind = peak;
    double hz = 1000.0, gain = 0.0, q = 1.0;
    int order = 2;
    int number = 0;
    bool active = true;
  };

  std::vector<Band> collect() const {
    std::vector<Band> bands;
    if (sweep) {
      Band band;
      band.kind = Band::peak;
      if (find({"positionPct"})) {
        band.hz = 350.0 * std::pow(2.0, position({"positionPct"}, 0.5) * 2.6);
      } else if (find({"centreHz", "centerHz"})) {
        band.hz = value({"centreHz", "centerHz"}, 800.0);
      } else {
        band.hz = value({"lowHz"}, 200.0) * std::pow(2.0, value({"rangeOct"}, 3.0) * 0.5);
      }
      band.gain = 6.0 + 12.0 * position({"resonancePct"}, 0.5);
      band.q = 1.5 + 5.0 * position({"resonancePct"}, 0.5);
      band.number = 1;
      bands.push_back(band);
      return bands;
    }
    for (int n = 1; n <= 8; ++n) {
      const juce::String id = "b" + juce::String(n);
      const KnobParam* freq = nullptr;
      for (const auto& p : params)
        if (p.id == id + "Freq")
          freq = &p;
      if (freq == nullptr)
        continue;
      Band band;
      band.number = n;
      band.hz = freq->value;
      for (const auto& p : params) {
        if (p.id == id + "Gain")
          band.gain = p.value;
        else if (p.id == id + "Q")
          band.q = p.value;
        else if (p.id == id + "Bypass")
          band.active = p.value < 0.5;
        else if (p.id == id + "Type") {
          const int type = juce::roundToInt(p.value);
          // The first band: peak, low shelf, high-pass; the last: peak, high shelf, low-pass.
          const bool first = n == 1;
          if (type == 1)
            band.kind = first ? Band::lowShelf : Band::highShelf;
          else if (type == 2)
            band.kind = first ? Band::highPass : Band::lowPass;
        }
      }
      bands.push_back(band);
    }
    if (!bands.empty())
      return bands;
    // The console equaliser.
    const auto add = [&](Band::Kind kind, const char* freqId, const char* gainId, double q, int number) {
      if (find({freqId}) == nullptr)
        return;
      Band band;
      band.kind = kind;
      band.hz = value({freqId}, 1000.0);
      band.gain = gainId != nullptr ? value({gainId}, 0.0) : 0.0;
      band.q = q;
      band.number = number;
      bands.push_back(band);
    };
    if (find({"lowGain"}) != nullptr) {
      add(Band::lowShelf, "lowFreq", "lowGain", 0.7, 1);
      add(Band::peak, "lowMidFreq", "lowMidGain", 1.0, 2);
      add(Band::peak, "highMidFreq", "highMidGain", 1.0, 3);
      add(Band::highShelf, "highFreq", "highGain", 0.7, 4);
      if (flag({"hpOn"}, false))
        add(Band::highPass, "hpFreq", nullptr, 0.7, 0);
      if (flag({"lpOn"}, false))
        add(Band::lowPass, "lpFreq", nullptr, 0.7, 0);
      return bands;
    }
    // The cuts.
    static const int orders[7] = {0, 1, 2, 3, 4, 6, 8};
    for (const char* which : {"hp", "lp"}) {
      const juce::String id(which);
      Band band;
      band.kind = id == "hp" ? Band::highPass : Band::lowPass;
      band.hz = value({(id + "Freq").toRawUTF8()}, id == "hp" ? 80.0 : 12000.0);
      band.order = orders[juce::jlimit(0, 6, choice({(id + "Slope").toRawUTF8()}, 0))];
      if (find({(id + "Freq").toRawUTF8()}) != nullptr && band.order > 0)
        bands.push_back(band);
    }
    return bands;
  }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    auto plot = area.withTrimmedBottom(14.0f).withTrimmedLeft(24.0f);
    const double lo = sweep ? -12.0 : -18.0, hi = sweep ? 24.0 : 18.0;
    const auto bands = collect();
    const auto xOf = [&](double hz) {
      return plot.getX() + plot.getWidth() * static_cast<float>(std::log(hz / 20.0) / std::log(1000.0));
    };
    const auto yOf = [&](double db) {
      return plot.getBottom() - plot.getHeight() * static_cast<float>((juce::jlimit(lo, hi, db) - lo) / (hi - lo));
    };
    paintGrid(g, plot, 6, 4);
    for (const double hz : {100.0, 1000.0, 10000.0}) {
      g.setColour(colour::faint);
      g.drawVerticalLine(juce::roundToInt(xOf(hz)), plot.getY(), plot.getBottom());
      smallText(g, hz >= 1000.0 ? juce::String(juce::roundToInt(hz / 1000.0)) + "k" : juce::String(juce::roundToInt(hz)),
                {xOf(hz) - 16.0f, plot.getBottom() + 1.0f, 32.0f, 12.0f}, juce::Justification::centred);
    }
    const std::vector<double> marks = sweep ? std::vector<double>{0.0, 12.0} : std::vector<double>{-12.0, 0.0, 12.0};
    for (const double db : marks)
      smallText(g, (db > 0 ? "+" : "") + juce::String(juce::roundToInt(db)), {area.getX(), yOf(db) - 6.0f, 22.0f, 12.0f},
                juce::Justification::centredRight);
    g.setColour(colour::dim);
    g.drawHorizontalLine(juce::roundToInt(yOf(0.0)), plot.getX(), plot.getRight());

    juce::Path curve, fill;
    const int steps = juce::jmax(24, juce::roundToInt(plot.getWidth() / 2.0f));
    for (int i = 0; i <= steps; ++i) {
      const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / steps);
      double db = 0.0;
      for (const Band& band : bands) {
        if (!band.active)
          continue;
        switch (band.kind) {
          case Band::peak: db += blockmath::peakDb(hz, band.hz, band.gain, band.q); break;
          case Band::lowShelf: db += blockmath::lowShelfDb(hz, band.hz, band.gain); break;
          case Band::highShelf: db += blockmath::highShelfDb(hz, band.hz, band.gain); break;
          case Band::highPass: db += blockmath::highPassDb(hz, band.hz, band.order); break;
          case Band::lowPass: db += blockmath::lowPassDb(hz, band.hz, band.order); break;
        }
      }
      const juce::Point<float> at(xOf(hz), yOf(db));
      if (i == 0) {
        curve.startNewSubPath(at);
        fill.startNewSubPath(at.x, yOf(0.0));
        fill.lineTo(at);
      } else {
        curve.lineTo(at);
        fill.lineTo(at);
      }
    }
    fill.lineTo(plot.getRight(), yOf(0.0));
    fill.closeSubPath();
    g.setColour(accent.withAlpha(0.2f));
    g.fillPath(fill);
    strokeCurve(g, curve, accent);
    for (const Band& band : bands) {
      if (band.number == 0)
        continue;
      const juce::Point<float> at(xOf(band.hz), yOf(band.kind == Band::highPass || band.kind == Band::lowPass ? 0.0 : band.gain));
      g.setColour(colour::ground);
      g.fillEllipse(Rectangle<float>(10.0f, 10.0f).withCentre(at));
      g.setColour(band.active ? accent : colour::faint);
      g.drawEllipse(Rectangle<float>(10.0f, 10.0f).withCentre(at), 1.5f);
      if (bands.size() > 1)
        smallText(g, juce::String(band.number), Rectangle<float>(14.0f, 10.0f).withCentre(at.translated(0.0f, -13.0f)),
                  juce::Justification::centred, colour::text);
    }
    if (bands.empty())
      smallText(g, "Flat", plot, juce::Justification::centred);
  }

private:
  bool sweep;
};

// ####################
// DYNAMICS
// ####################
class TransferVisual final : public BlockVisual {
protected:
  juce::String heading() const override { return "Transfer"; }

  bool liveMatters(const BlockLive& before, const BlockLive& after) const override {
    const auto gr = [](const BlockLive& l) {
      const auto it = l.readings.find("gainReductionDb");
      return it != l.readings.end() ? it->second : 0.0;
    };
    return std::abs(gr(before) - gr(after)) > 0.25 || std::abs(before.inDb - after.inDb) > 1.0f;
  }

  struct Law {
    double threshold = -20.0, ratio = 4.0, knee = 0.0, range = 0.0, ceiling = 0.0, inputGain = 0.0, makeup = 0.0;
    enum Kind { compressor, gate, limiter } kind = compressor;
  };

  Law law() const {
    Law l;
    if (find({"rangeDb"})) {
      l.kind = Law::gate;
      l.threshold = value({"thresholdDb"}, -50.0);
      l.range = value({"rangeDb"}, -60.0);
      return l;
    }
    if (find({"ceilingDb"})) {
      l.kind = Law::limiter;
      l.ceiling = value({"ceilingDb"}, -1.0);
      l.inputGain = value({"inputGainDb"}, 0.0);
      return l;
    }
    if (const KnobParam* t = find({"thresholdDb"})) {
      l.threshold = t->value;
    } else if (find({"peakReduction"})) {
      l.threshold = -8.0 - 40.0 * position({"peakReduction"}, 0.5);
    } else if (find({"sustain"})) {
      l.threshold = -10.0 - 40.0 * position({"sustain"}, 0.5);
    } else if (find({"inputDb"})) {
      l.threshold = -4.0 - value({"inputDb"}, 12.0);
    } else {
      l.threshold = -20.0;
    }
    if (const KnobParam* r = find({"ratio"})) {
      if (r->type == "choice") {
        const juce::String label = r->choices[juce::jlimit(0, juce::jmax(0, r->choices.size() - 1), juce::roundToInt(r->value))];
        l.ratio = label.containsIgnoreCase("all") ? 20.0 : juce::jmax(1.0, label.upToFirstOccurrenceOf(":", false, false).getDoubleValue());
      } else {
        l.ratio = r->value;
      }
    } else if (find({"mode"}) && choice({"mode"}, 0) == 1) {
      l.ratio = 20.0;
    } else {
      l.ratio = find({"sustain"}) ? 6.0 : 4.0;
    }
    l.knee = value({"kneeDb"}, 0.0);
    l.makeup = value({"makeupDb"}, 0.0);
    return l;
  }

  static double outOf(const Law& l, double in) {
    switch (l.kind) {
      case Law::gate: return in < l.threshold ? in + l.range : in;
      case Law::limiter: return juce::jmin(in + l.inputGain, l.ceiling);
      default: return blockmath::compressorOut(in, l.threshold, l.ratio, l.knee) + l.makeup;
    }
  }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    const Law l = law();
    auto reduction = area.removeFromRight(30.0f);
    area.removeFromRight(8.0f);
    auto plot = area.withTrimmedBottom(14.0f).withTrimmedLeft(24.0f);
    constexpr double kFloor = -72.0;
    const auto xOf = [&](double db) { return plot.getX() + plot.getWidth() * static_cast<float>((juce::jlimit(kFloor, 0.0, db) - kFloor) / -kFloor); };
    const auto yOf = [&](double db) { return plot.getBottom() - plot.getHeight() * static_cast<float>((juce::jlimit(kFloor, 0.0, db) - kFloor) / -kFloor); };
    paintGrid(g, plot, 6, 6);
    for (const double db : {-60.0, -40.0, -20.0, 0.0}) {
      smallText(g, juce::String(juce::roundToInt(db)), {plot.getX() - 24.0f, yOf(db) - 6.0f, 20.0f, 12.0f}, juce::Justification::centredRight);
      if (db < 0.0)
        smallText(g, juce::String(juce::roundToInt(db)), {xOf(db) - 14.0f, plot.getBottom() + 1.0f, 28.0f, 12.0f}, juce::Justification::centred);
    }
    g.setColour(colour::faint);
    g.drawLine(plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);
    // The threshold, dashed.
    const float tx = l.kind == Law::limiter ? xOf(l.ceiling - l.inputGain) : xOf(l.threshold);
    const float dash[] = {3.0f, 3.0f};
    g.setColour(accent.withAlpha(0.45f));
    juce::Path line;
    line.startNewSubPath(tx, plot.getBottom());
    line.lineTo(tx, plot.getY());
    juce::Path dashed;
    juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dash, 2);
    g.fillPath(dashed);

    juce::Path curve, fill;
    for (int i = 0; i <= 80; ++i) {
      const double in = kFloor + (-kFloor) * i / 80.0;
      const juce::Point<float> at(xOf(in), yOf(outOf(l, in)));
      if (i == 0) {
        curve.startNewSubPath(at);
        fill.startNewSubPath(plot.getX(), plot.getBottom());
        fill.lineTo(at);
      } else {
        curve.lineTo(at);
        fill.lineTo(at);
      }
    }
    fill.lineTo(plot.getRight(), plot.getBottom());
    fill.closeSubPath();
    g.setColour(accent.withAlpha(0.16f));
    g.fillPath(fill);
    strokeCurve(g, curve, accent);

    // The live operating point and the gain reduction bar.
    const auto reading = live.readings.find("gainReductionDb");
    const bool hasLive = live.inDb > -70.0f;
    if (hasLive) {
      const double in = live.inDb;
      const double out = reading != live.readings.end() ? in - reading->second : outOf(l, in);
      g.setColour(colour::text);
      g.fillEllipse(Rectangle<float>(8.0f, 8.0f).withCentre({xOf(in), yOf(out)}));
    }
    g.setColour(colour::raised);
    g.fillRoundedRectangle(reduction.withTrimmedTop(14.0f).withTrimmedBottom(14.0f).withSizeKeepingCentre(10.0f, reduction.getHeight() - 28.0f), 3.0f);
    const double gr = reading != live.readings.end() ? reading->second : 0.0;
    const double span = l.kind == Law::gate ? 60.0 : 24.0;
    const auto bar = reduction.withTrimmedTop(14.0f).withTrimmedBottom(14.0f).withSizeKeepingCentre(10.0f, reduction.getHeight() - 28.0f);
    g.setColour(accent);
    g.fillRoundedRectangle(bar.withHeight(bar.getHeight() * static_cast<float>(juce::jlimit(0.0, 1.0, gr / span))), 3.0f);
    smallText(g, "GR", reduction.removeFromTop(12.0f), juce::Justification::centred);
    smallText(g, reading != live.readings.end() ? juce::String(gr, 1) : juce::String("--"), reduction.removeFromBottom(12.0f),
              juce::Justification::centred, colour::text);
  }
};

// ####################
// DELAY
// ####################
class EchoVisual final : public BlockVisual {
public:
  explicit EchoVisual(juce::String kind) : kindId(std::move(kind)) {}

protected:
  juce::String heading() const override { return "Echoes"; }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    const double t = juce::jmax(20.0, timeMs() > 0.0 ? timeMs() : 350.0);
    const bool multi = find({"taps"}) != nullptr;
    const bool pingPong = kindId.contains("ping") || kindId.contains("dual");
    const double fb = juce::jlimit(0.0, 0.95, value({"feedback", "leftFeedback"}, 35.0) / 100.0);
    auto plot = area.withTrimmedBottom(16.0f);
    const float mid = pingPong ? plot.getCentreY() : plot.getBottom();
    const float maxH = pingPong ? plot.getHeight() * 0.46f : plot.getHeight() - 6.0f;
    g.setColour(colour::line);
    g.drawHorizontalLine(juce::roundToInt(mid), plot.getX(), plot.getRight());

    struct Stem {
      double ms, height;
      int side;
    };
    std::vector<Stem> stems{{0.0, 1.0, 0}};
    double span = t * 6.0;
    if (multi) {
      const int taps = juce::roundToInt(value({"taps"}, 6.0));
      const int pattern = choice({"pattern"}, 0);
      for (int i = 1; i <= taps; ++i) {
        const double f = static_cast<double>(i) / taps;
        stems.push_back({t * (pattern == 0 ? f : std::pow(f, 1.0 + 0.5 * pattern)), 1.0 - 0.75 * f * (1.0 - fb * 0.3), i % 2});
      }
      span = t * 1.12;
    } else {
      double h = 1.0;
      int side = 0;
      for (int n = 1; n <= 24 && h > 0.04; ++n) {
        h = std::pow(fb, n);
        side = pingPong ? n % 2 : 0;
        if (h > 0.04 || n == 1)
          stems.push_back({t * n, h, side});
      }
      span = juce::jmax(t * 3.0, stems.back().ms * 1.08);
    }
    for (size_t i = 0; i < stems.size(); ++i) {
      const float x = plot.getX() + 3.0f + (plot.getWidth() - 6.0f) * static_cast<float>(juce::jmin(1.0, stems[i].ms / span));
      const float h = maxH * static_cast<float>(stems[i].height);
      const bool down = stems[i].side == 1 && pingPong;
      const auto stem = juce::Rectangle<float>(x - 2.0f, down ? mid : mid - h, 4.0f, h);
      g.setColour(i == 0 ? colour::dim : (down ? accent.brighter(0.35f) : accent));
      g.fillRoundedRectangle(stem, 2.0f);
    }
    smallText(g, "Dry", {plot.getX(), plot.getBottom() + 2.0f, 30.0f, 12.0f}, juce::Justification::centredLeft);
    smallText(g, juce::String(juce::roundToInt(t)) + " ms" + (multi ? " span" : " apart") + " " + type::dot() + " " +
                     juce::String(multi ? stems.size() - 1 : stems.size() - 1) + (multi ? " taps" : " repeats"),
              {plot.getX() + 30.0f, plot.getBottom() + 2.0f, plot.getWidth() - 30.0f, 12.0f}, juce::Justification::centredRight, colour::text);
  }

private:
  juce::String kindId;
};

// ####################
// REVERB
// ####################
class ReverbVisual final : public BlockVisual {
protected:
  juce::String heading() const override { return "Decay"; }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    const double decay = juce::jmax(0.2, value({"decayS"}, 2.0));
    const double pre = value({"preDelayMs"}, 0.0) / 1000.0;
    const double damping = position({"damping", "tone"}, 0.3);
    const double size = position({"size", "diffusion"}, 0.5);
    auto plot = area.withTrimmedBottom(16.0f);
    const double span = juce::jmax(1.5, pre + decay * 1.15);
    const auto xOf = [&](double s) { return plot.getX() + plot.getWidth() * static_cast<float>(s / span); };
    g.setColour(colour::line);
    g.drawHorizontalLine(juce::roundToInt(plot.getBottom()), plot.getX(), plot.getRight());
    // The tail as many thin reflections under the envelope, then the envelope
    // of the highs (they die sooner the more the tail is damped).
    juce::Random random(7);
    const int lines = juce::roundToInt(plot.getWidth() / 3.0f);
    const double density = 0.55 + 0.4 * size;
    for (int i = 0; i < lines; ++i) {
      const double s = span * i / lines;
      if (s < pre)
        continue;
      const double env = std::pow(10.0, -3.0 * (s - pre) / decay);  // -60 dB at the decay time
      const double h = env * (0.35 + 0.65 * random.nextDouble()) * (random.nextDouble() < density ? 1.0 : 0.4);
      g.setColour(accent.withAlpha(0.5f));
      g.fillRect(xOf(s), plot.getBottom() - plot.getHeight() * static_cast<float>(h), 1.5f, plot.getHeight() * static_cast<float>(h));
    }
    juce::Path low, high;
    for (int i = 0; i <= 60; ++i) {
      const double s = span * i / 60.0;
      const double e = s < pre ? 0.0 : std::pow(10.0, -3.0 * (s - pre) / decay);
      const double eh = s < pre ? 0.0 : std::pow(10.0, -3.0 * (s - pre) / (decay * (1.0 - 0.7 * damping)));
      const juce::Point<float> a(xOf(s), plot.getBottom() - plot.getHeight() * static_cast<float>(e));
      const juce::Point<float> b(xOf(s), plot.getBottom() - plot.getHeight() * static_cast<float>(eh));
      i == 0 ? low.startNewSubPath(a) : low.lineTo(a);
      i == 0 ? high.startNewSubPath(b) : high.lineTo(b);
    }
    strokeCurve(g, low, accent);
    strokeCurve(g, high, colour::muted, 1.25f);
    if (pre > 0.0) {
      g.setColour(colour::dim);
      g.fillRect(xOf(pre) - 0.5f, plot.getY(), 1.0f, plot.getHeight());
    }
    smallText(g, pre > 0.0 ? "Pre " + juce::String(juce::roundToInt(pre * 1000.0)) + " ms" : juce::String("Direct"),
              {plot.getX(), plot.getBottom() + 2.0f, 80.0f, 12.0f}, juce::Justification::centredLeft);
    smallText(g, juce::String(decay, 1) + " s to -60 dB",
              {plot.getRight() - 120.0f, plot.getBottom() + 2.0f, 120.0f, 12.0f}, juce::Justification::centredRight, colour::text);
    smallText(g, "Lows", {plot.getRight() - 90.0f, plot.getY(), 40.0f, 12.0f}, juce::Justification::centredRight, accent);
    smallText(g, "Highs", {plot.getRight() - 46.0f, plot.getY(), 46.0f, 12.0f}, juce::Justification::centredRight, colour::muted);
  }
};

// ####################
// MODULATION
// ####################
class LfoVisual final : public BlockVisual {
protected:
  juce::String heading() const override { return "Modulation"; }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    auto plot = area.withTrimmedBottom(16.0f);
    const float mid = plot.getCentreY();
    g.setColour(colour::line);
    g.drawHorizontalLine(juce::roundToInt(mid), plot.getX(), plot.getRight());
    // A step pattern, when the block has one: its steps as bars.
    std::vector<const KnobParam*> steps;
    for (const auto& p : params)
      if (p.id.startsWithIgnoreCase("step") && p.type == "float")
        steps.push_back(&p);
    if (steps.size() >= 3) {
      const float cell = plot.getWidth() / static_cast<float>(steps.size());
      for (size_t i = 0; i < steps.size(); ++i) {
        const float h = plot.getHeight() * static_cast<float>(juce::jlimit(0.04, 1.0, steps[i]->toPosition(steps[i]->value)));
        g.setColour(accent);
        g.fillRoundedRectangle(plot.getX() + cell * static_cast<float>(i) + 2.0f, plot.getBottom() - h, cell - 4.0f, h, 3.0f);
      }
      smallText(g, juce::String(steps.size()) + " steps", {plot.getX(), plot.getBottom() + 2.0f, 80.0f, 12.0f}, juce::Justification::centredLeft);
      return;
    }
    double hz = rateHz();
    if (hz <= 0.0) {
      if (const KnobParam* rate = find({"rate", "speed", "modRateHz", "wowRateHz"})) {
        hz = rate->type == "choice" ? (rate->value >= 0.5 ? 6.0 : 0.8) : rate->value;
      } else {
        hz = 1.0;
      }
    }
    double depth = 0.6;
    if (const KnobParam* d = find({"depth", "depthPct", "depthCents", "modDepth", "wow"}))
      depth = juce::jlimit(0.05, 1.0, d->toPosition(d->value));
    const int shape = choice({"shape", "wave"}, 0);
    const double cycles = juce::jlimit(1.5, 5.0, 1.5 + 0.8 * std::log2(juce::jmax(0.1, hz) + 0.5));
    juce::Path wave;
    const float amp = plot.getHeight() * 0.46f * static_cast<float>(depth);
    for (int i = 0; i <= 160; ++i) {
      const double phase = cycles * i / 160.0;
      const double frac = phase - std::floor(phase);
      double y = std::sin(2.0 * 3.14159265358979 * phase);
      if (shape == 1)
        y = frac < 0.5 ? 4.0 * frac - 1.0 : 3.0 - 4.0 * frac;
      else if (shape == 2)
        y = frac < 0.5 ? 1.0 : -1.0;
      const juce::Point<float> at(plot.getX() + plot.getWidth() * static_cast<float>(i) / 160.0f, mid - amp * static_cast<float>(y));
      i == 0 ? wave.startNewSubPath(at) : wave.lineTo(at);
    }
    strokeCurve(g, wave, accent);
    smallText(g, juce::String(hz, hz < 10.0 ? 2 : 1) + " Hz", {plot.getX(), plot.getBottom() + 2.0f, 70.0f, 12.0f}, juce::Justification::centredLeft, colour::text);
    smallText(g, "Depth " + juce::String(juce::roundToInt(depth * 100.0)) + "%", {plot.getRight() - 90.0f, plot.getBottom() + 2.0f, 90.0f, 12.0f},
              juce::Justification::centredRight);
  }
};

// ####################
// DRIVE
// ####################
class DriveVisual final : public BlockVisual {
protected:
  juce::String heading() const override { return "Saturation"; }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    auto strip = area.removeFromBottom(34.0f);
    area.removeFromBottom(8.0f);
    double amount = 0.4;
    if (find({"drivePercent", "fuzzPercent", "distortionPercent", "drive", "gain"})) {
      amount = position({"drivePercent", "fuzzPercent", "distortionPercent", "drive", "gain"}, 0.4);
    } else {
      double sum = 0.0;
      int n = 0;
      for (const auto& p : params)
        if (p.type == "float" && !p.common) {
          sum += p.toPosition(p.value);
          ++n;
        }
      amount = n > 0 ? sum / n : 0.4;
    }
    const double k = 1.0 + amount * amount * 28.0;
    const double bias = (position({"biasPercent"}, 0.5) - 0.5) * 0.8;
    const auto shaped = [&](double x) { return (std::tanh(k * (x + bias)) - std::tanh(k * bias)) / std::tanh(k * (1.0 + juce::jmax(0.0, bias))); };
    auto curveArea = area.removeFromLeft(juce::jmin(area.getWidth() * 0.45f, area.getHeight()));
    area.removeFromLeft(10.0f);
    // The curve: in on x, out on y.
    g.setColour(colour::line);
    g.drawHorizontalLine(juce::roundToInt(curveArea.getCentreY()), curveArea.getX(), curveArea.getRight());
    g.drawVerticalLine(juce::roundToInt(curveArea.getCentreX()), curveArea.getY(), curveArea.getBottom());
    g.setColour(colour::faint);
    g.drawLine(curveArea.getX(), curveArea.getBottom(), curveArea.getRight(), curveArea.getY(), 1.0f);
    juce::Path curve;
    for (int i = 0; i <= 80; ++i) {
      const double x = -1.0 + 2.0 * i / 80.0;
      const juce::Point<float> at(curveArea.getCentreX() + curveArea.getWidth() * 0.5f * static_cast<float>(x),
                                  curveArea.getCentreY() - curveArea.getHeight() * 0.5f * static_cast<float>(juce::jlimit(-1.0, 1.0, shaped(x))));
      i == 0 ? curve.startNewSubPath(at) : curve.lineTo(at);
    }
    strokeCurve(g, curve, accent);
    // The wave: a sine in, what the stage makes of it out.
    juce::Path in, out;
    for (int i = 0; i <= 120; ++i) {
      const double s = std::sin(2.0 * 3.14159265358979 * 2.0 * i / 120.0) * 0.9;
      const float x = area.getX() + area.getWidth() * static_cast<float>(i) / 120.0f;
      const float yi = area.getCentreY() - area.getHeight() * 0.5f * static_cast<float>(s);
      const float yo = area.getCentreY() - area.getHeight() * 0.5f * static_cast<float>(juce::jlimit(-1.0, 1.0, shaped(s)));
      i == 0 ? in.startNewSubPath(x, yi) : in.lineTo(x, yi);
      i == 0 ? out.startNewSubPath(x, yo) : out.lineTo(x, yo);
    }
    strokeCurve(g, in, colour::faint, 1.25f);
    strokeCurve(g, out, accent);
    // The tone as a tilt between dark and bright.
    const double tone = position({"tonePercent", "tone", "toneHz", "treble"}, 0.5);
    g.setColour(colour::line);
    g.drawHorizontalLine(juce::roundToInt(strip.getCentreY()), strip.getX(), strip.getRight());
    const float tilt = static_cast<float>(tone - 0.5) * strip.getHeight() * 0.6f;
    g.setColour(accent);
    g.drawLine(strip.getX() + 44.0f, strip.getCentreY() + tilt, strip.getRight() - 44.0f, strip.getCentreY() - tilt, 2.0f);
    smallText(g, "Dark", strip.withWidth(40.0f), juce::Justification::centredLeft);
    smallText(g, "Bright", strip.removeFromRight(40.0f), juce::Justification::centredRight);
  }
};

// An amp's knobs as its own face: a faceplate with a row of knobs, a jewel
// light for the block being on and the sag the engine reports.
class AmpFace final : public BlockVisual {
protected:
  juce::String heading() const override { return "Amp"; }
  bool framed() const override { return false; }

  bool liveMatters(const BlockLive& before, const BlockLive& after) const override {
    const auto sag = [](const BlockLive& l) {
      const auto it = l.readings.find("sag");
      return it != l.readings.end() ? it->second : 0.0;
    };
    return std::abs(sag(before) - sag(after)) > 0.02;
  }

  void draw(juce::Graphics& g, Rectangle<float> box) override {
    // The chassis with its top rail in the block's colour.
    g.setColour(colour::drop);
    g.fillRoundedRectangle(box.translated(0.0f, size::dropDepth), size::panelRadius);
    g.setColour(colour::raised);
    g.fillRoundedRectangle(box, size::panelRadius);
    g.setColour(accent);
    g.fillRoundedRectangle(box.withHeight(5.0f), 2.5f);
    g.setColour(colour::line);
    g.drawRoundedRectangle(box.reduced(0.5f), size::panelRadius, 1.0f);
    auto inner = box.reduced(12.0f, 14.0f);
    auto top = inner.removeFromTop(14.0f);
    paint::label(g, "Amp", top.toNearestInt(), juce::Justification::centredLeft, accent);
    // The jewel light.
    const auto jewel = Rectangle<float>(9.0f, 9.0f).withCentre({top.getRight() - 6.0f, top.getCentreY()});
    g.setColour(enabled ? accent : colour::faint);
    g.fillEllipse(jewel);
    if (enabled) {
      g.setColour(accent.withAlpha(0.25f));
      g.fillEllipse(jewel.expanded(4.0f));
    }
    // The grille along the bottom.
    auto grille = inner.removeFromBottom(26.0f);
    g.setColour(colour::ground);
    g.fillRoundedRectangle(grille, 4.0f);
    g.setColour(colour::line);
    for (float y = grille.getY() + 4.0f; y < grille.getBottom() - 2.0f; y += 4.0f)
      g.drawHorizontalLine(juce::roundToInt(y), grille.getX() + 4.0f, grille.getRight() - 4.0f);
    const auto sag = live.readings.find("sag");
    if (sag != live.readings.end()) {
      g.setColour(accent.withAlpha(0.8f));
      g.fillRoundedRectangle(grille.reduced(4.0f, 8.0f).withWidth((grille.getWidth() - 8.0f) * static_cast<float>(juce::jlimit(0.0, 1.0, sag->second))), 2.0f);
      smallText(g, "Sag", grille.reduced(8.0f, 0.0f), juce::Justification::centredRight, colour::text);
    }
    inner.removeFromBottom(8.0f);
    std::vector<const KnobParam*> knobs;
    for (const auto& p : params)
      if (p.type == "float" && !p.common && p.unit != "dB" && knobs.size() < 8)
        knobs.push_back(&p);
    if (knobs.empty())
      return;
    const int perRow = knobs.size() > 4 ? 4 : static_cast<int>(knobs.size());
    const int rows = (static_cast<int>(knobs.size()) + perRow - 1) / perRow;
    const float cellW = inner.getWidth() / static_cast<float>(perRow), cellH = inner.getHeight() / static_cast<float>(rows);
    for (size_t i = 0; i < knobs.size(); ++i) {
      const auto cell = Rectangle<float>(cellW, cellH).withPosition(inner.getX() + cellW * static_cast<float>(i % perRow),
                                                                  inner.getY() + cellH * static_cast<float>(i / perRow));
      const float radius = juce::jmin(15.0f, cellH * 0.32f);
      const auto centre = juce::Point<float>(cell.getCentreX(), cell.getY() + radius + 4.0f);
      const double pos = knobs[i]->toPosition(knobs[i]->value);
      const float start = juce::MathConstants<float>::pi * 1.25f, sweep = juce::MathConstants<float>::pi * 1.5f;
      juce::Path track;
      track.addCentredArc(centre.x, centre.y, radius + 4.0f, radius + 4.0f, 0.0f, start, start + sweep, true);
      g.setColour(colour::line);
      g.strokePath(track, juce::PathStrokeType(2.0f));
      g.setColour(colour::drop);
      g.fillEllipse(Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre.translated(0.0f, 1.5f)));
      g.setColour(colour::control);
      g.fillEllipse(Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre));
      const float angle = start + sweep * static_cast<float>(pos);
      g.setColour(accent);
      g.drawLine({centre.getPointOnCircumference(radius * 0.35f, angle), centre.getPointOnCircumference(radius * 0.95f, angle)}, 2.5f);
      g.setFont(type::sans(10.0f, type::Weight::semibold));
      g.setColour(colour::muted);
      g.drawText(knobs[i]->name, juce::Rectangle<float>(cellW, 12.0f).withCentre({centre.x, centre.y + radius + 14.0f}).toNearestInt(),
                 juce::Justification::centred, true);
    }
  }
};

// ####################
// PITCH
// ####################
class PitchVisual final : public BlockVisual {
protected:
  juce::String heading() const override { return "Interval"; }

  struct Note {
    double semitones;
    double level;  // 0 to 1
  };

  std::vector<Note> notes() const {
    std::vector<Note> out;
    if (find({"semitones"})) {
      out.push_back({value({"semitones"}, 0.0) + value({"cents"}, 0.0) / 100.0, 1.0});
      return out;
    }
    const auto onLevel = [](const KnobParam& p) { return p.value > p.minValue + 0.5; };
    for (const auto& p : params) {
      if (p.id.startsWith("voice") && p.id.endsWith("Semitones")) {
        const KnobParam* level = nullptr;
        for (const auto& q : params)
          if (q.id == p.id.dropLastCharacters(9) + "Db")
            level = &q;
        if (level == nullptr || onLevel(*level))
          out.push_back({p.value, 1.0});
      }
    }
    if (!out.empty())
      return out;
    for (const auto& [id, st] : std::initializer_list<std::pair<const char*, double>>{{"octDown2Db", -24.0}, {"octDown1Db", -12.0}, {"octUp1Db", 12.0}})
      if (const KnobParam* p = find({id}))
        if (onLevel(*p))
          out.push_back({st, 1.0});
    if (out.empty())
      if (const KnobParam* p = find({"interval"}))
        out.push_back({p->type == "choice" ? p->value : p->value, 1.0});
    return out;
  }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    auto labelRow = area.removeFromBottom(14.0f);
    auto keys = area.removeFromBottom(area.getHeight() * 0.5f);
    constexpr int kKeys = 25;  // two octaves, C to C
    static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    int whites = 0;
    for (int i = 0; i < kKeys; ++i)
      if (!black[i % 12])
        ++whites;
    const float whiteW = keys.getWidth() / static_cast<float>(whites);
    std::vector<float> centreX(kKeys);
    float x = keys.getX();
    for (int i = 0; i < kKeys; ++i) {
      if (!black[i % 12]) {
        centreX[static_cast<size_t>(i)] = x + whiteW * 0.5f;
        x += whiteW;
      } else {
        centreX[static_cast<size_t>(i)] = x;
      }
    }
    const auto notesNow = notes();
    const auto lit = [&](int key) {
      for (const Note& n : notesNow)
        if (juce::roundToInt(n.semitones) + 12 == key)
          return true;
      return false;
    };
    x = keys.getX();
    for (int i = 0; i < kKeys; ++i) {
      if (black[i % 12])
        continue;
      const auto key = Rectangle<float>(x, keys.getY(), whiteW - 1.5f, keys.getHeight());
      g.setColour(i == 12 ? colour::dim : colour::muted.withAlpha(0.8f));
      g.fillRoundedRectangle(key, 2.0f);
      if (lit(i)) {
        g.setColour(accent);
        g.fillRoundedRectangle(key, 2.0f);
      }
      x += whiteW;
    }
    for (int i = 0; i < kKeys; ++i) {
      if (!black[i % 12])
        continue;
      const auto key = Rectangle<float>(whiteW * 0.6f, keys.getHeight() * 0.6f).withCentre({centreX[static_cast<size_t>(i)], keys.getY() + keys.getHeight() * 0.3f});
      g.setColour(lit(i) ? accent.darker(0.3f) : colour::ground);
      g.fillRoundedRectangle(key, 2.0f);
    }
    // The root marker and an arc to each note.
    const float rootX = centreX[12];
    g.setColour(colour::text);
    g.fillEllipse(Rectangle<float>(7.0f, 7.0f).withCentre({rootX, area.getBottom() - 4.0f}));
    for (const Note& n : notesNow) {
      const int key = juce::jlimit(0, kKeys - 1, juce::roundToInt(n.semitones) + 12);
      const float tx = centreX[static_cast<size_t>(key)];
      if (key == 12)
        continue;
      juce::Path arc;
      arc.startNewSubPath(rootX, area.getBottom() - 6.0f);
      arc.quadraticTo((rootX + tx) * 0.5f, area.getY() + 4.0f, tx, area.getBottom() - 6.0f);
      strokeCurve(g, arc, accent, 1.75f);
      g.setColour(accent);
      g.fillEllipse(Rectangle<float>(7.0f, 7.0f).withCentre({tx, area.getBottom() - 4.0f}));
    }
    juce::StringArray labels;
    for (const Note& n : notesNow)
      labels.add((n.semitones > 0.0 ? "+" : "") + juce::String(n.semitones, std::abs(n.semitones - std::round(n.semitones)) < 0.05 ? 0 : 1) + " st");
    smallText(g, labels.isEmpty() ? juce::String("Unison") : labels.joinIntoString("  "), labelRow, juce::Justification::centred, colour::text);
    smallText(g, "Played note", labelRow.withWidth(80.0f), juce::Justification::centredLeft);
  }
};

// ####################
// UTILITY AND THE REST
// ####################
class GlyphVisual final : public BlockVisual {
public:
  explicit GlyphVisual(Category c) : category(c) {}

protected:
  juce::String heading() const override { return "Signal"; }

  bool liveMatters(const BlockLive& before, const BlockLive& after) const override {
    return std::abs(before.inDb - after.inDb) > 1.0f || std::abs(before.outDb - after.outDb) > 1.0f;
  }

  void draw(juce::Graphics& g, Rectangle<float> area) override {
    auto meters = area.removeFromBottom(34.0f);
    const auto box = Rectangle<float>(64.0f, 64.0f).withCentre(area.getCentre());
    g.setColour(colour::line);
    g.fillRect(area.getX(), area.getCentreY() - 1.0f, box.getX() - area.getX(), 2.0f);
    g.fillRect(box.getRight(), area.getCentreY() - 1.0f, area.getRight() - box.getRight(), 2.0f);
    g.setColour(categoryTint(category));
    g.fillRoundedRectangle(box, size::panelRadius);
    g.setColour(accent.withAlpha(0.6f));
    g.drawRoundedRectangle(box.reduced(0.5f), size::panelRadius, 1.5f);
    icons::draw(g, icons::path(category), box.reduced(14.0f), accent);
    smallText(g, "In", Rectangle<float>(area.getX(), meters.getY(), 26.0f, 14.0f), juce::Justification::centredLeft);
    LevelMeter::paintBar(g, Rectangle<float>(area.getX() + 26.0f, meters.getY() + 4.0f, area.getWidth() - 26.0f, 6.0f), live.inDb, -100.0f);
    smallText(g, "Out", Rectangle<float>(area.getX(), meters.getY() + 16.0f, 26.0f, 14.0f), juce::Justification::centredLeft);
    LevelMeter::paintBar(g, Rectangle<float>(area.getX() + 26.0f, meters.getY() + 20.0f, area.getWidth() - 26.0f, 6.0f), live.outDb, -100.0f);
  }

private:
  Category category;
};

}  // namespace

std::unique_ptr<BlockVisual> makeBlockVisual(Category category, const juce::String& kindId) {
  switch (category) {
    case Category::eq:
      if (kindId == "eq.graphic10")
        return nullptr;
      return std::make_unique<EqVisual>(false);
    case Category::filter: return std::make_unique<EqVisual>(true);
    case Category::dynamics: return std::make_unique<TransferVisual>();
    case Category::delay: return std::make_unique<EchoVisual>(kindId);
    case Category::reverb: return std::make_unique<ReverbVisual>();
    case Category::modulation: return std::make_unique<LfoVisual>();
    case Category::drive: return std::make_unique<DriveVisual>();
    case Category::amp: return std::make_unique<AmpFace>();
    case Category::pitch: return std::make_unique<PitchVisual>();
    default: return std::make_unique<GlyphVisual>(category);
  }
}

}  // namespace pedaleira::ui
