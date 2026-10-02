#pragma once

#include "components/themes/minimal/MinimalTheme.h"

// The Bookshelf theme is the Minimal theme everywhere except Home. Home keeps
// three recent books in memory (the one at the top plus the face-on cover on
// the first shelf).
namespace BookshelfMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeRecentBooksCount = 3;
  return v;
}

constexpr ThemeMetrics values = makeValues();
}  // namespace BookshelfMetrics
