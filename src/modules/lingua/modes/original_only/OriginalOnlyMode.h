#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
class OriginalOnlyMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_ORIGINAL_ONLY; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::OriginalOnly; }
  LinguaLayout layout() const override { return layoutValue(); }
};
