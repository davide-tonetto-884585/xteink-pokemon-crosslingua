#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
class InterlinearMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_INTERLINEAR; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::Interlinear; }
  LinguaLayout layout() const override { return layoutValue(); }
  bool translationVisible() const { return translationVisible_; }
  void toggleTranslation() { translationVisible_ = !translationVisible_; }

 private:
  bool translationVisible_ = true;
};
