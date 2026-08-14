#pragma once

#include "CrossPointSettings.h"
#include "modules/lingua/layout/LinguaLayout.h"

class LinguaMode {
 public:
  virtual ~LinguaMode() = default;
  virtual CrossPointSettings::LINGUA_MODE id() const = 0;
  virtual LinguaLayout layout() const = 0;
};
