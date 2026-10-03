// Host-side unit test for the chapter recap module (text reconstruction + Gemini request/response).
// Own main() with a tiny TEST/EXPECT shim (no GTest), like TextNormalizeTest: returns non-zero on
// failure so CTest reports pass/fail.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "modules/recap/GeminiRecap.h"
#include "modules/recap/RecapText.h"

namespace {
int g_failures = 0;
int g_checks = 0;
std::vector<std::pair<const char*, void (*)()>>& tests() {
  static std::vector<std::pair<const char*, void (*)()>> all;
  return all;
}
struct Registrar {
  Registrar(const char* name, void (*fn)()) { tests().emplace_back(name, fn); }
};
void check(const bool ok, const char* expr, const int line) {
  g_checks++;
  if (!ok) {
    g_failures++;
    std::printf("FAIL line %d: %s\n", line, expr);
  }
}
}  // namespace

#define TEST(suite, name)                                          \
  static void suite##_##name();                                    \
  static const Registrar suite##_##name##_reg(#suite "." #name, &suite##_##name); \
  static void suite##_##name()
#define EXPECT_TRUE(x) check(static_cast<bool>(x), #x, __LINE__)
#define EXPECT_FALSE(x) check(!(x), "!(" #x ")", __LINE__)
#define EXPECT_EQ(a, b) check((a) == (b), #a " == " #b, __LINE__)
#define EXPECT_NE(a, b) check((a) != (b), #a " != " #b, __LINE__)

namespace {
void add(recap::RecapTextBuilder& b, const char* word, bool spaceBefore = true, bool hyphen = false) {
  b.addWord(word, std::strlen(word), spaceBefore, hyphen);
}
}  // namespace

TEST(RecapTextBuilder, JoinsWordsLinesAndPagesWithSingleSpaces) {
  recap::RecapTextBuilder b;
  add(b, "The", false);
  add(b, "cat");
  b.endLine();
  add(b, "sat", false);
  b.endPage();
  add(b, "down.", false);
  EXPECT_EQ(b.take(), "The cat sat down.");
}

TEST(RecapTextBuilder, RejoinsWordsSplitByInsertedHyphen) {
  recap::RecapTextBuilder b;
  add(b, "a", false);
  add(b, "won-", true, true);
  b.endLine();
  add(b, "derful", false);
  add(b, "day");
  EXPECT_EQ(b.take(), "a wonderful day");
}

TEST(RecapTextBuilder, KeepsAuthorHyphensAndAttachedPunctuation) {
  recap::RecapTextBuilder b;
  add(b, "well-", false);  // real hyphen at a line end, not inserted by the line breaker
  b.endLine();
  add(b, "known", false);
  add(b, "\xe2\x80\x9d", false);  // closing quote laid out as its own run, no space before
  EXPECT_EQ(b.take(), "well- known\xe2\x80\x9d");
}

TEST(RecapTextBuilder, EmSpaceIndentStartsNewParagraph) {
  recap::RecapTextBuilder b;
  add(b, "End.", false);
  b.endLine();
  add(b, "\xe2\x80\x83Next", false);
  add(b, "one.");
  EXPECT_EQ(b.take(), "End.\nNext one.");
}

TEST(RecapTextBuilder, LeadingIndentOnFirstWordAddsNoBlankLine) {
  recap::RecapTextBuilder b;
  add(b, "\xe2\x80\x83Start", false);
  EXPECT_EQ(b.take(), "Start");
}

TEST(RecapKeepTail, CutsAtWordBoundary) {
  std::string text = "alpha beta gamma delta";
  EXPECT_TRUE(recap::keepTail(text, 9));
  EXPECT_EQ(text, "delta");
}

TEST(RecapKeepTail, NoOpWhenShortEnough) {
  std::string text = "short";
  EXPECT_FALSE(recap::keepTail(text, 10));
  EXPECT_EQ(text, "short");
}

