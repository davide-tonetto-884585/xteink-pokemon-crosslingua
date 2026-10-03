#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "components/themes/BaseTheme.h"

// Drawing helpers shared by the chapter and book translation screens: the standard header (with the
// battery indicator), centered info lines, a large percentage + progress bar, and a row of tappable
// option buttons. All functions take the current y and return the y below what they drew.
namespace TranslationProgressUi {

// Header bar with the screen title and the battery level, like every other CrossInk screen.
int drawHeader(GfxRenderer& renderer, const char* title);

// One centered line, truncated to the screen width.
int drawLine(GfxRenderer& renderer, int y, const char* text, int fontId, bool bold = false);

// Centered text wrapped over up to `maxLines` lines.
int drawWrapped(GfxRenderer& renderer, int y, const char* text, int fontId, int maxLines = 3);

// Large "NN%" followed by a progress bar.
int drawProgress(GfxRenderer& renderer, int y, int current, int total);

// Thin horizontal separator with vertical padding.
int drawSeparator(GfxRenderer& renderer, int y);

// Up to 2 option buttons stacked vertically; the selected one is drawn inverted. Their rectangles are
// written to outRects so the activity can hit-test taps.
int drawButtons(GfxRenderer& renderer, int y, const char* const* labels, int count, int selected, Rect* outRects);

// "m:ss" or "h:mm:ss".
std::string formatDuration(unsigned long ms);

inline bool rectContains(const Rect& r, int x, int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace TranslationProgressUi
