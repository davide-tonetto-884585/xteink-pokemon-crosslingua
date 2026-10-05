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

// ─── Book assistant ──────────────────────────────────────────────────────────

namespace {
void appendQuoted(std::string& out, const char* label, const char* value) {
  if (!value || !value[0]) return;
  out += label;
  out += " \"";
  out += value;
  out += "\".";
}

const char* languageOrEnglish(const char* languageName) {
  return (languageName && languageName[0]) ? languageName : "English";
}
}  // namespace

std::string assistantInstructions(const AssistantPrompt& prompt) {
  std::string out =
      "You are a reading companion for a reader who is partway through a book. The user message contains "
      "summaries of the parts of the book they have already finished, then the full text of the chapter they "
      "are reading, from its beginning up to where they stopped, then their request.";
  appendQuoted(out, " Book:", prompt.bookTitle);
  out +=
      " That material is everything the reader knows. Answer only from it: never reveal, hint at or confirm "
      "anything that happens after the reader's position, even if you know the book. If the material does not "
      "contain the answer, say so plainly instead of guessing. Write in ";
  out += languageOrEnglish(prompt.languageName);
  out += ", whatever language the book is in. Plain text only: no Markdown, no headings, no bullet symbols. ";
  switch (prompt.task) {
    case AssistantTask::Characters:
      out +=
          "List the main characters the reader has met so far, most important first, at most 12. One paragraph "
          "per character: the name, a colon, then who they are, how they relate to the other characters and "
          "where they stand at the reader's position. Keep each paragraph to 2-3 sentences.";
      break;
    case AssistantTask::Question:
      out += "Answer the reader's question directly and concisely, in at most about 200 words.";
      break;
    case AssistantTask::WhoIs:
      out +=
          "Explain who or what the term the reader selected is (a character, place, object, group or idea) and "
          "its role in the story up to the reader's position, in at most about 150 words. If it is a character, "
          "mention their relationships with the other characters.";
      break;
  }
  return out;
}

std::string assistantRequestLine(const AssistantPrompt& prompt) {
  switch (prompt.task) {
    case AssistantTask::Characters:
      return "Request: list the main characters so far.";
    case AssistantTask::Question: {
      std::string line = "The reader's question: ";
      line += prompt.subject ? prompt.subject : "";
      return line;
    }
    case AssistantTask::WhoIs: {
      std::string line = "Request: who or what is \"";
      line += prompt.subject ? prompt.subject : "";
      line += "\"?";
      return line;
    }
  }
  return {};
}

std::string chapterSummaryInstructions(const char* languageName, const char* bookTitle, const char* chapterTitle) {
  std::string out = "The user message is one chapter of a book.";
  appendQuoted(out, " Book:", bookTitle);
  appendQuoted(out, " Chapter:", chapterTitle);
  out +=
      " Summarize it for a reading companion that will later answer a reader's questions about the book using "
      "only such summaries, so keep what matters for that: every named character who appears, with who they "
      "are and what they do; the key events in order; places; and any revelation, decision or change in a "
      "relationship. Only use the text, do not add anything from outside it. Write in ";
  out += languageOrEnglish(languageName);
  out +=
      ", plain text, no Markdown, 120 to 250 words. If the text is not story content (a title page, table of "
      "contents, copyright or acknowledgements), answer with just the word NONE.";
  return out;
}

std::string contextSummaryHeading(const int partNumber, const char* title) {
  std::string out = "[Part ";
  out += std::to_string(partNumber);
  if (title && title[0]) {
    out += ": ";
    out += title;
  }
  out += "]\n";
  return out;
}

std::string contextCurrentChapterHeading(const char* title, const bool startsMidChapter) {
  std::string out = "\nThe chapter the reader is in now";
  if (title && title[0]) {
    out += " (\"";
    out += title;
    out += "\")";
  }
  out += startsMidChapter ? ", from partway through it up to where the reader stopped:\n\n"
                          : ", from its beginning up to where the reader stopped:\n\n";
  return out;
}

std::string streamingBodyPrefix(const std::string& instructions, const bool lowThinking) {
  std::string out = "{\"systemInstruction\":{\"parts\":[{\"text\":\"";
  appendJsonEscaped(out, instructions.data(), instructions.size());
  out += "\"}]},";
  if (lowThinking) out += "\"generationConfig\":{\"thinkingConfig\":{\"thinkingLevel\":\"low\"}},";
  out += "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"";
  return out;
}

const char* streamingBodySuffix() { return "\"}]}]}"; }

void appendJsonEscaped(std::string& out, const char* data, const size_t len) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    const auto c = static_cast<unsigned char>(data[i]);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += HEX_DIGITS[c >> 4];
          out += HEX_DIGITS[c & 0xF];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
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
