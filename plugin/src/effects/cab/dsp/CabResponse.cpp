// The cab model: frequency response from physics, and its minimum-phase
// impulse response. See cab/CabResponse.h for the outline and
// docs/cab-framework.md for the equations and where they come from.
#include "cab/CabResponse.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace cab {

namespace {

using Complex = std::complex<double>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kSpeedOfSound = 343.0;
constexpr double kMetresPerInch = 0.0254;
// The grille cloth sits this far in front of the baffle.
constexpr double kGrilleMetres = 0.012;
// The dust cap sits this far behind the baffle, per inch of diameter (4.5 cm
// on a 12 inch cone).
constexpr double kConeDepthPerInch = 0.00375;
// The radiating piston is this much of the nominal radius (the surround and
// the frame do not move air).
constexpr double kPistonFraction = 0.85;
// The cab stands on the floor on short feet.
constexpr double kFeetMetres = 0.03;
// An open back faces a wall this far behind it.
constexpr double kWallBehindMetres = 0.3;
// Reflection coefficients: the floor, and the wall behind an open back.
constexpr double kFloorReflection = 0.7;
constexpr double kBackWallReflection = 0.6;
// The room around the cab: side walls either side of the cab's centre, the
// ceiling above the floor, the wall the cab faces in front of its baffle.
constexpr double kRoomSideMetres = 2.1;
constexpr double kRoomCeilingMetres = 2.6;
constexpr double kRoomFrontMetres = 4.2;
// The room is heard as if the mic were at least this far away (see
// docs/cab-framework.md, "Room").
constexpr double kRoomPickupMetres = 1.0;
// Wall and air absorption: each bounce is a one-pole low-pass at this corner.
constexpr double kRoomAbsorptionHz = 5000.0;
// The rear wave of an open back loses its top end on the way round.
constexpr double kRearWaveCornerHz = 800.0;
// A mic within this part of the radiating radius from a cone's centre is in
// its beam (see geometryResponse).
constexpr double kBeamFraction = 0.5;

const CabGeometry kGeometries[kCabSizes] = {
    {"1x12", 1, 12.0, 0.50, 0.45, 0.25, 40.0, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
    {"2x12", 2, 12.0, 0.74, 0.48, 0.27, 33.0, {-0.17, 0.17, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
    {"4x12", 4, 12.0, 0.76, 0.76, 0.36, 28.0, {-0.17, 0.17, -0.17, 0.17}, {0.17, 0.17, -0.17, -0.17}},
    {"1x10", 1, 10.0, 0.42, 0.38, 0.22, 24.0, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
    {"2x10", 2, 10.0, 0.60, 0.40, 0.24, 20.0, {-0.14, 0.14, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
};

struct Vec3 {
  double x, y, z;
};

/** A complex product without the checks for infinities std::complex makes
    (it calls a library routine for them): the hot loops multiply finite
    numbers only. */
inline Complex times(const Complex& a, const Complex& b) noexcept {
  return {a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()};
}

double distance(const Vec3& a, const Vec3& b) noexcept {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

/** |2 J1(x) / x| of a rigid piston, as its smooth envelope: -3 dB at
    x = 1.62 like the piston, then falling as x^-1.5 like the envelope of its
    side lobes (a real cone has no clean nulls). */
double pistonDirectivity(double x) noexcept {
  const double u = std::abs(x) / 1.62;
  return 1.0 / std::sqrt(1.0 + u * u * u);
}

/** Equivalent point-source distance of a piston of radius `a` heard on axis
    at distance `r`: a / 2 at the cone, r far away. On axis, the near field of
    a piston at low frequencies is k a^2 / (2 R) with R this distance
    (|p| = 2 rho c u sin(k (sqrt(r^2 + a^2) - r) / 2)). */
double pistonDistance(double r, double a) noexcept {
  return a * a / (2.0 * (std::sqrt(r * r + a * a) - r));
}

/** A second-order resonator, peak or dip, as the analogue peaking section
    (s^2 + s (A / Q) w0 + w0^2) / (s^2 + s / (A Q) w0 + w0^2), A = 10^(g / 40). */
struct Resonator {
  double frequencyHz{1000.0};
  double zeroDamping{1.0};
  double poleDamping{1.0};

  static Resonator from(const ConeResonance& r, double frequencyScale) noexcept {
    const double a = std::pow(10.0, r.gainDb / 40.0);
    return {r.frequencyHz * frequencyScale, a / r.q, 1.0 / (a * r.q)};
  }
  Complex at(double f) const noexcept {
    const Complex s(0.0, f / frequencyHz);
    return (s * s + s * zeroDamping + 1.0) / (s * s + s * poleDamping + 1.0);
  }
};

/** Second-order high-pass at `fc` with quality `q`. */
Complex highPass2(double f, double fc, double q) noexcept {
  const Complex s(0.0, f / fc);
  return s * s / (s * s + s / q + 1.0);
}

/** Second-order low-pass at `fc` with quality `q`. */
Complex lowPass2(double f, double fc, double q) noexcept {
  const Complex s(0.0, f / fc);
  return 1.0 / (s * s + s / q + 1.0);
}

Complex lowPass1(double f, double fc) noexcept { return 1.0 / Complex(1.0, f / fc); }

/** Butterworth low-pass magnitude of order `n`. */
double butterworthLowMagnitude(double f, double fc, int n) noexcept {
  return 1.0 / std::sqrt(1.0 + std::pow(f / fc, 2.0 * n));
}

/** What a mic type is made of. */
struct MicModel {
  // Polar pattern: pressure part plus gradient part (cardioid 0.5 / 0.5,
  // figure of eight 0 / 1).
  double pressure;
  double gradient;
  // Effective capsule radius for the high-frequency loss off axis (the
  // diaphragm plus what the housing adds).
  double capsuleRadius;
  // The capsule's own low end: a second-order high-pass.
  double lowCornerHz;
  double lowQ;
  // The top: a second-order low-pass.
  double highCornerHz;
  double highQ;
  // A presence resonance (0 dB for none).
  ConeResonance presence;
};

const MicModel& micModel(MicType type) noexcept {
  static const MicModel models[kMicTypes] = {
      // Dynamic: a cardioid moving coil. The diaphragm's mass and the
      // capsule's front cavity make a broad presence peak; its bass is tuned
      // down to offset the proximity effect up close.
      {0.5, 0.5, 0.025, 180.0, 0.55, 12500.0, 0.8, {5200.0, 5.0, 1.1}},
      // Ribbon: a figure of eight. The ribbon's mass and the transformer roll
      // the top off early and gently; all pressure gradient, it has the most
      // proximity effect.
      {0.0, 1.0, 0.026, 90.0, 0.6, 6500.0, 0.55, {3000.0, 0.0, 1.0}},
      // Condenser: a cardioid with a light diaphragm, flat from the bottom to
      // past the guitar's range.
      {0.5, 0.5, 0.022, 25.0, 0.7, 18000.0, 0.7, {11000.0, 1.0, 1.0}},
  };
  return models[static_cast<int>(type)];
}

/** One way the sound gets from the cab to the mic. */
struct Path {
  enum class Kind {
    // From a cone (or its mirror image) straight to the mic: the cone's
    // directivity towards the mic.
    Radiated,
    // Diffracted at a baffle edge: what the cone sends along the baffle.
    Edge,
    // An open back's rear wave, round the box: the same, low-passed.
    Rear,
  };
  Kind kind{Kind::Radiated};
  // Everything that does not depend on frequency: spreading, sign,
  // reflection coefficients, the obliquity, the mic's pattern at the angle of
  // arrival.
  double amplitude{0.0};
  // Length, for the delay.
  double length{0.0};
  // A radiated path's direction from its cone: across and along the axis.
  double lateral{0.0};
  double axial{0.0};
  // Bounces off the room's walls (each a low-pass).
  int bounces{0};
};

/** Everything about one cab that does not depend on frequency. */
struct Scene {
  // The speaker, scaled to the cab's cone size.
  double qBox{0.7};
  double fBox{100.0};
  double coilRe{6.4};
  double coilK{0.0};
  double coilN{0.65};
  Complex coilPhase{1.0, 0.0};
  double rolloffHz{5000.0};
  int rolloffOrder{4};
  std::vector<Resonator> breakup;
  // The cone.
  double pistonRadius{0.1};
  double radiatingFraction{0.5};
  double breakupOnsetHz{1000.0};
  // Every path to the mic.
  std::vector<Path> paths;
  // The mic.
  MicModel micData{};
  Resonator micPresence;
  double micAngle{0.0};
  double proximityDistance{0.1};
  // Filters.
  double lowCutHz{30.0};
  double highCutHz{16000.0};
};

/** The radius of the part of the cone that moves as one at `f`: all of it low
    down, the inner `radiatingFraction` of it once the cone breaks up. */
double radiatingRadius(const Scene& scene, double f) noexcept {
  const double fb = f / scene.breakupOnsetHz;
  return scene.pistonRadius * (scene.radiatingFraction + (1.0 - scene.radiatingFraction) / (1.0 + fb * fb));
}

/** The paths from every speaker of the cab to the mic (see
    docs/cab-framework.md, "The paths"). */
void addPaths(Scene& scene, const CabGeometry& geometry, CabBack back, double room, const Vec3& mic,
              double coneDepth) {
  const MicModel& m = scene.micData;
  // A reflection arriving at the mic from `from`: the mic's pattern at that
  // angle against its pattern on axis (the mic faces the baffle).
  const auto arrival = [&](const Vec3& from) {
    const double r = distance(from, mic);
    const double cosine = r > 0.0 ? (mic.z - from.z) / r : 1.0;
    return (m.pressure + m.gradient * cosine) / (m.pressure + m.gradient);
  };
  // From a cone at `from`, with the obliquity (1 + cos) / 2 of a source
  // sitting in its frame behind the baffle, which the frame shades at grazing
  // angles: the neighbours of a close mic are heard across the baffle.
  const auto radiated = [&](const Vec3& from, double amplitude, int bounces) {
    Path p;
    p.kind = Path::Kind::Radiated;
    p.length = std::max(distance(from, mic), 1.0e-6);
    p.lateral = std::sqrt((mic.x - from.x) * (mic.x - from.x) + (mic.y - from.y) * (mic.y - from.y));
    p.axial = std::abs(mic.z - from.z);
    p.amplitude = amplitude * 0.5 * (1.0 + p.axial / p.length);
    p.bounces = bounces;
    scene.paths.push_back(p);
  };
  const auto along = [&](Path::Kind kind, double amplitude, double length) {
    Path p;
    p.kind = kind;
    p.amplitude = amplitude;
    p.length = length;
    scene.paths.push_back(p);
  };

  const double halfW = 0.5 * geometry.width;
  const double halfH = 0.5 * geometry.height;
  const double backDepth = geometry.depth - coneDepth;
  // The floor, in baffle coordinates (the baffle's centre is the origin).
  const double floorY = -(0.5 * geometry.height + kFeetMetres);
  // A close mic hears the room as if it were at least kRoomPickupMetres away.
  const double roomGain = 0.9 * std::clamp(room, 0.0, 1.0) *
                          std::max(scene.proximityDistance, kRoomPickupMetres) / scene.proximityDistance;

  for (int j = 0; j < geometry.speakers; ++j) {
    const Vec3 s{geometry.x[j], geometry.y[j], -coneDepth};
    // Direct sound, spread as the piston's near field.
    radiated(s, 1.0 / pistonDistance(distance(s, mic), scene.pistonRadius), 0);

    // The four baffle edges, each a secondary source of the opposite sign
    // (half the incident wave, shared between the edges): far away they sum
    // to the 6 dB baffle step, up close they are nothing against the direct
    // sound.
    const Vec3 edges[4] = {{-halfW, s.y, 0.0}, {halfW, s.y, 0.0}, {s.x, -halfH, 0.0}, {s.x, halfH, 0.0}};
    for (const Vec3& e : edges) {
      const double toEdge = std::abs(e.x - s.x) + std::abs(e.y - s.y);
      const double path = toEdge + distance(e, mic);
      along(Path::Kind::Edge, -0.5 * 0.25 / path, path);
      if (back == CabBack::Open) {
        // The rear wave, inverted, out of the open back and round the box.
        const double rear = std::hypot(backDepth, toEdge) + geometry.depth + distance(e, mic);
        along(Path::Kind::Rear, -0.25 / rear, rear);
        // And off the wall behind the cab.
        const double wall = rear + 2.0 * kWallBehindMetres;
        along(Path::Kind::Rear, -0.25 * kBackWallReflection / wall, wall);
      }
    }

    // The floor, as the image of the source.
    const Vec3 floorImage{s.x, 2.0 * floorY - s.y, s.z};
    radiated(floorImage, kFloorReflection * arrival(floorImage) / distance(floorImage, mic), 0);

    // The room: first-order images in the side walls, the ceiling and the wall
    // in front, and each of them again in the floor. A mirror image leaves the
    // cone at the angle the reflected sound did, so the cone's directivity is
    // read off the image's direction.
    if (roomGain > 0.0) {
      const double ceiling = floorY + kRoomCeilingMetres;
      const Vec3 first[4] = {{-2.0 * kRoomSideMetres - s.x, s.y, s.z},
                             {2.0 * kRoomSideMetres - s.x, s.y, s.z},
                             {s.x, 2.0 * ceiling - s.y, s.z},
                             {s.x, s.y, 2.0 * kRoomFrontMetres - s.z}};
      for (const Vec3& image : first) {
        radiated(image, roomGain * arrival(image) / distance(image, mic), 1);
        const Vec3 twice{image.x, 2.0 * floorY - image.y, image.z};
        radiated(twice, roomGain * kFloorReflection * arrival(twice) / distance(twice, mic), 2);
      }
    }
  }
}

Scene makeScene(const SpeakerDescription& speaker, const CabSettings& settings) {
  Scene scene;
  const CabGeometry& geometry = cabGeometry(settings.size);
  // The speaker's values are for its own nominal size; in a cab of the other
  // size the cone is scaled by the ratio of the diameters: the modes of a
  // smaller cone sit higher (frequency as 1 / D), so does its resonance (the
  // moving mass falls as D^2 against a similar suspension), and its Vas falls
  // as Sd^2, D^4.
  const double ratio = speaker.diameterInches / geometry.speakerInches;
  const double fs = speaker.resonanceHz * ratio;
  const double vas = speaker.complianceLitres / std::pow(ratio, 4.0);
  if (settings.back == CabBack::Open) {
    // No box air behind the cone: the free-air resonance and Q.
    scene.fBox = fs;
    scene.qBox = speaker.qTotal;
  } else {
    // A closed box adds its air as a second spring, alpha = Vas / Vb: the
    // resonance and the Q both rise by sqrt(1 + alpha) (R. H. Small, "Closed-box
    // loudspeaker systems", JAES 20(10), 1972).
    const double alpha = vas / geometry.litresPerSpeaker;
    scene.fBox = fs * std::sqrt(1.0 + alpha);
    scene.qBox = speaker.qTotal * std::sqrt(1.0 + alpha);
  }
  scene.coilRe = speaker.coilResistanceOhm;
  scene.coilN = speaker.coilLossExponent;
  const double w1k = 2.0 * kPi * 1000.0;
  scene.coilK = speaker.coilInductanceMh * 1.0e-3 * w1k / std::pow(w1k, scene.coilN);
  scene.coilPhase = std::polar(1.0, scene.coilN * 0.5 * kPi);
  scene.rolloffHz = speaker.rolloffHz * ratio;
  scene.rolloffOrder = speaker.rolloffOrder;
  for (const auto& r : speaker.breakup)
    scene.breakup.push_back(Resonator::from(r, ratio));

  scene.pistonRadius = 0.5 * geometry.speakerInches * kMetresPerInch * kPistonFraction;
  scene.radiatingFraction = speaker.radiatingFraction;
  // The cone stops moving as one around 1 kHz on a 12 inch speaker, higher
  // on a smaller one.
  scene.breakupOnsetHz = 1000.0 * 12.0 / geometry.speakerInches;
  // The cone's acoustic centre: half way down from the baffle to the dust cap.
  const double coneDepth = 0.5 * geometry.speakerInches * kConeDepthPerInch;

  // The mic: on speaker 0, moving from the dust cap towards the edge of the
  // cone in the direction of the box's centre (the right on a single speaker).
  const double direction = geometry.speakers > 1 && geometry.x[0] > 0.0 ? -1.0 : 1.0;
  const double position = std::clamp(settings.position, 0.0, 1.0);
  const double distanceMetres = std::clamp(settings.distanceCm, 0.1, 300.0) * 0.01;
  const Vec3 mic{geometry.x[0] + direction * position * scene.pistonRadius, geometry.y[0],
                 kGrilleMetres + distanceMetres};

  scene.micData = micModel(settings.mic);
  scene.micPresence = Resonator::from(scene.micData.presence, 1.0);
  scene.micAngle = std::clamp(settings.angleDeg, 0.0, 90.0) * kPi / 180.0;
  // The proximity effect follows the distance from the source the pressure
  // gradient sees: the piston's equivalent distance on speaker 0's axis.
  scene.proximityDistance = pistonDistance(mic.z + coneDepth, scene.pistonRadius);

  addPaths(scene, geometry, settings.back, settings.room, mic, coneDepth);

  scene.lowCutHz = settings.lowCutHz;
  scene.highCutHz = settings.highCutHz;
  return scene;
}

/** The speaker alone: the cone's acceleration under a voltage drive, then the
    breakup and the roll-off. Common to every source of the cab. */
Complex speakerResponse(const Scene& scene, double f) {
  // The box: a second-order high-pass at its resonance.
  Complex h = highPass2(f, scene.fBox, scene.qBox);
  // The voice coil: current, so force, falls as the coil's impedance
  // Re + K (j w)^n rises above Re.
  if (scene.coilK > 0.0) {
    const Complex coil = scene.coilK * std::pow(2.0 * kPi * f, scene.coilN) * scene.coilPhase;
    h *= scene.coilRe / (scene.coilRe + coil);
  }
  for (const auto& r : scene.breakup)
    h *= r.at(f);
  h *= butterworthLowMagnitude(f, scene.rolloffHz, scene.rolloffOrder);
  return h;
}

/** Every path from the cab to the mic, summed with its delay. `phasors`, when
    given, holds exp(-j k length) of each path at this frequency. */
Complex pathSum(const Scene& scene, double f, double k, const Complex* phasors) {
  const double a = radiatingRadius(scene, f);
  // What a cone sends along the baffle (to an edge, or round the box).
  const double grazing = pistonDirectivity(k * a);
  // The rear wave of an open back wraps round the box whole at low
  // frequencies (no obliquity: it is not grazing a baffle but diffracting
  // round a corner) and loses its top on the way.
  const Complex rear = lowPass1(f, kRearWaveCornerHz) * grazing;
  const Complex absorb = lowPass1(f, kRoomAbsorptionHz);
  const Complex absorbTwice = absorb * absorb;
  const double beam = kBeamFraction * a;

  Complex sum = 0.0;
  for (size_t i = 0; i < scene.paths.size(); ++i) {
    const Path& p = scene.paths[i];
    const Complex phase = phasors != nullptr ? phasors[i] : std::polar(1.0, -k * p.length);
    switch (p.kind) {
      case Path::Kind::Radiated: {
        // The cone's directivity, with the angle taken from the near side of
        // the radiating disc rather than from its centre: a mic over the
        // moving part of the cone is in its beam, so the top end only falls
        // away once the mic passes beyond it (this is what makes the
        // position matter up close and hardly at all far away).
        const double outside = std::max(0.0, p.lateral - beam);
        const double sine = outside / std::sqrt(outside * outside + p.axial * p.axial + 1.0e-12);
        const double directivity = p.amplitude * pistonDirectivity(k * a * sine);
        if (p.bounces == 0)
          sum += directivity * phase;
        else
          sum += directivity * times(p.bounces == 1 ? absorb : absorbTwice, phase);
        break;
      }
      case Path::Kind::Edge:
        sum += p.amplitude * grazing * phase;
        break;
      case Path::Kind::Rear:
        sum += p.amplitude * times(rear, phase);
        break;
    }
  }
  return sum;
}

Complex cabResponseAt(const Scene& scene, double f, const Complex* phasors) {
  if (f <= 0.0)
    return 0.0;
  const double k = 2.0 * kPi * f / kSpeedOfSound;
  Complex h = speakerResponse(scene, f);
  h *= pathSum(scene, f, k, phasors);

  // The mic: its pattern on the main path (tilted by its angle), with the
  // proximity effect of its gradient part (1 + 1 / (j k r)); its own low and
  // high ends and presence; and the capsule's high-frequency loss off axis.
  const MicModel& m = scene.micData;
  const Complex gradient = 1.0 + 1.0 / Complex(0.0, k * scene.proximityDistance);
  h *= m.pressure + m.gradient * std::cos(scene.micAngle) * gradient;
  h *= highPass2(f, m.lowCornerHz, m.lowQ);
  h *= lowPass2(f, m.highCornerHz, m.highQ);
  if (m.presence.gainDb != 0.0)
    h *= scene.micPresence.at(f);
  h *= pistonDirectivity(k * m.capsuleRadius * std::sin(scene.micAngle));

  // The cut filters.
  h *= highPass2(f, scene.lowCutHz, std::sqrt(0.5));
  h *= lowPass2(f, scene.highCutHz, std::sqrt(0.5));
  return h;
}

// ---- FFT ----------------------------------------------------------------------

/** In-place iterative radix-2 FFT; `inverse` scales by 1 / n. */
void fft(std::vector<Complex>& data, bool inverse) {
  const size_t n = data.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(data[i], data[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
    const Complex step = std::polar(1.0, angle);
    for (size_t i = 0; i < n; i += len) {
      Complex w = 1.0;
      for (size_t j = 0; j < len / 2; ++j) {
        const Complex u = data[i + j];
        const Complex v = data[i + j + len / 2] * w;
        data[i + j] = u + v;
        data[i + j + len / 2] = u - v;
        w *= step;
      }
    }
  }
  if (inverse)
    for (auto& x : data)
      x /= static_cast<double>(n);
}

}  // namespace

const CabGeometry& cabGeometry(CabSize size) noexcept {
  return kGeometries[std::clamp(static_cast<int>(size), 0, kCabSizes - 1)];
}

const char* cabSizeName(CabSize size) noexcept { return cabGeometry(size).label; }

const char* micTypeName(MicType mic) noexcept {
  switch (mic) {
    case MicType::Dynamic: return "dynamic";
    case MicType::Ribbon: return "ribbon";
    case MicType::Condenser: return "condenser";
  }
  return "";
}

std::vector<std::complex<double>> cabFrequencyResponse(const SpeakerDescription& speaker,
                                                       const CabSettings& settings,
                                                       const std::vector<double>& frequenciesHz) {
  const Scene scene = makeScene(speaker, settings);
  std::vector<std::complex<double>> out;
  out.reserve(frequenciesHz.size());
  for (const double f : frequenciesHz)
    out.push_back(cabResponseAt(scene, f, nullptr));
  return out;
}

int cabImpulseLength(double sampleRate) noexcept {
  // 85 ms, rounded up to a power of two: the box resonance, the low cut and
  // the room's reflections have all died away by then.
  const double wanted = 0.0853 * std::max(8000.0, sampleRate);
  int length = 256;
  while (length < wanted)
    length <<= 1;
  return length;
}

std::vector<float> cabImpulseResponse(const SpeakerDescription& speaker, const CabSettings& settings,
                                      double sampleRate) {
  const int length = cabImpulseLength(sampleRate);
  // Four times the kernel: the cepstrum of a response with deep notches decays
  // slowly, and a short transform would fold it back on itself.
  const size_t n = static_cast<size_t>(length) * 4;
  const Scene scene = makeScene(speaker, settings);

  // Magnitude on the transform's bins, 0 to Nyquist. The bins are evenly
  // spaced, so each path's delay turns by a fixed step from one to the next:
  // its phasor is rotated rather than recomputed (and recomputed exactly every
  // 64 bins, so rounding cannot build up).
  const size_t half = n / 2;
  const double binHz = sampleRate / static_cast<double>(n);
  const size_t numPaths = scene.paths.size();
  std::vector<Complex> phasors(numPaths, Complex(1.0, 0.0));
  std::vector<Complex> steps(numPaths);
  for (size_t p = 0; p < numPaths; ++p)
    steps[p] = std::polar(1.0, -2.0 * kPi * binHz / kSpeedOfSound * scene.paths[p].length);
  std::vector<double> magnitude(half + 1);
  double peak = 0.0;
  for (size_t i = 0; i <= half; ++i) {
    const double f = binHz * static_cast<double>(i);
    if (i % 64 == 0) {
      const double k = 2.0 * kPi * f / kSpeedOfSound;
      for (size_t p = 0; p < numPaths; ++p)
        phasors[p] = std::polar(1.0, -k * scene.paths[p].length);
    }
    magnitude[i] = std::abs(cabResponseAt(scene, f, phasors.data()));
    peak = std::max(peak, magnitude[i]);
    for (size_t p = 0; p < numPaths; ++p)
      phasors[p] = times(phasors[p], steps[p]);
  }

  // The level: 0 dB on average from 150 Hz to 3.5 kHz, power on a log axis.
  double weighted = 0.0, weights = 0.0;
  for (size_t i = 1; i <= half; ++i) {
    const double f = binHz * static_cast<double>(i);
    if (f < 150.0 || f > 3500.0)
      continue;
    weighted += magnitude[i] * magnitude[i] / f;
    weights += 1.0 / f;
  }
  const double reference = weights > 0.0 && weighted > 0.0 ? std::sqrt(weighted / weights) : peak;
  const double floor = std::max(peak, 1.0e-30) * 1.0e-6;

  // Minimum phase by the real cepstrum (A. V. Oppenheim and R. W. Schafer,
  // Discrete-Time Signal Processing, 3rd ed., section 12.8): the log magnitude
  // to the cepstrum, keep it causal (double the positive quefrencies), back to
  // a spectrum, exponentiate, back to time.
  std::vector<Complex> work(n);
  for (size_t i = 0; i <= half; ++i) {
    const double value = std::log(std::max(magnitude[i], floor) / reference);
    work[i] = value;
    if (i > 0 && i < half)
      work[n - i] = value;
  }
  fft(work, true);
  for (size_t i = 1; i < half; ++i) {
    work[i] = 2.0 * work[i].real();
    work[n - i] = 0.0;
  }
  work[0] = work[0].real();
  work[half] = work[half].real();
  fft(work, false);
  for (auto& x : work)
    x = std::exp(x);
  fft(work, true);

  std::vector<float> ir(static_cast<size_t>(length));
  const int fadeStart = length - length / 4;
  for (int i = 0; i < length; ++i) {
    double value = work[static_cast<size_t>(i)].real();
    if (i >= fadeStart) {
      const double t = static_cast<double>(i - fadeStart) / static_cast<double>(length - fadeStart);
      value *= 0.5 * (1.0 + std::cos(kPi * t));
    }
    ir[static_cast<size_t>(i)] = static_cast<float>(value);
  }
  return ir;
}

}  // namespace cab
