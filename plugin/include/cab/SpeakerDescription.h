#pragma once
// cab/SpeakerDescription.h: a guitar speaker, described as data.
//
// A SpeakerDescription is what the cab model (CabResponse.h) needs to know
// about one speaker type: its size, its Thiele-Small parameters (free-air
// resonance, total Q, the air volume of equal compliance), its voice coil, the
// resonances of its cone in breakup and the roll-off above them. Everything
// else (the box, the number of speakers, the mic, the room) is a setting of
// the cab.model block.
//
// It is read from a JSON file by parseSpeakerDescription
// (SpeakerDescription.cpp); the format is documented in docs/cab-framework.md.
// This header is plain C++ and std, with no JSON and no JUCE, so the structure
// can be built by hand too.
//
// Units: Hz, litres, ohms, millihenries, inches, dB.
#include <string>
#include <vector>

namespace cab {

/** The format version this build reads. */
constexpr int kSpeakerDescriptionVersion = 1;

/** At most this many cone resonances per speaker. */
constexpr int kMaxBreakupResonances = 16;

/** One resonance of the cone: a peak (gain above 0) or a dip (below 0), with
    the quality factor of a second-order resonator. */
struct ConeResonance {
  double frequencyHz{2000.0};
  double gainDb{0.0};
  double q{2.0};
};

struct SpeakerDescription {
  // ---- Metadata -----------------------------------------------------------
  int version{kSpeakerDescriptionVersion};
  std::string name;
  std::string author;
  /** The licence of this description file (an SPDX identifier or a name). */
  std::string licence;
  std::string summary;

  /** Nominal diameter the values below describe (10 or 12 inches, or any size
      from 6 to 15). In a cab of the other size the cone is scaled. */
  double diameterInches{12.0};

  // ---- Thiele-Small ---------------------------------------------------------
  /** Free-air resonance fs. */
  double resonanceHz{80.0};
  /** Total Q at fs (Qts): electrical and mechanical damping together. */
  double qTotal{0.8};
  /** Vas: the volume of air with the same compliance as the suspension. */
  double complianceLitres{60.0};

  // ---- Voice coil -----------------------------------------------------------
  /** DC resistance Re. */
  double coilResistanceOhm{6.4};
  /** The coil's inductance, as the inductance that has the coil's reactance at
      1 kHz. 0 for a coil whose inductance is not modelled. */
  double coilInductanceMh{0.0};
  /** A real coil in an iron gap is lossy: its impedance rises as f^n with n
      under 1 (0.5 to 0.8), not as f. */
  double coilLossExponent{0.65};

  // ---- Cone -----------------------------------------------------------------
  /** The part of the cone's radius that still moves as one at the top of the
      range (sets how the response narrows off axis and towards the edge). */
  double radiatingFraction{0.5};
  /** The resonances of the cone in breakup, peaks and dips. */
  std::vector<ConeResonance> breakup;
  /** Above the breakup the cone stops radiating: a roll-off with this corner
      and order (6 dB per octave per order: 4 is 24 dB per octave). */
  double rolloffHz{5500.0};
  int rolloffOrder{4};
};

/** Reads a speaker description from JSON text. On failure returns false and
    `error` says what is wrong and where (a path such as "cone.breakup[2].q").
    Not for the audio thread. */
bool parseSpeakerDescription(const std::string& text, SpeakerDescription& out, std::string& error);

}  // namespace cab
