#pragma once

// Minimal stand-in for lib/I18n. The real header pulls in I18nKeys.h / I18nStrings.h, which are
// generated at firmware build time and gitignored, so the host test cannot use it. Only
// SideBySideLayout needs a string here, and the layout under test never depends on its content.
enum StrId { STR_NO_TRANSLATION = 0 };

inline const char* tr(StrId) { return "[not translated]"; }
