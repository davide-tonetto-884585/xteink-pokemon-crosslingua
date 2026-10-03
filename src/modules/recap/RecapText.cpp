#include "modules/recap/RecapText.h"

namespace recap {
namespace {
// U+2003 EM SPACE: the layout prefixes a paragraph's first word with it to draw the indent.
constexpr char EM_SPACE[] = "\xe2\x80\x83";
constexpr size_t EM_SPACE_LEN = 3;

bool isSpace(const char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

void trimTrailingSpaces(std::string& s) {
  while (!s.empty() && s.back() == ' ') s.pop_back();
}
}  // namespace

void RecapTextBuilder::addWord(const char* word, size_t len, const bool spaceBefore,
                               const bool endsWithInsertedHyphen) {
  if (!word) return;
  bool paragraphStart = false;
  while (len >= EM_SPACE_LEN && word[0] == EM_SPACE[0] && word[1] == EM_SPACE[1] && word[2] == EM_SPACE[2]) {
    paragraphStart = true;
    word += EM_SPACE_LEN;
    len -= EM_SPACE_LEN;
  }
  if (endsWithInsertedHyphen && len > 0 && word[len - 1] == '-') len--;
  if (len == 0) return;

  if (!text_.empty()) {
    if (paragraphStart) {
      trimTrailingSpaces(text_);
      if (text_.back() != '\n') text_ += '\n';
    } else if (lineStart_) {
      if (!joinNextLine_ && !isSpace(text_.back())) text_ += ' ';
    } else if (spaceBefore && !isSpace(text_.back())) {
      text_ += ' ';
    }
  }
  text_.append(word, len);
  lineStart_ = false;
  joinNextLine_ = endsWithInsertedHyphen;
}

void RecapTextBuilder::endLine() { lineStart_ = true; }

std::string RecapTextBuilder::take() {
  std::string out = std::move(text_);
  text_.clear();
  lineStart_ = true;
  joinNextLine_ = false;
  return out;
}

bool keepTail(std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return false;
  const size_t start = text.size() - maxBytes;
  // Advance to the next whitespace so no word is split. Scripts without spaces (CJK) fall back to
  // the next UTF-8 lead byte, so a sequence is never cut in half.
  constexpr size_t MAX_WORD_SEARCH = 64;
  size_t cut = start;
  while (cut < text.size() && cut - start < MAX_WORD_SEARCH && !isSpace(text[cut])) cut++;
  if (cut < text.size() && isSpace(text[cut])) {
    while (cut < text.size() && isSpace(text[cut])) cut++;
  } else {
    cut = start;
    while (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut++;
  }
  text.erase(0, cut);
  return true;
}

}  // namespace recap
