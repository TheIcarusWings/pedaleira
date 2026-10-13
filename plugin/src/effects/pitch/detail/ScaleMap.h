#pragma once
// Private to the pitch family: the scales of pitch.harmony and the arithmetic
// that turns "a third above" into a number of semitones for the note that is
// being played.
//
// A scale is a list of semitone offsets from its key. A voice is a number of
// scale STEPS (degrees) away from the played note: +2 is the next-but-one note
// of the scale (a third in a seven-note scale, a fifth in a pentatonic one),
// -7 is an octave down in a seven-note scale. The result is the interval in
// semitones, which depends on where in the scale the played note is.
//
// A played note that is not in the scale is treated as the scale note just
// below it and keeps its distance from it, so the voice stays the same
// distance from what was played instead of snapping.
#include <juce_core/juce_core.h>

#include <array>

namespace pitchfx {

struct ScaleDef {
  const char* name;
  int size;
  int offsets[12];
};

inline constexpr int kNumScales = 10;

inline constexpr std::array<ScaleDef, kNumScales> kScales = {{
    {"Major", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"Natural Minor", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"Harmonic Minor", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"Melodic Minor", 7, {0, 2, 3, 5, 7, 9, 11}},
    {"Dorian", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"Mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"Pentatonic Major", 5, {0, 2, 4, 7, 9}},
    {"Pentatonic Minor", 5, {0, 3, 5, 7, 10}},
    {"Blues", 6, {0, 3, 5, 6, 7, 10}},
    {"Chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
}};

inline juce::StringArray scaleNames() {
  juce::StringArray names;
  for (const ScaleDef& scale : kScales)
    names.add(scale.name);
  return names;
}

inline juce::StringArray keyNames() {
  return {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}

constexpr int floorDivide(int a, int b) noexcept {
  const int q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/** Semitones above the key of the scale note `index` steps from the key (any
    integer: `size` is the key an octave up, -1 the last note below it). */
constexpr int scaleOffset(const ScaleDef& scale, int index) noexcept {
  const int octave = floorDivide(index, scale.size);
  return octave * 12 + scale.offsets[index - octave * scale.size];
}

/** The scale step the MIDI `note` sits on, or just above: the largest index
    whose offset is at or below the note's. */
constexpr int stepOfNote(const ScaleDef& scale, int note, int key) noexcept {
  const int relative = note - key;
  const int octave = floorDivide(relative, 12);
  const int pitchClass = relative - octave * 12;
  int degree = 0;
  for (int d = scale.size - 1; d > 0; --d)
    if (scale.offsets[d] <= pitchClass) {
      degree = d;
      break;
    }
  return octave * scale.size + degree;
}

/** True when the MIDI `note` is a note of the scale. */
constexpr bool noteInScale(const ScaleDef& scale, int note, int key) noexcept {
  const int step = stepOfNote(scale, note, key);
  return scaleOffset(scale, step) == note - key;
}

/** The interval in semitones from the played MIDI `note` to the scale note
    `steps` scale steps away from it, in the scale `scaleIndex` of the key
    `key` (0 = C ... 11 = B). */
constexpr int harmonyInterval(int note, int key, int scaleIndex, int steps) noexcept {
  const ScaleDef& scale = kScales[static_cast<size_t>(scaleIndex)];
  const int index = stepOfNote(scale, note, key);
  return scaleOffset(scale, index + steps) - scaleOffset(scale, index);
}

// Compile-time self-checks of the arithmetic.
static_assert(harmonyInterval(60, 0, 0, 2) == 4, "a third above C in C major is E");
static_assert(harmonyInterval(62, 0, 0, 2) == 3, "a third above D in C major is F");
static_assert(harmonyInterval(67, 0, 0, 4) == 7, "a fifth above G in C major is D");
static_assert(harmonyInterval(71, 0, 0, 4) == 6, "a fifth above B in C major is F");
static_assert(harmonyInterval(60, 0, 0, -2) == -3, "a third below C in C major is A");
static_assert(harmonyInterval(69, 9, 1, 2) == 3, "a third above A in A minor is C");
static_assert(harmonyInterval(48, 0, 0, 7) == 12, "seven steps are an octave");
static_assert(harmonyInterval(60, 0, 6, 2) == 4, "two steps above C in C pentatonic major is E");
static_assert(harmonyInterval(60, 0, 6, 3) == 7, "three steps above C in C pentatonic major is G");
static_assert(harmonyInterval(65, 5, 0, 2) == 4, "a third above F in F major is A");

}  // namespace pitchfx
