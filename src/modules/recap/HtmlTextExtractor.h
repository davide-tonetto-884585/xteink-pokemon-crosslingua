#pragma once

#include <cstddef>
#include <functional>
#include <string>

/**
 * Book assistant: streaming XHTML -> plain text, for chapters the reader has not laid out.
 *
 * The assistant summarizes every chapter before the current one, and most of them have no page
 * cache, so their text comes straight from the EPUB's XHTML. This is a small tolerant state
 * machine, not a parser: tags are dropped, block-level tags become line breaks, <head>/<script>/
 * <style>/ruby annotations are skipped, entities are decoded and whitespace is collapsed. Fed in
 * arbitrary chunks (a tag, entity or UTF-8 sequence may straddle two feed() calls) and writes to
 * a sink in small batches, so a whole chapter never has to sit in RAM. Pure; see test/chapter_recap.
 */
namespace recap {

class HtmlTextExtractor {
 public:
  using Sink = std::function<void(const char* data, size_t len)>;

  explicit HtmlTextExtractor(Sink sink) : sink_(std::move(sink)) {}

  void feed(const char* data, size_t len);
  // Flushes buffered text. Call once after the last feed().
  void finish();

  // Bytes of text written to the sink so far (after finish(), the total).
  size_t textBytes() const { return emitted_ + out_.size(); }

 private:
  enum class State : unsigned char { Text, Tag, Comment, Entity };

  void handleTag();
  void handleEntity();
  void emitChar(char c);
  void emitText(const char* s, size_t len);
  void flush();

  Sink sink_;
  State state_ = State::Text;
  std::string out_;
  size_t emitted_ = 0;
  std::string tag_;     // raw tag contents between '<' and '>'
  std::string entity_;  // "&...;" being collected
  std::string skipUntil_;  // closing tag name ending a skipped element (empty: not skipping)
  char quote_ = 0;         // quote char while inside a quoted attribute value
  int commentDashes_ = 0;
  bool pendingSpace_ = false;
  bool pendingNewline_ = false;
  bool lastWasNewline_ = true;  // start of output behaves like after a newline
};

}  // namespace recap
