#pragma once

class GfxRenderer;

// Fork-owned popup that word-wraps its message to fit the oriented viewable area (inside the
// bezel) instead of sizing a single-line box to the full string width, as BaseTheme::drawPopup
// does. Use it for messages that can be long in some languages (translated Lingua toasts), which
// would otherwise overflow the screen.
//
// Lives outside the theme classes on purpose: no theme ever overrode it, and keeping it here
// leaves BaseTheme/LyraTheme/RoundedRaffTheme byte-identical to upstream, so a future upstream
// merge does not conflict over it. Styling still comes from the active theme's metrics.
//
// Wraps on spaces, hard-clips unbreakable words, grows in height up to a small line cap, is
// centered, and works in all 4 orientations by construction (renderer dimensions only). Sizes and
// flushes like BaseTheme::drawPopup.
void drawWrappedPopup(const GfxRenderer& renderer, const char* message);
