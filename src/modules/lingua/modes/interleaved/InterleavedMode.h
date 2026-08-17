#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
class InterleavedMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_INTERLEAVED; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::Both; }
  static constexpr uint8_t translatedWordInk(CrossPointSettings::LINGUA_MODE mode, uint8_t shade) {
    if (mode != CrossPointSettings::LINGUA_INTERLEAVED) return 0;
    return shade == CrossPointSettings::SHADE_DIMMED_LIGHT ? 2 : 1;
  }
  LinguaLayout layout() const override { return layoutValue(); }
};
