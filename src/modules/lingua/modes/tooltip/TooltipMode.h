#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
#include "modules/lingua/modes/tooltip/TooltipOverlay.h"
class TooltipMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_TOOLTIP; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::OriginalOnly; }
  LinguaLayout layout() const override { return layoutValue(); }

  bool handleInput(MappedInputManager& input) { return overlay_.handleInput(input); }
  bool active() const { return overlay_.isActive(); }
  bool hasPendingPageTurn() const { return overlay_.pendingPageForward || overlay_.pendingPageBack; }
  bool takePendingPageTurn() {
    const bool forward = overlay_.pendingPageForward;
    overlay_.pendingPageForward = false;
    overlay_.pendingPageBack = false;
    return forward;
  }
  void render(GfxRenderer& renderer, const Page& page, int fontId, int tooltipFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight) {
    overlay_.render(renderer, page, fontId, tooltipFontId, xOffset, yOffset, viewportWidth, viewportHeight);
  }
  void collectGlyphText(const Page& page, std::string& out) { overlay_.collectPageGlyphText(page, out); }
  void setTranslatedHtmlPath(const std::string& path) { overlay_.setTranslatedHtmlPath(path); }
  void onPageChanged() { overlay_.onPageChanged(); }

 private:
  TooltipOverlay overlay_;
};
