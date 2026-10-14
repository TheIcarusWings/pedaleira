#pragma once
// cab/CabResponse.h: the response of a mic'd guitar cab, from physics.
//
// cabFrequencyResponse evaluates the model at any frequency; cabImpulseResponse
// turns it into the minimum-phase impulse response the cab.model block plays
// through a convolver. The model, term by term (docs/cab-framework.md has the
// equations and the references):
//
//   speaker   a lumped electro-mechanical model from Thiele-Small parameters:
//             the cone's acceleration is a second-order high-pass at the
//             resonance, which a closed box moves up and tightens (its air is
//             a second spring) and an open back leaves at the free-air value;
//             the voice coil's lossy inductance; the cone's breakup
//             resonances and the roll-off above them (SpeakerDescription).
//   geometry  every speaker of the cab as a source on the baffle, heard at the
//             mic's position with its own distance, delay and directivity
//             (a piston whose radiating radius is a fraction of the cone);
//             the diffraction at the baffle edges; with an open back the
//             speakers' rear waves (inverted) around the box and off the wall
//             behind; the floor as an image source.
//   mic       dynamic (cardioid, presence peak), ribbon (figure of eight, dark
//             top), condenser (cardioid, flat); the proximity effect of the
//             pressure-gradient part; the capsule's high-frequency loss off
//             axis.
//   room      short early reflections off the walls and the ceiling of a small
//             room, as image sources.
//   filters   a low cut and a high cut, both second-order Butterworth.
//
// Plain C++ (std only), no allocation in cabFrequencyResponse beyond its
// result; cabImpulseResponse allocates its FFT buffers. Neither is for the
// audio thread.
#include <complex>
#include <vector>

#include "cab/SpeakerDescription.h"

namespace cab {

enum class CabSize { OneByTwelve, TwoByTwelve, FourByTwelve, OneByTen, TwoByTen };
enum class CabBack { Closed, Open };
enum class MicType { Dynamic, Ribbon, Condenser };

constexpr int kCabSizes = 5;
constexpr int kMicTypes = 3;

/** The settings of one cab: everything but the speaker. */
struct CabSettings {
  CabSize size{CabSize::FourByTwelve};
  CabBack back{CabBack::Closed};
  MicType mic{MicType::Dynamic};
  /** Where the mic points on the cone: 0 at the centre (dust cap), 1 at the
      edge of the cone. */
  double position{0.35};
  /** From the grille cloth to the mic capsule, in centimetres. */
  double distanceCm{2.5};
  /** The mic's tilt off the speaker's axis, in degrees. */
  double angleDeg{0.0};
  /** The room's early reflections, 0 to 1. */
  double room{0.1};
  /** Low and high cut corners (second-order Butterworth each). */
  double lowCutHz{30.0};
  double highCutHz{16000.0};
};

/** One cabinet: the baffle, the box and where its speakers sit. Metres. */
struct CabGeometry {
  const char* label;
  int speakers;
  /** Nominal speaker diameter in inches (10 or 12). */
  double speakerInches;
  double width;
  double height;
  double depth;
  /** Net internal volume per speaker (closed back), litres. */
  double litresPerSpeaker;
  /** Speaker centres on the baffle, from the baffle's centre (x right, y up).
      The mic is on speaker 0 and moves towards the box's centre (or the right
      edge on a single speaker) as the position goes to the edge. */
  double x[4];
  double y[4];
};

/** The geometry of `size`. */
const CabGeometry& cabGeometry(CabSize size) noexcept;
/** "1x12", "2x12", ... */
const char* cabSizeName(CabSize size) noexcept;
const char* micTypeName(MicType mic) noexcept;

/** The cab's complex response at each frequency in `frequenciesHz`, before the
    level normalisation of cabImpulseResponse (the absolute level is
    arbitrary; ratios between frequencies are the model's). */
std::vector<std::complex<double>> cabFrequencyResponse(const SpeakerDescription& speaker,
                                                       const CabSettings& settings,
                                                       const std::vector<double>& frequenciesHz);

/** Length of the impulse response cabImpulseResponse makes at `sampleRate`:
    about 85 ms (4096 samples at 48 kHz), a power of two. */
int cabImpulseLength(double sampleRate) noexcept;

/** The minimum-phase impulse response of the cab at `sampleRate`, of
    cabImpulseLength(sampleRate) samples, faded out over its last quarter.
    Its level is normalised so that the band from 150 Hz to 3.5 kHz averages
    0 dB (power, on a log frequency axis): turning a control changes the
    tone, not the loudness. */
std::vector<float> cabImpulseResponse(const SpeakerDescription& speaker, const CabSettings& settings,
                                      double sampleRate);

}  // namespace cab
