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

// Removes the Markdown the UI cannot render (**bold**, __bold__, # headings, "* " bullets) so the
// recap reads as plain text on the e-ink screen.
void stripMarkdown(std::string& text);

}  // namespace recap
