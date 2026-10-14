// What makes one block look different from another in its settings: the
// section a parameter belongs to (keyed by category, with a keyword fallback
// so a new kind is still grouped), and the picture at the top of the panel,
// one family per category: a response curve for equalisers, the transfer curve
// for dynamics, the echoes of a delay, the decay of a reverb, the wave of a
// modulation, the saturation of a drive, the interval
// of a pitch block and a level glyph for the rest. The pictures are drawn
// from the parameters (and, where the engine reports them, live levels and
// readings) and repaint only when something they show has moved.
#pragma once

#include "../Tokens.h"
#include "ParamKnobs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <vector>

namespace pedaleira::ui {

// The section a parameter is shown in, for a block of this category.
juce::String paramGroup(Category category, const KnobParam& param);
// Where a section sits among the others (lower first); unknown ones go last
// but before the output.
int groupRank(const juce::String& group);

// What the engine reports for a block while it runs (meters.get, blocks.<id>).
struct BlockLive {
  float inDb = -100.0f, outDb = -100.0f;
  std::map<juce::String, double> readings;
  static BlockLive fromVar(const juce::var& blockMeters);
};

class BlockVisual : public juce::Component {
public:
  void setAccent(juce::Colour colour);
  // The parameters as block.listParams reports them.
  void setParams(const std::vector<KnobParam>& newParams, bool blockEnabled);
  void setLive(const BlockLive& newLive);
  void setTempo(double bpm);

  void paint(juce::Graphics& g) override;

protected:
  virtual juce::String heading() const = 0;
  // Draws into the plot area, inside the frame.
  virtual void draw(juce::Graphics& g, juce::Rectangle<float> area) = 0;
  // Whether a live change is worth a repaint.
  virtual bool liveMatters(const BlockLive& before, const BlockLive& after) const;
  // Whether the picture has the frame and heading (a picture that draws its
  // own turns this off).
  virtual bool framed() const { return true; }

  const KnobParam* find(std::initializer_list<const char*> ids) const;
  double value(std::initializer_list<const char*> ids, double fallback) const;
  // 0 to 1 along the parameter's range.
  double position(std::initializer_list<const char*> ids, double fallback) const;
  bool flag(std::initializer_list<const char*> ids, bool fallback) const;
  int choice(std::initializer_list<const char*> ids, int fallback) const;
  // The tempo-synced time or rate of the block in ms (sync, division, timeMs
  // or rateHz); 0 when it has none.
  double timeMs() const;
  double rateHz() const;

  juce::Colour accent = colour::iris;
  bool enabled = true;
  std::vector<KnobParam> params;
  BlockLive live;
  double bpm = 120.0;
};

// The picture for a category; nothing for the graphic equaliser, whose band
// sliders are the picture, and nothing for an amp, which is drawn as its
// faceplate (AmpFaceplate.h).
std::unique_ptr<BlockVisual> makeBlockVisual(Category category, const juce::String& kindId);

// Exposed for the testbed's checks.
namespace blockmath {
// The response in dB of a peaking filter at hz (RBJ), sample rate 48 kHz.
double peakDb(double hz, double centre, double gainDb, double q);
double lowShelfDb(double hz, double centre, double gainDb);
double highShelfDb(double hz, double centre, double gainDb);
// A Butterworth cut of this order, in dB.
double highPassDb(double hz, double corner, int order);
double lowPassDb(double hz, double corner, int order);
// A compressor's output level for an input level (dB), soft knee.
double compressorOut(double inDb, double thresholdDb, double ratio, double kneeDb);
}  // namespace blockmath

}  // namespace pedaleira::ui
