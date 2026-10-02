#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

class GfxRenderer;
class MappedInputManager;

// CrossLingua: the non-template FreeInkApp host the Lingua screens (UiListActivity,
// LinguaModeChooser) were written against. CrossInk's own UiAppHost is a capacity template; this
// keeps the Lingua module's API intact on top of the same shared helpers (UiAppHelpers.h).
class LinguaUiAppHost {
 public:
  using UiApp = freeink::ui::FreeInkApp<24, 6>;
  using UiScreen = UiApp::ScreenType;

  explicit LinguaUiAppHost(const GfxRenderer& renderer);

  // Screen-entry reset: close the routing gate and rebind the shared theme tokens.
  void resetUi();
  // Render the app and open the routing gate after the first publish.
  void renderUi();

  struct TouchRoute {
    freeink::ui::ActionEvent event{};
    freeink::ui::InputSnapshot snap{};
    bool routed = false;
    explicit operator bool() const { return static_cast<bool>(event); }
  };

  // Gated snapshot-build + route. withLongPress keeps the SDK long-press; routeHeld forwards held
  // frames for InputDrag elements.
  TouchRoute routeTouch(const MappedInputManager& input, bool withLongPress = false, bool routeHeld = false);
  freeink::ui::ActionEvent route(const freeink::ui::InputSnapshot& snap);

  void closeRouting() { uiReady = false; }
  bool routingReady() const { return uiReady.load(); }

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;

 private:
  std::atomic<bool> uiReady{false};
};