TEST(RecapKeepTail, NeverSplitsUtf8WithoutSpaces) {
  // 4 x U+4E00 (3 bytes each) with no whitespace.
  std::string text = "\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80";
  EXPECT_TRUE(recap::keepTail(text, 7));
  EXPECT_EQ(text, "\xe4\xb8\x80\xe4\xb8\x80");
}

TEST(GeminiRecap, EndpointHasModelAndNoKey) {
  EXPECT_EQ(recap::geminiEndpoint("gemini-3-flash-preview"),
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-3-flash-preview:generateContent");
}

TEST(GeminiRecap, RequestBodyEscapesTextAndCarriesLanguage) {
  const std::string text = "He said \"hi\"\nthen left\\.";
  recap::RecapRequest request{text};
  request.languageName = "Italiano";
  request.bookTitle = "Book";
  request.startsMidChapter = true;
  const std::string body = recap::buildGeminiRequestBody(request);
  EXPECT_NE(body.find("He said \\\"hi\\\"\\nthen left\\\\."), std::string::npos);
  EXPECT_NE(body.find("in Italiano"), std::string::npos);
  EXPECT_NE(body.find("partway through"), std::string::npos);
  EXPECT_NE(body.find("\"thinkingLevel\":\"low\""), std::string::npos);

  request.lowThinking = false;
  EXPECT_EQ(recap::buildGeminiRequestBody(request).find("thinkingConfig"), std::string::npos);
}

TEST(GeminiRecap, ParsesTextPartsSkippingThoughts) {
  const std::string json = R"({"candidates":[{"content":{"parts":[
      {"text":"thinking...","thought":true},
      {"text":"  **Marco** arriva.\n"},
      {"text":"Poi parte."}]},"finishReason":"STOP"}]})";
  std::string out;
  EXPECT_EQ(recap::parseGeminiResponse(json, out), recap::RecapParseStatus::Ok);
  EXPECT_EQ(out, "Marco arriva.\nPoi parte.");
}

TEST(GeminiRecap, ReportsBlockedAndEmptyAndInvalid) {
  std::string out;
  EXPECT_EQ(recap::parseGeminiResponse(R"({"promptFeedback":{"blockReason":"SAFETY"}})", out),
            recap::RecapParseStatus::Blocked);
  EXPECT_EQ(recap::parseGeminiResponse(R"({"candidates":[{"finishReason":"SAFETY"}]})", out),
            recap::RecapParseStatus::Blocked);
  EXPECT_EQ(recap::parseGeminiResponse(R"({"candidates":[]})", out), recap::RecapParseStatus::Empty);
  EXPECT_EQ(recap::parseGeminiResponse("not json", out), recap::RecapParseStatus::Invalid);
}

TEST(GeminiRecap, ParsesErrorMessage) {
  EXPECT_EQ(recap::parseGeminiError(R"({"error":{"code":400,"message":"API key not valid."}})"),
            "API key not valid.");
  EXPECT_EQ(recap::parseGeminiError("<html>"), "");
}

TEST(GeminiRecap, StripMarkdownRemovesHeadingsBulletsAndEmphasis) {
  std::string text = "## Riassunto\n* Primo __punto__\n- Secondo\nNormale - trattino";
  recap::stripMarkdown(text);
  EXPECT_EQ(text, "Riassunto\nPrimo punto\nSecondo\nNormale - trattino");
}

TEST(GeminiRecap, RetriesOnlyOnAvailabilityErrors) {
  EXPECT_TRUE(recap::shouldTryNextModel(429));
  EXPECT_TRUE(recap::shouldTryNextModel(404));
  EXPECT_TRUE(recap::shouldTryNextModel(503));
  EXPECT_FALSE(recap::shouldTryNextModel(400));
  EXPECT_FALSE(recap::shouldTryNextModel(403));
}

int main() {
  for (const auto& [name, fn] : tests()) {
    const int before = g_failures;
    fn();
    if (g_failures != before) std::printf("  in %s\n", name);
  }
  if (g_failures == 0) {
    std::printf("OK: all %d checks passed\n", g_checks);
    return 0;
  }
  std::printf("FAILED: %d/%d checks failed\n", g_failures, g_checks);
  return 1;
}
