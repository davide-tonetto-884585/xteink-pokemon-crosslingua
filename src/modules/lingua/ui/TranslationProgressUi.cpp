#include "modules/lingua/ui/TranslationProgressUi.h"

#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

namespace TranslationProgressUi {

namespace {
constexpr int SIDE_MARGIN = 24;
}

int drawHeader(GfxRenderer& renderer, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
  return metrics.topPadding + metrics.headerHeight + 14;
}

int drawLine(GfxRenderer& renderer, const int y, const char* text, const int fontId, const bool bold) {
  if (text && text[0] != '\0') {
    const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string fitted =
        renderer.truncatedText(fontId, text, renderer.getScreenWidth() - 2 * SIDE_MARGIN, style);
    renderer.drawCenteredText(fontId, y, fitted.c_str(), true, style);
  }
  return y + renderer.getLineHeight(fontId) + 4;
}

int drawWrapped(GfxRenderer& renderer, int y, const char* text, const int fontId, const int maxLines) {
  const auto lines = renderer.wrappedText(fontId, text, renderer.getScreenWidth() - 2 * SIDE_MARGIN, maxLines);
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    renderer.drawCenteredText(fontId, y, line.c_str());
    y += lineHeight;
  }
  return y + 4;
}

int drawProgress(GfxRenderer& renderer, int y, const int current, const int total) {
  const int pct = total > 0 ? static_cast<int>((static_cast<int64_t>(current) * 100) / total) : 0;
  char pctText[8];
  snprintf(pctText, sizeof(pctText), "%d%%", pct > 100 ? 100 : pct);
  renderer.drawCenteredText(UI_12_FONT_ID, y, pctText, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 8;

  const int barX = SIDE_MARGIN;
  const int barW = renderer.getScreenWidth() - 2 * SIDE_MARGIN;
  constexpr int barH = 16;
  renderer.drawRect(barX, y, barW, barH, true);
  renderer.drawRect(barX + 1, y + 1, barW - 2, barH - 2, true);
  if (total > 0 && current > 0) {
    int fillW = static_cast<int>((static_cast<int64_t>(barW - 6) * current) / total);
    if (fillW > barW - 6) fillW = barW - 6;
    if (fillW > 0) renderer.fillRect(barX + 3, y + 3, fillW, barH - 6, true);
  }
  return y + barH + 14;
}

int drawSeparator(GfxRenderer& renderer, int y) {
  renderer.drawLine(SIDE_MARGIN, y, renderer.getScreenWidth() - SIDE_MARGIN, y, true);
  return y + 14;
}

int drawButtons(GfxRenderer& renderer, int y, const char* const* labels, const int count, const int selected,
                Rect* outRects) {
  const int w = renderer.getScreenWidth() - 2 * SIDE_MARGIN;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + 20;
  for (int i = 0; i < count; i++) {
    const Rect r{SIDE_MARGIN, y, w, h};
    if (outRects) outRects[i] = r;
    const bool sel = i == selected;
    if (sel) {
      renderer.fillRect(r.x, r.y, r.width, r.height, true);
    } else {
      renderer.drawRect(r.x, r.y, r.width, r.height, true);
      renderer.drawRect(r.x + 1, r.y + 1, r.width - 2, r.height - 2, true);
    }
    const std::string fitted = renderer.truncatedText(UI_12_FONT_ID, labels[i], w - 20, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, y + 10, fitted.c_str(), !sel, EpdFontFamily::BOLD);
    y += h + 12;
  }
  return y;
}

std::string formatDuration(const unsigned long ms) {
  const unsigned long totalSec = ms / 1000;
  const unsigned long h = totalSec / 3600;
  const unsigned long m = (totalSec / 60) % 60;
  const unsigned long s = totalSec % 60;
  char buf[16];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", h, m, s);
  } else {
    snprintf(buf, sizeof(buf), "%lu:%02lu", m, s);
  }
  return buf;
}

}  // namespace TranslationProgressUi
