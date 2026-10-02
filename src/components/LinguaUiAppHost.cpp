#include "LinguaUiAppHost.h"

#include "UiAppHelpers.h"

namespace fui = freeink::ui;

LinguaUiAppHost::LinguaUiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void LinguaUiAppHost::resetUi() {
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
}

void LinguaUiAppHost::renderUi() {
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
}

LinguaUiAppHost::TouchRoute LinguaUiAppHost::routeTouch(const MappedInputManager& input, const bool withLongPress,
                                                        const bool routeHeld) {
  TouchRoute result;
  if (!uiReady) return result;
  result.snap = touchSnapshotFrom(input);
  if (!withLongPress && result.snap.longPress) {
    // Callers that did not opt into long-press get it as a plain tap release.
    result.snap.longPress = false;
  }
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }
  result.routed = true;
  result.event = app.route(result.snap);
  return result;
}

fui::ActionEvent LinguaUiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  return app.route(snap);
}
