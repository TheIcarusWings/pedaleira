// The capture wizard (boards "Capture 1, source" to "Capture 7, review"):
// record a device, train a NAM A2 model of it, review it and save it. Seven
// steps with a stepper above and a footer below; each step is a CaptureStep
// and everything it does is a capture.* command (docs/capture.md), so the
// session lives in the engine, survives a restart and is found again here:
// opening the page resumes the capture that is in progress, at its step.
//
// The header's Save stays a secondary button on this page (the shell does
// that), so each step has one primary button, in the footer.
#pragma once

#include "../widgets/Dialog.h"
#include "CaptureParts.h"
#include "CaptureViews.h"

#include <array>

namespace pedaleira::ui {

class CapturePage : public juce::Component, private CaptureHost, private juce::Timer {
public:
  explicit CapturePage(EngineClient& engine);
  ~CapturePage() override;

  // The page is shown: finds the capture in progress, if any, and opens it.
  void refresh();

  // A capture was added to the rig or saved to the library.
  std::function<void()> onChanged;

  // Testbed and tests.
  capture::Step currentStep() const { return shown; }
  juce::String sessionId() const { return currentId; }
  const juce::var& currentSession() const { return session; }
  CaptureSourceStep& source() { return sourceStep; }
  CaptureWiringStep& wiring() { return wiringStep; }
  CaptureLevelsStep& levels() { return levelsStep; }
  CaptureRecordStep& record() { return recordStep; }
  CaptureCheckStep& check() { return checkStep; }
  CaptureTrainStep& train() { return trainStep; }
  CaptureReviewStep& review() { return reviewStep; }
  Dialog& dialog() { return prompt; }
  // Presses the footer's buttons; the trainer check runs now, not later.
  void pressPrimary();
  void pressSecondary();
  void pressBack();
  void checkTrainerNow();
  // Opens the capture with this id at its step.
  void open(const juce::String& id);

  void paint(juce::Graphics& g) override;
  void resized() override;
  void visibilityChanged() override;

private:
  // CaptureHost.
  EngineClient& engine() override { return client; }
  EngineReply run(const juce::String& command, const juce::var& args, const juce::String& failureTitle) override;
  void adopt(const juce::var& newSession) override;
  bool moveTo(capture::Step step, const juce::var& extra) override;
  void sessionChanged(const juce::var& newSession) override;
  void reload() override;
  void footerChanged() override { layoutFooter(); repaint(); }
  void rigChanged() override {
    if (onChanged)
      onChanged();
  }
  void notice(const juce::String& title, const juce::String& message) override;
  void confirm(const juce::String& title, const juce::String& message, const juce::String& button,
               std::function<void()> yes) override;
  void later(std::function<void()> work) override;
  void goBack() override;
  void cancelCapture() override;
  const TrainerInfo& trainer() override { return trainerInfo; }
  void checkTrainer() override;

  void timerCallback() override;
  CaptureStep& view(capture::Step step);
  void present();
  void reset();
  void readTrainer();
  void layoutFooter();
  juce::Rectangle<int> stepperBounds() const;
  juce::Rectangle<int> footerBounds() const;
  juce::Rectangle<int> contentBounds() const;

  EngineClient& client;
  juce::String currentId;
  juce::var session;
  capture::Step shown = capture::Step::source;
  TrainerInfo trainerInfo;
  int ticks = 0;

  CaptureSourceStep sourceStep{*this};
  CaptureWiringStep wiringStep{*this};
  CaptureLevelsStep levelsStep{*this};
  CaptureRecordStep recordStep{*this};
  CaptureCheckStep checkStep{*this};
  CaptureTrainStep trainStep{*this};
  CaptureReviewStep reviewStep{*this};

  PushButton cancel{"Cancel capture", PushButton::Kind::ghost};
  PushButton backButton{"Back"};
  PushButton secondaryButton{"Secondary"};
  PushButton primaryButton{"Primary", PushButton::Kind::primary};
  Dialog prompt;
};

}  // namespace pedaleira::ui
