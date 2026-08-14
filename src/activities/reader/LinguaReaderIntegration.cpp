#include "LinguaReaderIntegration.h"

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleInput(MappedInputManager& input,
                                                                          const bool hasSection) {
  InputAction action = handleTransientUi();
  if (action != InputAction::None) return action;

  action = handleFallbackDialog(input);
  if (action != InputAction::None) return action;

  action = handleTooltipInput(input, hasSection);
  if (action != InputAction::None) return action;

  action = handlePageTranslationInput(input, hasSection);
  if (action != InputAction::None) return action;

  return handleInterlinearInput(input, hasSection);
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleTransientUi() {
  if (showNoTranslationsToast_ && millis() - noTranslationsToastTime_ >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showNoTranslationsToast_ = false;
    return InputAction::Render;
  }
  return InputAction::None;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleFallbackDialog(MappedInputManager& input) {
  if (!fallbackDialogActive_) return InputAction::None;
  if (fallbackDialogDrawn_ &&
      (input.wasReleased(MappedInputManager::Button::Confirm) || input.wasReleased(MappedInputManager::Button::Back))) {
    fallbackDialogActive_ = false;
    fallbackDialogDrawn_ = false;
    return InputAction::Render;
  }
  return InputAction::Consumed;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleTooltipInput(MappedInputManager& input,
                                                                                 const bool hasSection) {
  if (!hasSection || SETTINGS.translationDisplayMode != CrossPointSettings::LINGUA_TOOLTIP) {
    return InputAction::None;
  }

  if (tooltip_.handleInput(input)) {
    if (tooltip_.pendingPageForward || tooltip_.pendingPageBack) {
      const bool forward = tooltip_.pendingPageForward;
      tooltip_.pendingPageForward = false;
      tooltip_.pendingPageBack = false;
      return forward ? InputAction::PageForward : InputAction::PageBack;
    }
    return InputAction::Render;
  }

  const bool front = SETTINGS.tooltipButtons == CrossPointSettings::OVERLAY_BUTTONS_FRONT;
  const bool side = SETTINGS.tooltipButtons == CrossPointSettings::OVERLAY_BUTTONS_SIDE;
  const bool held = (front && (input.isPressed(MappedInputManager::Button::Left) ||
                               input.isPressed(MappedInputManager::Button::Right))) ||
                    (side && (input.isPressed(MappedInputManager::Button::PageBack) ||
                              input.isPressed(MappedInputManager::Button::PageForward)));
  return held ? InputAction::Consumed : InputAction::None;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handlePageTranslationInput(MappedInputManager& input,
                                                                                         const bool hasSection) {
  if (pageTranslation_.handleInput(input)) {
    return InputAction::Render;
  }

  if (pageTranslation_.isActive()) return InputAction::Consumed;

  if (hasSection && SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_PAGE_TRANSLATION) {
    const bool forward = input.wasReleased(MappedInputManager::Button::PageForward);
    const bool back = input.wasReleased(MappedInputManager::Button::PageBack);
    if (forward || back) {
      if (input.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
        pageTranslation_.open();
        clearOverlayPrewarm();
        return InputAction::Render;
      }
      return forward ? InputAction::PageForward : InputAction::PageBack;
    }
    if (input.isPressed(MappedInputManager::Button::PageForward) ||
        input.isPressed(MappedInputManager::Button::PageBack)) {
      return InputAction::Consumed;
    }
  }
  return InputAction::None;
}

LinguaReaderIntegration::InputAction LinguaReaderIntegration::handleInterlinearInput(MappedInputManager& input,
                                                                                     const bool hasSection) {
  if (hasSection && SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_INTERLINEAR &&
      SETTINGS.interlinearToggleByLongPress) {
    const bool front = SETTINGS.interlinearToggleButtons == CrossPointSettings::OVERLAY_BUTTONS_FRONT;
    const auto backButton = front ? MappedInputManager::Button::Left : MappedInputManager::Button::PageBack;
    const auto forwardButton = front ? MappedInputManager::Button::Right : MappedInputManager::Button::PageForward;
    const bool back = input.wasReleased(backButton);
    const bool forward = input.wasReleased(forwardButton);
    if (back || forward) {
      if (input.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
        toggleInterlinearTranslation();
        return InputAction::Render;
      }
      return forward ? InputAction::PageForward : InputAction::PageBack;
    }
    if (input.isPressed(backButton) || input.isPressed(forwardButton)) return InputAction::Consumed;
  }

  return InputAction::None;
}

PageFontSet LinguaReaderIntegration::pageFontSet() const {
  PageFontSet fonts = SETTINGS.readerPageFontSet();
  fonts.annotationVisible = interlinearTranslationVisible_;
  return fonts;
}

void LinguaReaderIntegration::renderOverlay(GfxRenderer& renderer, const Page& page, const int xOffset,
                                            const int yOffset, const int viewportWidth, const int viewportHeight) {
  const int bodyFont = SETTINGS.getReaderFontId();
  if (pageTranslation_.isActive()) {
    pageTranslation_.render(renderer, page, bodyFont, getPageTranslationFontId(), xOffset, yOffset, viewportWidth,
                            viewportHeight);
    if (!pageTranslation_.isActive()) {
      showNoTranslationsToast_ = true;
      noTranslationsToastTime_ = millis();
    }
  }
  if (tooltip_.isActive()) {
    tooltip_.render(renderer, page, bodyFont, getTooltipFontId(), xOffset, yOffset, viewportWidth, viewportHeight);
  }
}

bool LinguaReaderIntegration::prepareOverlayFonts(GfxRenderer& renderer, const Page& page, const PageFontSet& fonts,
                                                  const int spineIndex, const int pageIndex, const int xOffset,
                                                  const int yOffset) {
  if (!pageTranslation_.isActive() && !tooltip_.isActive()) {
    clearOverlayPrewarm();
    return false;
  }

  auto* cache = renderer.getFontCacheManager();
  const int overlayFont = pageTranslation_.isActive() ? getPageTranslationFontId() : getTooltipFontId();
  const bool stale = !overlayPrewarm_ || overlayPrewarmSpine_ != spineIndex || overlayPrewarmPage_ != pageIndex ||
                     overlayPrewarmFontId_ != fonts.body || overlayPrewarmOverlayFontId_ != overlayFont ||
                     overlayPrewarmGeneration_ != cache->cacheGeneration();
  if (!stale) return true;

  std::string overlayText;
  if (pageTranslation_.isActive()) {
    pageTranslation_.collectPageGlyphText(page, overlayText);
  } else {
    tooltip_.collectPageGlyphText(page, overlayText);
  }

  overlayPrewarm_.emplace(*cache);
  page.render(renderer, fonts, xOffset, yOffset);
  if (overlayFont == fonts.body && !overlayText.empty()) {
    cache->recordText(overlayText.c_str(), fonts.body, EpdFontFamily::REGULAR);
  }
  overlayPrewarm_->endScanAndPrewarm();
  if (overlayFont != fonts.body && !overlayText.empty()) {
    cache->prewarmCache(overlayFont, overlayText.c_str(), 0x01);
  }

  overlayPrewarmSpine_ = spineIndex;
  overlayPrewarmPage_ = pageIndex;
  overlayPrewarmFontId_ = fonts.body;
  overlayPrewarmOverlayFontId_ = overlayFont;
  overlayPrewarmGeneration_ = cache->cacheGeneration();
  return true;
}

void LinguaReaderIntegration::drawTransientUi(GfxRenderer& renderer) const {
  if (showNoTranslationsToast_) {
    GUI.drawWrappedPopup(renderer, tr(STR_NO_TRANSLATIONS_FOR_PAGE));
  }
}

bool LinguaReaderIntegration::prepareSection(Section& section) {
  if (SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_NORMAL) return false;
  if (!section.isTranslationPresenceKnown()) section.resolveTranslationPresence();
  if (section.hasTranslation()) return false;

  SETTINGS.translationDisplayMode = CrossPointSettings::LINGUA_NORMAL;
  SETTINGS.saveToFile();
  fallbackDialogActive_ = true;
  fallbackDialogDrawn_ = false;
  return true;
}

void LinguaReaderIntegration::drawFallbackDialog(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_LINGUA));

  const int maxTextWidth = std::max(1, screen.width - 2 * (metrics.popupMarginX + metrics.popupFrameThickness));
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const auto lines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_NO_TRANSLATION_SWITCH_NORMAL), maxTextWidth, 4);
  int textY = screen.y + (screen.height - static_cast<int>(lines.size()) * lineHeight) / 2;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_12_FONT_ID, textY, line.c_str(), true);
    textY += lineHeight;
  }

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  fallbackDialogDrawn_ = true;
}

void LinguaReaderIntegration::onPageChanged() {
  pageTranslation_.onPageChanged();
  tooltip_.onPageChanged();
  clearOverlayPrewarm();
}

void LinguaReaderIntegration::onSectionChanged(const std::string& translatedHtmlPath) {
  if (translatedHtmlPath_ == translatedHtmlPath) return;
  translatedHtmlPath_ = translatedHtmlPath;
  pageTranslation_.setTranslatedHtmlPath(translatedHtmlPath);
  pageTranslation_.onSectionChanged();
  tooltip_.setTranslatedHtmlPath(translatedHtmlPath);
  tooltip_.onPageChanged();
  clearOverlayPrewarm();
}

void LinguaReaderIntegration::onReaderExit() {
  clearOverlayPrewarm();
  pageTranslation_.onSectionChanged();
  tooltip_.onPageChanged();
  translatedHtmlPath_.clear();
}

void LinguaReaderIntegration::clearOverlayPrewarm() {
  overlayPrewarm_.reset();
  overlayPrewarmSpine_ = -1;
  overlayPrewarmPage_ = -1;
  overlayPrewarmFontId_ = -1;
  overlayPrewarmOverlayFontId_ = -1;
  overlayPrewarmGeneration_ = 0;
}
