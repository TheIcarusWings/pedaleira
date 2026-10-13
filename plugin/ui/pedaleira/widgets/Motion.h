// Motion: how long things take to change, and a value that eases toward a
// target while its owner repaints. Hover and press answer at once (about a
// tenth of a second), selection and sliding parts a little slower, panels
// slower still. Everything eases out: fast at first, settling at the end.
// Motion can be switched off (the testbed does, so its pictures never catch
// a value halfway).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace pedaleira::ui {

namespace motion {
inline constexpr int hoverMs = 120;
inline constexpr int pressMs = 80;
inline constexpr int selectMs = 180;
inline constexpr int panelMs = 220;

// Off: every Tween jumps to its target.
void setEnabled(bool enabled);
bool enabled();

// Ease-out cubic, 0 to 1.
inline float easeOut(float t) {
  const float inverse = 1.0f - juce::jlimit(0.0f, 1.0f, t);
  return 1.0f - inverse * inverse * inverse;
}

// Fades a component in (alpha 0 to 1), or shows it at once with motion off.
void fadeIn(juce::Component& component, int durationMs = panelMs);
}  // namespace motion

// A float that moves to its target over a duration, eased out, repainting
// its owner (or calling onStep) on every frame while it moves. Runs a timer
// only while it moves.
class Tween : private juce::Timer {
public:
  explicit Tween(juce::Component& owner, float start = 0.0f, int durationMs = motion::hoverMs);

  void setTarget(float target);
  // Straight to a value, no motion.
  void snapTo(float value);
  void setDuration(int durationMs) { duration = durationMs; }
  float value() const { return current; }
  float target() const { return goal; }
  bool moving() const { return isTimerRunning(); }

  // Called on each frame instead of repainting the whole owner.
  std::function<void()> onStep;

private:
  void timerCallback() override;

  juce::Component& owner;
  float current, from, goal;
  int duration;
  double startMs = 0.0;
};

}  // namespace pedaleira::ui
