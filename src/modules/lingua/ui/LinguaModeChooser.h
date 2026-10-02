#pragma once

#include <FreeInkApp.h>

#include "components/LinguaUiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// The "choose display mode" screen the chapter/book translators show once a translation finishes.
//
// Both translators are state machines, not single-list screens, so they cannot derive from
// UiListActivity (see the note in UiAppHost.h). They hold this instead: it owns a FreeInkApp for
// the one state that IS a list, and the activity keeps its own lifecycle. Shared by both so the
// chooser exists once.
class LinguaModeChooser {
 public:
  explicit LinguaModeChooser(const GfxRenderer& renderer);

  // What one input pass did. The caller owns what happens next: Picked means read selected() and
  // apply it, Cancelled means leave the screen unchanged.
  enum class Result : uint8_t { None, Redraw, Picked, Cancelled };

  // Entering the chooser state: rebind the theme, register the row action, open on `selected`.
  void begin(int selected);
  int selected() const { return nav.selected; }

  Result handleInput(MappedInputManager& input);
  // Header, list and button hints, flushed. Call from the activity's render().
  void render(GfxRenderer& renderer, MappedInputManager& input);

 private:
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;

  static void screenTrampoline(LinguaUiAppHost::UiScreen& screen, void* user);
  static void rowTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void buildScreen(LinguaUiAppHost::UiScreen& screen);

  const GfxRenderer& renderer;
  LinguaUiAppHost host;
  freeink::ui::ListNav nav;
  // Set by the row trampoline when a tap lands on a row; drained by handleInput().
  int tappedRow = -1;
  bool hasTouch = false;
};
