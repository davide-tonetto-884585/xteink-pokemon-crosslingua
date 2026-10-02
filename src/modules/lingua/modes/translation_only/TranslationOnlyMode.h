#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
class TranslationOnlyMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_TRANSLATION_ONLY; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::TranslationOnly; }
  LinguaLayout layout() const override { return layoutValue(); }
};
