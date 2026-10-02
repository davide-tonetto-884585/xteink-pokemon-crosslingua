#include "modules/lingua/activities/LanguagePickerActivity.h"

#include <GfxRenderer.h>
#include <algorithm>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "modules/lingua/ui/LinguaTouch.h"

// Sentinel value used by SETTINGS.sourceTranslationLanguage to mean "auto-detect".
// Matches the encoding in CrossPointSettings.
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

const LanguagePickerActivity::Language LanguagePickerActivity::LANGUAGES[] = {
    {"Arabic", "ar"},
    {"Bulgarian", "bg"},
    {"Catalan", "ca"},
    {"Chinese (Simplified)", "zh-CN"},
    {"Chinese (Traditional)", "zh-TW"},
    {"Croatian", "hr"},
    {"Czech", "cs"},
    {"Danish", "da"},
    {"Dutch", "nl"},
    {"English", "en"},
    {"Estonian", "et"},
    {"Finnish", "fi"},
    {"French", "fr"},
    {"German", "de"},
    {"Greek", "el"},
    {"Hebrew", "he"},
    {"Hindi", "hi"},
    {"Hungarian", "hu"},
    {"Indonesian", "id"},
    {"Italian", "it"},
    {"Japanese", "ja"},
    {"Korean", "ko"},
    {"Latvian", "lv"},
    {"Lithuanian", "lt"},
    {"Malay", "ms"},
    {"Norwegian", "no"},
    {"Persian", "fa"},
    {"Polish", "pl"},
    {"Portuguese", "pt"},
    {"Romanian", "ro"},
    {"Russian", "ru"},
    {"Serbian", "sr"},
    {"Slovak", "sk"},
    {"Slovenian", "sl"},
    {"Spanish", "es"},
    {"Swedish", "sv"},
    {"Thai", "th"},
    {"Turkish", "tr"},
    {"Ukrainian", "uk"},
    {"Vietnamese", "vi"},
};
const int LanguagePickerActivity::NUM_LANGUAGES = static_cast<int>(sizeof(LANGUAGES) / sizeof(LANGUAGES[0]));

const char* LanguagePickerActivity::itemName(int idx) const {
  if (includeAutoDetect) {
    if (idx == 0) return tr(STR_AUTO_DETECT);
    return LANGUAGES[idx - 1].name;
  }
  return LANGUAGES[idx].name;
}

void LanguagePickerActivity::setInitialSelection(uint8_t initialSelection) {
  if (includeAutoDetect) {
    if (initialSelection == AUTO_DETECT_SENTINEL) {
      selectedIndex = 0;  // Auto-detect entry sits at visible index 0
      return;
    }
    // Concrete language: shift by +1 because Auto-detect occupies index 0
    const int target = static_cast<int>(initialSelection) + 1;
    selectedIndex = (target >= 0 && target < itemCount()) ? target : 0;
    return;
  }
  // No auto-detect: visible index == LANGUAGES[] index
  selectedIndex = (initialSelection < NUM_LANGUAGES) ? static_cast<int>(initialSelection) : 0;
}

int LanguagePickerActivity::resultIndexFor(int visibleIdx) const {
  if (includeAutoDetect) {
    if (visibleIdx == 0) return AUTO_DETECT_SENTINEL;
    return visibleIdx - 1;
  }
  return visibleIdx;
}

int LanguagePickerActivity::visibleRowCount() const {
  return (renderer.getScreenHeight() - LIST_START_Y - 40) / LINE_H;
}

int LanguagePickerActivity::firstVisibleIndex() const {
  const int count = itemCount();
  const int visibleRows = visibleRowCount();
  int startIdx = selectedIndex - visibleRows / 2;
  if (startIdx < 0) startIdx = 0;
  if (startIdx + visibleRows > count) startIdx = count - visibleRows;
  if (startIdx < 0) startIdx = 0;
  return startIdx;
}

void LanguagePickerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void LanguagePickerActivity::onExit() { Activity::onExit(); }

void LanguagePickerActivity::loop() {
  const int count = itemCount();
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });

  // Touch (X4 Pro): swipe pages the list, a tap on a row picks it, a tap on the title or the bottom
  // hint strip goes back.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int page = std::max(1, visibleRowCount());
    selectedIndex = swipe == MappedInputManager::SwipeDir::Up ? std::min(count - 1, selectedIndex + page)
                                                              : std::max(0, selectedIndex - page);
    requestUpdate();
    return;
  }
  int tapY = 0;
  const LinguaTouch::Tap tap = LinguaTouch::readTap(mappedInput, renderer, nullptr, &tapY);
  if (tap == LinguaTouch::Tap::Confirm) {
    if (tapY < LIST_START_Y) {
      ActivityResult cancelled;
      cancelled.isCancelled = true;
      cancelled.data = MenuResult{-1, 0, 0};
      setResult(std::move(cancelled));
      finish();
      return;
    }
    const int row = (tapY - LIST_START_Y) / LINE_H;
    const int idx = firstVisibleIndex() + row;
    if (row < visibleRowCount() && idx < count) {
      setResult(MenuResult{resultIndexFor(idx), 0, 0});
      finish();
      return;
    }
  }
  if (tap == LinguaTouch::Tap::Back) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    cancelled.data = MenuResult{-1, 0, 0};
    setResult(std::move(cancelled));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(MenuResult{resultIndexFor(selectedIndex), 0, 0});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    cancelled.data = MenuResult{-1, 0, 0};
    setResult(std::move(cancelled));
    finish();
    return;
  }
}

void LanguagePickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const char* title = customTitle ? customTitle : tr(STR_TARGET_LANGUAGE);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title, true, EpdFontFamily::BOLD);

  // Scrolling list - show a window of entries centred on the selection
  const int startY = LIST_START_Y;
  const int count = itemCount();
  const int visibleRows = visibleRowCount();
  const int startIdx = firstVisibleIndex();

  for (int row = 0; row < visibleRows && startIdx + row < count; row++) {
    const int idx = startIdx + row;
    const int y = startY + row * LINE_H;
    const bool sel = (idx == selectedIndex);

    if (sel) renderer.fillRect(0, y, pageWidth - 1, LINE_H, true);
    renderer.drawText(UI_10_FONT_ID, 20, y, itemName(idx), !sel);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
