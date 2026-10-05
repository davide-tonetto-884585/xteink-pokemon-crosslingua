// Host-side unit test for Lingua's decoding of HTML entities in translation-engine output.
// Built and run via the CMake native-test harness (test/CMakeLists.txt ->
// add_subdirectory(html_entity_decode)); its own main() returns non-zero on failure.

#include <cstdio>
#include <string>

#include "modules/lingua/utils/HtmlEntityDecode.h"

static int g_failures = 0;
static int g_checks = 0;

static std::string decoded(std::string s) {
  lingua::decodeHtmlEntitiesInPlace(s);
  return s;
}

static void expectEq(const char* name, const std::string& got, const std::string& want) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("FAIL %-32s got=\"%s\" want=\"%s\"\n", name, got.c_str(), want.c_str());
  }
}

int main() {
  // What Google's translateHtml endpoint actually returns.
  expectEq("google_apostrophe", decoded("se n&#39;è andata"), "se n'è andata");
  expectEq("google_quotes", decoded("ha detto &quot;ciao&quot;"), "ha detto \"ciao\"");
  expectEq("google_escaped_markup", decoded("&lt;presto&gt; A&amp;B"), "<presto> A&B");

  // Numeric forms.
  expectEq("hex_lower", decoded("dell&#x27;alba"), "dell'alba");
  expectEq("hex_upper", decoded("dell&#X2019;alba"), "dell\xE2\x80\x99"
                                                      "alba");
  expectEq("decimal_multibyte", decoded("&#8220;due&#8221;"), "\xE2\x80\x9C"
                                                              "due\xE2\x80\x9D");
  expectEq("supplementary_plane", decoded("&#128512;"), "\xF0\x9F\x98\x80");

  // Named forms from the shared EPUB table.
  expectEq("named_quotes", decoded("&lsquo;uno&rsquo; &laquo;tre&raquo;"), "\xE2\x80\x98uno\xE2\x80\x99 \xC2\xABtre\xC2\xBB");
  expectEq("named_nbsp", decoded("x&nbsp;y"), "x\xC2\xA0y");
  expectEq("named_apos", decoded("l&apos;alba"), "l'alba");

  // Plain text that merely contains '&' passes through unchanged.
  expectEq("no_entities", decoded("dell'alba \xE2\x80\x94 caff\xC3\xA8"), "dell'alba \xE2\x80\x94 caff\xC3\xA8");
  expectEq("bare_ampersand", decoded("AT&T and a & b"), "AT&T and a & b");
  expectEq("unknown_name", decoded("&bogus; &amp"), "&bogus; &amp");
  expectEq("no_semicolon_far", decoded("&quotxxxxxxxxxxxxxxxxx;"), "&quotxxxxxxxxxxxxxxxxx;");
  expectEq("trailing_ampersand", decoded("fine &"), "fine &");

  // Invalid numeric entities are left alone.
  expectEq("zero", decoded("&#0;"), "&#0;");
  expectEq("surrogate", decoded("&#xD800;"), "&#xD800;");
  expectEq("too_large", decoded("&#x110000;"), "&#x110000;");
  expectEq("empty_numeric", decoded("&#; &#x;"), "&#; &#x;");
  expectEq("bad_digit", decoded("&#12a;"), "&#12a;");

  // Single pass: decoded output is not re-scanned.
  expectEq("no_double_decode", decoded("&amp;quot;"), "&quot;");

  if (g_failures == 0) {
    std::printf("OK: all %d checks passed\n", g_checks);
    return 0;
  }
  std::printf("FAILED: %d/%d checks failed\n", g_failures, g_checks);
  return 1;
}
