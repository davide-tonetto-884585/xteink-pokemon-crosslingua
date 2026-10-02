#pragma once

#include <GfxRenderer.h>

#include "MappedInputManager.h"

// Touch fallbacks for the Lingua screens that were written for button-only readers. On a touch-only
// board (X4 Pro) the bottom strip -- where the button hints sit -- acts as Back, and a tap anywhere
// else acts as Confirm / selects what is under the finger.
namespace LinguaTouch {

enum class Tap : uint8_t { None, Back, Confirm };

// Height of the bottom "Back" strip, as a fraction of the screen.
constexpr int BACK_ZONE_PERCENT = 15;

inline Tap readTap(const MappedInputManager& input, const GfxRenderer& renderer, int* outX = nullptr,
                   int* outY = nullptr) {
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return Tap::None;
  if (outX) *outX = x;
  if (outY) *outY = y;
  const int h = renderer.getScreenHeight();
  return y >= h - (h * BACK_ZONE_PERCENT) / 100 ? Tap::Back : Tap::Confirm;
}

}  // namespace LinguaTouch
