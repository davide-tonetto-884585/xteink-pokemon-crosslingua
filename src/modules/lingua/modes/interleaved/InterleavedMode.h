#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
class InterleavedMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_INTERLEAVED; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::Both; }
  LinguaLayout layout() const override { return layoutValue(); }
};
