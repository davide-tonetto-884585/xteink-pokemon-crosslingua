#pragma once
#include <Epub/PageFontSet.h>

#include "modules/lingua/modes/common/LinguaMode.h"
class SideBySideMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_SIDE_BY_SIDE; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::SideBySide; }
  static constexpr uint8_t translationInk(CrossPointSettings::LINGUA_MODE mode, uint8_t shade) {
    return mode == CrossPointSettings::LINGUA_SIDE_BY_SIDE ? shade : PageFontSet::INK_INHERIT;
  }
  LinguaLayout layout() const override { return layoutValue(); }
};
