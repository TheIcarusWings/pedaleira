#include "effects/dsp/Noise.h"
#include "ImageDeck.h"

namespace fx {

std::uint32_t effectNoiseSeed(std::uint32_t salt) noexcept {
  // The fixed base every effect stream derives from.
  std::uint32_t seed = 0x47524944u;  // "GRID"
  // The offline-render override lives with the Spread wobble (DeckWobble),
  // which is what it was made for; effects follow the same switch.
  if (DeckWobble::seeded.load(std::memory_order_acquire)) {
    const auto pinned = static_cast<std::uint64_t>(DeckWobble::seed.load(std::memory_order_relaxed));
    seed ^= hash32(static_cast<std::uint32_t>(pinned) ^ static_cast<std::uint32_t>(pinned >> 32));
  }
  return hash32(seed ^ hash32(salt + 0x9e3779b9u));
}

}  // namespace fx
