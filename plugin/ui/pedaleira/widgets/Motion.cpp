#include "Motion.h"

#include <atomic>

namespace pedaleira::ui {

namespace motion {
namespace {
std::atomic<bool> on{true};
}  // namespace

void setEnabled(bool enabled) { on.store(enabled); }
bool enabled() { return on.load(); }

void fadeIn(juce::Component& component, int durationMs) {
  if (!enabled()) {
    component.setAlpha(1.0f);
    return;
  }
  component.setAlpha(0.0f);
  juce::Desktop::getInstance().getAnimator().animateComponent(&component, component.getBounds(), 1.0f, durationMs,
                                                              false, 0.0, 1.0);
}
}  // namespace motion

Tween::Tween(juce::Component& ownerComponent, float start, int durationMs)
    : owner(ownerComponent), current(start), from(start), goal(start), duration(durationMs) {}

void Tween::setTarget(float target) {
  if (target == goal && (isTimerRunning() || current == target))
    return;
  goal = target;
  if (!motion::enabled() || duration <= 0) {
    snapTo(target);
    return;
  }
  from = current;
  startMs = juce::Time::getMillisecondCounterHiRes();
  if (!isTimerRunning())
    startTimerHz(60);
}

void Tween::snapTo(float value) {
  stopTimer();
  current = from = goal = value;
  if (onStep)
    onStep();
  else
    owner.repaint();
}

void Tween::timerCallback() {
  const double elapsed = juce::Time::getMillisecondCounterHiRes() - startMs;
  const float t = static_cast<float>(elapsed / duration);
  if (t >= 1.0f) {
    current = goal;
    stopTimer();
  } else {
    current = from + (goal - from) * motion::easeOut(t);
  }
  if (onStep)
    onStep();
  else
    owner.repaint();
}

}  // namespace pedaleira::ui
