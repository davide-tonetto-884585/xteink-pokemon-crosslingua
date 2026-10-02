#include "components/WrappedPopup.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

void drawWrappedPopup(const GfxRenderer& renderer, const char* message) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style fontStyle = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

  // Constrain the popup to the oriented viewable area (inside the bezel) so it fits in every
  // orientation, unlike drawPopup which sizes to the full string width against the whole screen.
  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int viewableW = screenW - viewLeft - viewRight;
  const int viewableH = screenH - viewTop - viewBottom;

  // Leave room for the box margins and frame on both sides; wrap the text into that width.
  const int maxTextW = std::max(1, viewableW - 2 * (marginX + frameThickness));
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  // Cap lines to what the viewable height can hold; a toast never needs more than a few.
  constexpr int POPUP_MAX_LINES = 4;
  const int maxLinesByHeight = std::max(1, (viewableH - 2 * (marginY + frameThickness)) / std::max(1, lineHeight));
  const int maxLines = std::min(POPUP_MAX_LINES, maxLinesByHeight);

  // wrappedText breaks on spaces and UTF-8-safely hard-clips overflowing words / excess lines.
  const auto lines = renderer.wrappedText(UI_12_FONT_ID, message, maxTextW, maxLines, fontStyle);
  if (lines.empty()) return;

  // Box width tracks the widest wrapped line (clamped to the wrap width so it never exceeds
  // the viewable area); height grows with the wrapped line count.
  int textW = 0;
  for (const auto& line : lines) {
    textW = std::max(textW, renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), fontStyle));
  }
  textW = std::min(textW, maxTextW);

  const int w = textW + marginX * 2;
  const int h = static_cast<int>(lines.size()) * lineHeight + marginY * 2;
  const int x = (screenW - w) / 2;
  const int y = static_cast<int>(screenH * metrics.popupTopOffsetRatio);

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  for (const auto& line : lines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), fontStyle);
    const int textX = x + (w - lineW) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, line.c_str(), metrics.popupTextInverted, fontStyle);
    textY += lineHeight;
  }
  renderer.displayBuffer();
}
