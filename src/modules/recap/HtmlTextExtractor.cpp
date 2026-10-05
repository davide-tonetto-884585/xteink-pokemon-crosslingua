#include "modules/recap/HtmlTextExtractor.h"

#include <Epub/htmlEntities.h>

#include <cstdlib>
#include <cstring>

namespace recap {
namespace {
constexpr size_t FLUSH_BYTES = 512;
constexpr size_t MAX_ENTITY_BYTES = 12;
// Tags longer than this are malformed or carry huge attributes (inline SVG paths); only the name
// matters, so the rest is dropped instead of growing the buffer.
constexpr size_t MAX_TAG_BYTES = 64;

bool isSpace(const char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

bool oneOf(const std::string& name, const char* const* names) {
  for (; *names; ++names) {
    if (name == *names) return true;
  }
  return false;
}

constexpr const char* SKIPPED_ELEMENTS[] = {"head", "script", "style", "title", "rt", "rp", "svg", nullptr};
constexpr const char* BLOCK_ELEMENTS[] = {
    "p",     "div",    "br",      "h1",     "h2",     "h3",         "h4",      "h5",  "h6",
    "li",    "tr",     "ul",      "ol",     "dl",     "dt",         "dd",      "hr",  "blockquote",
    "pre",   "table",  "section", "article", "header", "footer",     "aside",   "nav", "figure",
    "figcaption", "caption", "body", nullptr};

void appendUtf8(std::string& out, const unsigned long cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}
}  // namespace

void HtmlTextExtractor::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = data[i];
    switch (state_) {
      case State::Text:
        if (c == '<') {
          state_ = State::Tag;
          tag_.clear();
          quote_ = 0;
        } else if (!skipUntil_.empty()) {
          // Inside a skipped element: ignore text.
        } else if (c == '&') {
          state_ = State::Entity;
          entity_.assign(1, '&');
        } else {
          emitChar(c);
        }
        break;

      case State::Tag:
        if (quote_) {
          if (c == quote_) quote_ = 0;
          break;
        }
        if (c == '>') {
          state_ = State::Text;
          handleTag();
          break;
        }
        if ((c == '"' || c == '\'') && !tag_.empty() && tag_[0] != '!') {
          quote_ = c;
          break;
        }
        if (tag_.size() < MAX_TAG_BYTES) tag_ += c;
        if (tag_ == "!--") {
          state_ = State::Comment;
          commentDashes_ = 0;
        }
        break;

      case State::Comment:
        if (c == '>' && commentDashes_ >= 2) {
          state_ = State::Text;
        } else {
          commentDashes_ = c == '-' ? commentDashes_ + 1 : 0;
        }
        break;

      case State::Entity:
        entity_ += c;
        if (c == ';') {
          state_ = State::Text;
          handleEntity();
        } else if (entity_.size() >= MAX_ENTITY_BYTES || isSpace(c) || c == '<' || c == '&') {
          // Not an entity after all: emit what was collected as text, then re-process this byte.
          entity_.pop_back();
          state_ = State::Text;
          emitText(entity_.data(), entity_.size());
          i--;
        }
        break;
    }
  }
}

void HtmlTextExtractor::handleTag() {
  if (tag_.empty() || tag_[0] == '!' || tag_[0] == '?') return;  // doctype, CDATA, processing instruction
  const bool closing = tag_[0] == '/';
  size_t pos = closing ? 1 : 0;
  std::string name;
  while (pos < tag_.size() && !isSpace(tag_[pos]) && tag_[pos] != '/') {
    name += lower(tag_[pos]);
    pos++;
  }
  // Drop a namespace prefix (xhtml:p).
  const size_t colon = name.find(':');
  if (colon != std::string::npos) name.erase(0, colon + 1);
  const bool selfClosing = !closing && !tag_.empty() && tag_.back() == '/';

  if (!skipUntil_.empty()) {
    if (closing && name == skipUntil_) skipUntil_.clear();
    return;
  }
  if (!closing && !selfClosing && oneOf(name, SKIPPED_ELEMENTS)) {
    skipUntil_ = name;
    return;
  }
  if (oneOf(name, BLOCK_ELEMENTS)) {
    pendingNewline_ = true;
  } else if (name == "td" || name == "th") {
    pendingSpace_ = true;
  }
}

void HtmlTextExtractor::handleEntity() {
  std::string decoded;
  if (entity_.size() > 3 && entity_[1] == '#') {
    const bool hex = entity_[2] == 'x' || entity_[2] == 'X';
    const std::string digits = entity_.substr(hex ? 3 : 2, entity_.size() - (hex ? 4 : 3));
    char* end = nullptr;
    const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
    if (end && *end == '\0' && !digits.empty() && cp > 0) appendUtf8(decoded, cp);
  } else if (const char* value = lookupHtmlEntity(entity_.data(), entity_.size())) {
    decoded = value;
  }
  if (decoded.empty()) {
    emitText(entity_.data(), entity_.size());
  } else if (decoded == "\xc2\xa0") {
    emitChar(' ');  // &nbsp; is just a space for the model
  } else {
    emitText(decoded.data(), decoded.size());
  }
}

void HtmlTextExtractor::emitChar(const char c) {
  if (isSpace(c)) {
    pendingSpace_ = true;
    return;
  }
  if (pendingNewline_) {
    if (!lastWasNewline_) out_ += '\n';
    lastWasNewline_ = true;
  } else if (pendingSpace_ && !lastWasNewline_) {
    out_ += ' ';
  }
  pendingNewline_ = false;
  pendingSpace_ = false;
  out_ += c;
  lastWasNewline_ = false;
  if (out_.size() >= FLUSH_BYTES) flush();
}

void HtmlTextExtractor::emitText(const char* s, const size_t len) {
  for (size_t i = 0; i < len; i++) emitChar(s[i]);
}

void HtmlTextExtractor::flush() {
  if (out_.empty()) return;
  if (sink_) sink_(out_.data(), out_.size());
  emitted_ += out_.size();
  out_.clear();
}

void HtmlTextExtractor::finish() {
  if (state_ == State::Entity) emitText(entity_.data(), entity_.size());
  state_ = State::Text;
  flush();
}

}  // namespace recap
