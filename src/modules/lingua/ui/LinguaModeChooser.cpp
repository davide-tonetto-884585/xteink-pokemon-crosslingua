#include "modules/lingua/ui/LinguaModeChooser.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "modules/lingua/LinguaModeCatalog.h"

namespace fui = freeink::ui;

namespace {
// Rows are the selectable Lingua display modes, so the item array is fixed-size and built once.
fui::ListItem modeRows[LINGUA_SELECTABLE_MODE_COUNT]{};

void ensureModeRows() {
  if (modeRows[0].label != nullptr) return;
  for (size_t i = 0; i < LINGUA_SELECTABLE_MODE_COUNT; i++) {
    modeRows[i].label = I18N.get(linguaModeLabel(LINGUA_SELECTABLE_MODES[i]));
    modeRows[i].actionValue = static_cast<int16_t>(i);
  }
}
}  // namespace

LinguaModeChooser::LinguaModeChooser(const GfxRenderer& renderer) : renderer(renderer), host(renderer) {}

void LinguaModeChooser::screenTrampoline(UiAppHost::UiScreen& screen, void* user) {
  static_cast<LinguaModeChooser*>(user)->buildScreen(screen);
}

void LinguaModeChooser::rowTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LinguaModeChooser*>(user);
  if (event.value < 0 || event.value >= static_cast<int>(LINGUA_SELECTABLE_MODE_COUNT)) return;
  self->tappedRow = event.value;
}

void LinguaModeChooser::begin(const int selected) {
  ensureModeRows();
  // Labels come from the I18N table, which a language change swaps under us; refresh on entry.
  for (size_t i = 0; i < LINGUA_SELECTABLE_MODE_COUNT; i++) {
    modeRows[i].label = I18N.get(linguaModeLabel(LINGUA_SELECTABLE_MODES[i]));
  }
  nav.reset(selected);
  tappedRow = -1;
  host.resetUi();
  host.app.on(ACTION_ROW, &LinguaModeChooser::rowTrampoline, this);
  host.app.setScreen(&LinguaModeChooser::screenTrampoline, this);
}

LinguaModeChooser::Result LinguaModeChooser::handleInput(MappedInputManager& input) {
  hasTouch = input.hasTouch();

  constexpr int count = static_cast<int>(LINGUA_SELECTABLE_MODE_COUNT);
  if (input.wasReleased(MappedInputManager::Button::Up)) {
    nav.selected = (nav.selected + count - 1) % count;
    nav.follow(count);
    return Result::Redraw;
  }
  if (input.wasReleased(MappedInputManager::Button::Down)) {
    nav.selected = (nav.selected + 1) % count;
    nav.follow(count);
    return Result::Redraw;
  }
  if (input.wasReleased(MappedInputManager::Button::Confirm)) return Result::Picked;
  if (input.wasReleased(MappedInputManager::Button::Back)) return Result::Cancelled;

  // Touch: a tap on a row selects and activates it in one go, like every FreeInkUI list.
  const auto route = host.routeTouch(input);
  if (route.routed && tappedRow >= 0) {
    nav.selected = tappedRow;
    tappedRow = -1;
    host.app.clearTapFlash();
    return Result::Picked;
  }
  if (route.routed && host.app.invalidated()) return Result::Redraw;

  const auto swipe = input.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? nav.pageRows() : -nav.pageRows();
    if (nav.scrollBy(delta, count)) return Result::Redraw;
  }
  return Result::None;
}

void LinguaModeChooser::buildScreen(UiAppHost::UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/false);
  // Content: the safe area minus the header band render() paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = modeRows;
  props.count = static_cast<uint16_t>(LINGUA_SELECTABLE_MODE_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleInput()
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;

  int16_t rowHeight = screen.theme().rowHeight;
  if (!hasTouch) {
    // Non-touch hardware keeps the denser per-theme row height, as UiListActivity does.
    rowHeight = static_cast<int16_t>(metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  nav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap,
                  static_cast<int>(LINGUA_SELECTABLE_MODE_COUNT), props);
  screen.list(props);
}

void LinguaModeChooser::render(GfxRenderer& renderer, MappedInputManager& input) {
  hasTouch = input.hasTouch();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/false);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_CHOOSE_DISPLAY_MODE));
  host.renderUi();
  // Wrapped labels can shrink the page; the nav asks for another build when the selection fell
  // outside what was drawn. Bounded, exactly as UiListActivity::render().
  for (int pass = 0; nav.consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                   tr(STR_CHOOSE_DISPLAY_MODE));
    host.renderUi();
  }

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
