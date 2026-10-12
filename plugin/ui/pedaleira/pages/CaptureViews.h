// The seven steps of the capture wizard (boards "Capture 1, source" to
// "Capture 7, review"). Each one reads the session the page hands it, draws
// its board and acts only through the capture.* commands (docs/commands.md);
// the page decides which one is in front and draws the stepper and footer.
#pragma once

#include "CaptureParts.h"

#include "../widgets/Controls.h"
#include "../widgets/Knob.h"

#include <array>

namespace pedaleira::ui {

// ####################
// 1. SOURCE
// ####################
class CaptureSourceStep : public CaptureStep {
public:
  explicit CaptureSourceStep(CaptureHost& hostPage);

  void show(const juce::var& newSession) override;
  Footer footer() const override;
  void primary() override;

  // Testbed and tests.
  void chooseSource(int index);
  void setName(const juce::String& text) { name.setText(text, true); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  juce::Rectangle<int> detailsBounds() const;

  std::array<std::unique_ptr<ChoiceCard>, 4> cards;
  int sourceIndex = 1;
  juce::TextEditor name;
  SegmentedControl gainTag{{"Clean", "Crunch", "High gain"}};
  SegmentedControl modelSize{{"Full", "Lite"}};
};

// ####################
// 2. WIRING
// ####################
class CaptureWiringStep : public CaptureStep {
public:
  explicit CaptureWiringStep(CaptureHost& hostPage);

  void show(const juce::var& newSession) override;
  Footer footer() const override;
  void primary() override;

  // Testbed and tests: the variant of the segmented control (0 box, 1 no box, 2 dry/wet).
  void chooseVariant(int index);
  capture::Wiring wiring() const { return current; }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout;
  Layout layout() const;
  void sendChannels(const juce::var& args);
  void rebuildChecklist();
  juce::String channelName(bool output, int index) const;
  juce::String subtitle() const;

  capture::Wiring current = capture::Wiring::reampBox;
  // The wizard's own choice for a re-amp: the session has only reamp and dryWet.
  bool hasBox = true;
  int deviceInputs = 2;
  int deviceOutputs = 2;
  bool channelsKnown = false;
  juce::StringArray warnings;

  SegmentedControl variant{{"I have one", "I don't have one", "Play it in"}};
  Checklist checklist;
  SelectBox firstChannel{"Send output"};
  SelectBox secondChannel{"Return input"};
  juce::TextEditor maxLevel;
  PushButton testClick{"Play test click"};
  PushButton measureGuitar{"Measure my guitar"};
};

// ####################
// 3. LEVELS
// ####################
class CaptureLevelsStep : public CaptureStep {
public:
  explicit CaptureLevelsStep(CaptureHost& hostPage);
  ~CaptureLevelsStep() override;

  void show(const juce::var& newSession) override;
  void tick(int count) override;
  void leave() override;
  Footer footer() const override;
  void primary() override;

  // Testbed and tests: reads the levels now; starts or stops the tone.
  void poll();
  void setTone(bool on);

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout;
  Layout layout() const;
  void apply(const juce::var& status);
  void measureNoise();
  void resetHold();
  void chooseTarget();
  void chooseLength();
  void flushSend();

  bool dryWet = false;
  bool toneOn = false;
  bool pollFailed = false;
  capture::LevelState state = capture::LevelState::silent;
  bool inRange = false;
  bool measured = false;
  double peakDb = -120.0, holdDb = -120.0, wetPeakDb = -120.0, wetHoldDb = -120.0;
  int clipped = 0;
  double targetDb = -12.0, lowDb = -15.0, highDb = -9.0, sendDb = -24.0;
  juce::String noiseState = "none";
  double noiseDb = -120.0;
  double plannedSeconds = 180.0;
  juce::String calibrationText, channelText;
  // The send level the knob asked for, sent on the next tick.
  bool sendPending = false;
  double pendingSendDb = 0.0;

  Knob sendKnob{"Send level"};
  Toggle toneSwitch{"Test tone"};
  PushButton resetButton{"Reset peak hold"};
  PushButton targetButton{"Target", PushButton::Kind::ghost};
  PushButton noiseButton{"Measure", PushButton::Kind::ghost};
  SelectBox lengthBox{"Recording length"};
};

// ####################
// 4. RECORD
// ####################
class CaptureRecordStep : public CaptureStep {
public:
  explicit CaptureRecordStep(CaptureHost& hostPage);
  ~CaptureRecordStep() override;

