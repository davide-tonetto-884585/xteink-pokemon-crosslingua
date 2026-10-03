#include "modules/recap/GeminiRecap.h"

#include <ArduinoJson.h>

#include <cstring>

namespace recap {

std::string geminiEndpoint(const char* model) {
  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += model;
  url += ":generateContent";
  return url;
}

std::string buildGeminiRequestBody(const RecapRequest& request) {
  std::string instructions =
      "You help a reader get back into the book they are reading. The user message is the text they "
      "read most recently in the current chapter";
  instructions += request.startsMidChapter ? " (it starts partway through the chapter)." : ".";
  if (request.bookTitle && request.bookTitle[0]) {
    instructions += " Book: \"";
    instructions += request.bookTitle;
    instructions += "\".";
  }
  if (request.chapterTitle && request.chapterTitle[0]) {
    instructions += " Chapter: \"";
    instructions += request.chapterTitle;
    instructions += "\".";
  }
  instructions += " Write a concise recap of that text in ";
  instructions += (request.languageName && request.languageName[0]) ? request.languageName : "English";
  instructions +=
      ", whatever language the book is in. Cover the key events, who is involved and where things stand at "
      "the end, so the reader can pick up where they left off. Only use what is in the text: do not invent "
      "events and do not reveal anything that comes later in the book. Plain text only, no Markdown, no "
      "headings, no bullet lists: 2 to 4 short paragraphs, at most about 250 words.";

  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = instructions;
  JsonObject content = doc["contents"].add<JsonObject>();
  content["role"] = "user";
  content["parts"][0]["text"] = request.text;
  if (request.lowThinking) {
    doc["generationConfig"]["thinkingConfig"]["thinkingLevel"] = "low";
  }

  std::string body;
  serializeJson(doc, body);
  return body;
}

RecapParseStatus parseGeminiResponse(const std::string& json, std::string& outText) {
  outText.clear();
  JsonDocument doc;
  if (deserializeJson(doc, json)) return RecapParseStatus::Invalid;
  if (!doc["promptFeedback"]["blockReason"].isNull()) return RecapParseStatus::Blocked;

  JsonVariantConst candidate = doc["candidates"][0];
  if (candidate.isNull()) return RecapParseStatus::Empty;
  for (JsonVariantConst part : candidate["content"]["parts"].as<JsonArrayConst>()) {
    if (part["thought"] | false) continue;
    const char* text = part["text"] | "";
    outText += text;
  }
  stripMarkdown(outText);
  // Trim surrounding whitespace.
  const size_t first = outText.find_first_not_of(" \n\r\t");
  if (first == std::string::npos) {
    outText.clear();
    const char* finish = candidate["finishReason"] | "";
    return std::strcmp(finish, "SAFETY") == 0 || std::strcmp(finish, "PROHIBITED_CONTENT") == 0
               ? RecapParseStatus::Blocked
               : RecapParseStatus::Empty;
  }
  const size_t last = outText.find_last_not_of(" \n\r\t");
  outText = outText.substr(first, last - first + 1);
  return RecapParseStatus::Ok;
}

std::string parseGeminiError(const std::string& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return {};
  const char* message = doc["error"]["message"] | "";
  return message;
}

void stripMarkdown(std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool lineStart = true;
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (lineStart) {
      // Drop heading markers and turn "* " / "- " bullets into plain lines.
      size_t j = i;
      while (j < text.size() && text[j] == '#') j++;
      if (j > i && j < text.size() && text[j] == ' ') {
        i = j;
        continue;
      }
      if ((c == '*' || c == '-') && i + 1 < text.size() && text[i + 1] == ' ') {
        i++;
        lineStart = false;
        continue;
      }
    }
    if ((c == '*' || c == '_') && i + 1 < text.size() && text[i + 1] == c) {
      i++;
      lineStart = false;
      continue;
    }
    out += c;
    lineStart = c == '\n';
  }
  text = std::move(out);
}

}  // namespace recap
