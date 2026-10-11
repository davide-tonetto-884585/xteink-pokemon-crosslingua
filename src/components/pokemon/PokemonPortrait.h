#pragma once

#if defined(CROSSINK_ENABLE_POKEMON)

#include <cstdint>
#include <memory>

namespace pokemon {

// Values of a portrait pixel: 0..14 are grays from black to white, 15 is transparent.
constexpr uint8_t PORTRAIT_TRANSPARENT = 15;

// Loads the art pack's grayscale portrait of a species (sleep/NNN.bmp: a 4-bit
// indexed BMP, see scripts/generate_pokemon_sleep_art.py) into `gray4`, top
// row first, (width + 1) / 2 bytes per row, high nibble first. Returns false
// when the art pack predates these portraits, or memory or storage fail.
bool loadPokemonPortrait(uint16_t speciesId, int& width, int& height, std::unique_ptr<uint8_t[]>& gray4);

inline uint8_t portraitPixel(const uint8_t* gray4, const int width, const int x, const int y) {
  const uint8_t byte = gray4[static_cast<size_t>(y) * ((width + 1) / 2) + x / 2];
  return (x & 1) == 0 ? byte >> 4 : byte & 0x0F;
}

}  // namespace pokemon

#endif
