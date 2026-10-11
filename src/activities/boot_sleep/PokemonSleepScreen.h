#pragma once

#if defined(CROSSINK_ENABLE_POKEMON)

class GfxRenderer;

namespace pokemon {

// Where the generated sleep image is written before SleepActivity shows it.
inline constexpr char POKEMON_SLEEP_IMAGE_PATH[] = "/.crosspoint/pokemon-sleep.bmp";

// Picks a random party member and writes its sleep screen as a screen-sized
// 8-bit grayscale BMP to POKEMON_SLEEP_IMAGE_PATH: the room seen through a
// Poke Ball with the labels underneath, or (fullScreen) the room filling the
// screen with the labels on a panel at the bottom.
// The labels are drawn through `renderer` (its frame buffer is used as a
// scratch text mask and left cleared). Returns false when there is no party
// yet, or memory or storage fail; the caller then falls back to another
// sleep screen.
bool writePokemonSleepImage(const GfxRenderer& renderer, bool fullScreen);

}  // namespace pokemon

#endif
