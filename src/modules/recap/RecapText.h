#pragma once

#include <cstddef>
#include <string>

/**
 * Chapter recap: plain-text reconstruction of laid-out reader pages.
 *
 * The reader only keeps the chapter as laid-out lines of words (TextBlock), so the text sent to the
 * model is rebuilt word by word: hyphens the line breaker inserted are dropped and the two halves
 * joined, an em-space indent prefix (how the layout marks a paragraph's first line) becomes a
 * paragraph break, and everything else is joined with single spaces. Pure (no Arduino deps) so the
 * native tests can cover it.
 */
namespace recap {

// Upper bound on the chapter text sent in one request. The request body (this text JSON-escaped)
// and the response both live in heap during the TLS session, with the reader torn down and the
// framebuffer released; ~20 KB of text is ~5k tokens, far below the model's context but enough for
// 10-15 typical pages.
constexpr size_t MAX_INPUT_BYTES = 20000;

class RecapTextBuilder {
 public:
  // Append one laid-out word. `spaceBefore` mirrors TextBlock::wordHasSpaceBefore() for words after
  // the first on a line; `endsWithInsertedHyphen` mirrors TextBlock::wordEndsWithInsertedHyphen().
  void addWord(const char* word, size_t len, bool spaceBefore, bool endsWithInsertedHyphen);
  // A laid-out line ended: the next line's first word gets a space unless the line was hyphenated.
  void endLine();
  // A page ended: pages are joined like lines (a paragraph can continue on the next page).
  void endPage() { endLine(); }

  bool empty() const { return text_.empty(); }
  size_t size() const { return text_.size(); }
  std::string take();
  // Hands over the text built so far but keeps the join state, so text written page by page to a
  // file reads exactly like one take() of the whole range.
  std::string drain();

 private:
  std::string text_;
  bool lineStart_ = true;
  bool joinNextLine_ = false;
  char last_ = 0;  // last byte ever appended (0: nothing yet)
};

// Keep only the last `maxBytes` of `text`, cutting at a whitespace boundary (never inside a UTF-8
// sequence). Returns true when something was dropped.
bool keepTail(std::string& text, size_t maxBytes);

}  // namespace recap