  void show(const juce::var& newSession) override;
  void tick(int count) override;
  void leave() override;
  Footer footer() const override;
  void primary() override;
  void secondary() override;
  void back() override;

  // Testbed and tests: reads the recording now.
  void poll();

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout;
  Layout layout() const;
  void apply(const juce::var& status);
  void start();
  void discard();
  bool recording() const { return state == "recording" || state == "writing"; }
  bool needsInputFile() const { return !dryWet && !recording() && state != "complete"; }

  bool dryWet = false;
  juce::String state = "none";
  double progress = 0.0, recorded = 0.0, total = 0.0, left = 0.0;
  double peakDb = -120.0, wetPeakDb = -120.0;
  bool hasPeak = false;
  bool checking = false;
  int clipped = 0;
  bool hasLatency = false;
  int latencySamples = 0;
  double latencyMs = 0.0;
  juce::String error, channelText;
  double plannedSeconds = 180.0;

  InputFilePanel inputFile;
};

// ####################
// 5. CHECK
// ####################
class CaptureCheckStep : public CaptureStep {
public:
  explicit CaptureCheckStep(CaptureHost& hostPage);

  void show(const juce::var& newSession) override;
  void trainerChanged() override;
  Footer footer() const override;
  void primary() override;
  void secondary() override;
  void back() override;

  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout;
  Layout layout() const;
  void runChecks();
  void recordAgain();
  bool hasReport() const { return session["checks"].isObject(); }
  juce::String verdict() const { return session["checks"]["verdict"].toString(); }
  bool canTrain() const;

  int scroll = 0;
  bool checking = false;
  PushButton again{"Record again"};
  PushButton listen{"Listen to return"};
  CheckBox trainAnyway{"Train anyway, at your own risk"};
  TrainerPanel trainerPanel;
};

// ####################
// 6. TRAIN
// ####################
class CaptureTrainStep : public CaptureStep {
public:
  explicit CaptureTrainStep(CaptureHost& hostPage);
  ~CaptureTrainStep() override;

  void show(const juce::var& newSession) override;
  void tick(int count) override;
  void trainerChanged() override;
  Footer footer() const override;
  void primary() override;
  void secondary() override;

  // Testbed and tests: reads the training status now.
  void poll();

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout;
  Layout layout() const;
  void apply(const juce::var& status);
  void start();
  bool running() const { return state == "running" || state == "checking"; }

  juce::String state = "none";
  int epoch = 0, epochs = 100;
  double progress = 0.0, secondsLeft = -1.0, elapsed = 0.0;
  bool hasEsr = false;
  double esr = 0.0;
  juce::String esrBandLabel, esrBand, error, message;
  juce::StringArray log;
  // (epoch, ESR) as seen while the page was polling.
  std::vector<std::pair<int, double>> history;
  juce::String historyFor;

  PushButton minimise{"Minimise to background"};
  TrainerPanel trainerPanel;
  InputFilePanel inputFile;
};

// ####################
// 7. REVIEW
// ####################
class CaptureReviewStep : public CaptureStep {
public:
  explicit CaptureReviewStep(CaptureHost& hostPage);
  ~CaptureReviewStep() override;

  void show(const juce::var& newSession) override;
  Footer footer() const override;
  void primary() override;
  void secondary() override;
  void back() override {}

  // Testbed and tests.
  void addToRig();
  bool saved() const { return !savedFile.isEmpty(); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  struct Layout;
  class AbButton;
  Layout layout() const;
  void save();
  void applyMetadata();
  void trainAgain();
  void rebuildTypes();

  juce::var review;
  juce::String savedFile, rigNote;
  bool addedToRig = false;
  bool updating = false;

  juce::TextEditor nameEdit;
  juce::TextEditor notesEdit;
  std::unique_ptr<SegmentedControl> typeControl;
  SegmentedControl gainControl{{"Clean", "Crunch", "High gain"}};
  CheckBox addOnSave{"Add to the current preset"};
  std::unique_ptr<AbButton> realButton, captureButton;
  Toggle blindSwitch{"Blind test"};
};

}  // namespace pedaleira::ui
