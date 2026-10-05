#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Chapter recap: Gemini API (Google AI Studio) request/response handling.
 *
 * Pure string-in/string-out (ArduinoJson only) so the native tests can cover it; the HTTP call
 * itself lives in ChapterRecapActivity. The API key travels in the x-goog-api-key header, never in
 * the URL, so it cannot leak into a logged request line.
 */
namespace recap {

// Tried in order: when a model is unavailable for this key/project (404), rate limited (429) or
// overloaded (5xx), the next one is used. The free tier gives each model its own quota, so falling
// back to Flash-Lite keeps the feature usable after Flash's per-minute limit is hit.
inline constexpr const char* GEMINI_MODELS[] = {
    "gemini-3-flash-preview",
    "gemini-3.1-flash-lite",
    "gemini-flash-latest",
};
inline constexpr size_t GEMINI_MODEL_COUNT = sizeof(GEMINI_MODELS) / sizeof(GEMINI_MODELS[0]);

// Chapter summaries are many short, simple requests (one per chapter the first time a book is
// used): Flash-Lite first spares the scarcer Flash quota for the answer the reader is waiting for.
inline constexpr const char* GEMINI_SUMMARY_MODELS[] = {
    "gemini-3.1-flash-lite",
    "gemini-3-flash-preview",
    "gemini-flash-latest",
};
inline constexpr size_t GEMINI_SUMMARY_MODEL_COUNT = sizeof(GEMINI_SUMMARY_MODELS) / sizeof(GEMINI_SUMMARY_MODELS[0]);

inline constexpr const char* GEMINI_API_KEY_HEADER = "x-goog-api-key";

struct RecapRequest {
  const std::string& text;
  // Native name of the language the recap must be written in (the device UI language).
  const char* languageName = "English";
  const char* bookTitle = "";
  const char* chapterTitle = "";
  // The excerpt was cut at the front (it starts mid-chapter, not at the chapter's beginning).
  bool startsMidChapter = false;
  // Ask Gemini 3 for low thinking: a recap needs no deep reasoning and this keeps latency low.
  // Dropped on a retry if a model rejects the field with HTTP 400.
  bool lowThinking = true;
};

std::string geminiEndpoint(const char* model);
std::string buildGeminiRequestBody(const RecapRequest& request);

enum class RecapParseStatus : uint8_t { Ok, Blocked, Empty, Invalid };

// Extracts the recap text (all non-thought parts of the first candidate, Markdown emphasis
// stripped) from a generateContent response.
RecapParseStatus parseGeminiResponse(const std::string& json, std::string& outText);

// The `error.message` of a Gemini error body, or empty if there is none.
std::string parseGeminiError(const std::string& json);

// Whether a failed HTTP status is worth retrying on the next model in GEMINI_MODELS.
constexpr bool shouldTryNextModel(const int httpCode) {
  return httpCode == 404 || httpCode == 429 || httpCode == 500 || httpCode == 503 || httpCode == 504;
}

// ─── Book assistant (context = chapter summaries + current chapter) ─────────

// What the assistant is asked to do with the book-so-far context.
enum class AssistantTask : uint8_t {
  Characters,  // main characters met so far
  Question,    // the reader's free-form question
  WhoIs,       // who or what a name/term selected on the page is
};

struct AssistantPrompt {
  AssistantTask task = AssistantTask::Question;
  const char* languageName = "English";
  const char* bookTitle = "";
  // Question text (Question) or the selected name/term (WhoIs); unused for Characters.
  const char* subject = "";
};

// System instruction for an assistant request: what the context is, the no-spoiler rule, the
// output language and format.
std::string assistantInstructions(const AssistantPrompt& prompt);
// The request line appended after the context, at the end of the user message.
std::string assistantRequestLine(const AssistantPrompt& prompt);

// System instruction for summarizing one chapter (cached on the SD card, reused by every later
// assistant request on the book).
std::string chapterSummaryInstructions(const char* languageName, const char* bookTitle, const char* chapterTitle);

// Context section labels written into the user message (English: they are for the model).
inline constexpr const char* CONTEXT_SUMMARIES_HEADER =
    "Summaries of the parts of the book the reader has already finished, in reading order:\n\n";
inline constexpr const char* CONTEXT_NO_SUMMARIES = "(The reader is in the first chapter of the book.)\n\n";
std::string contextSummaryHeading(int partNumber, const char* title);
std::string contextCurrentChapterHeading(const char* title, bool startsMidChapter);

// Streaming request body: prefix + JSON-escaped user text (written in any number of chunks) +
// suffix. Lets a body of hundreds of KB be written to and sent from the SD card.
std::string streamingBodyPrefix(const std::string& instructions, bool lowThinking);
const char* streamingBodySuffix();
// Appends `data` JSON-string-escaped. Chunk boundaries may split UTF-8 sequences: only ASCII bytes
// are ever escaped, so the output is the same as escaping the whole text at once.
void appendJsonEscaped(std::string& out, const char* data, size_t len);

// Removes the Markdown the UI cannot render (**bold**, __bold__, # headings, "* " bullets) so the
// recap reads as plain text on the e-ink screen.
void stripMarkdown(std::string& text);

}  // namespace recap
