#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
class SideBySideMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_SIDE_BY_SIDE; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::SideBySide; }
  LinguaLayout layout() const override { return layoutValue(); }
};
