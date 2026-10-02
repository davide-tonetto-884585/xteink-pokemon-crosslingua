#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;
struct Arena;
template <typename T>
class ArenaVector;

class ParsedText {
 public:
  // Lingua: where a PRE-layout token index ended up once the block was broken into lines. Filled 1:1
  // with setTrackedWords, in the same order, so a caller (LinguaLayout::Interlinear) can map "the word
  // that opens sentence k" to a line and an x without re-deriving anything layout already knows.
  // The tracked indices constrain nothing: this is a pure REPORT.
  struct TrackedWordPos {
    static constexpr uint16_t NOT_PLACED = 0xFFFF;
    // Ordinal among the lines actually emitted by this layout call.
    uint16_t line = NOT_PLACED;
    // x within the block's measure at which that token was laid out.
    int16_t x = 0;
    // The token is the FIRST LOGICAL word of that line.
    bool startsLine = false;
  };

 private:
  // words/rubyTexts are std::deque, not std::vector: a paragraph can hold thousands
  // of tokens (CJK splits every character), and a vector grows by reallocating its
  // whole element array into one contiguous block (32 B/std::string -> 64-128 KB at
  // a few thousand tokens). On the ESP32-C3 that single large contiguous request
  // fails under a fragmented, BLE-resident heap and the throwing operator new
  // abort()s the firmware (fresh-open CJK crash). A deque grows in fixed ~512 B nodes
  // (largest contiguous alloc stays ~2 KB regardless of token count), so it never
  // triggers that. The per-token parallel arrays below stay vectors: 1 byte / 1 bit
  // each, they never approach the contiguous-block ceiling.
  std::deque<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;         // true = word attaches to previous (no space before it)
  std::vector<bool> wordNoSpaceBefore;     // true = may break before token, but no synthetic space when joined
  std::vector<uint8_t> wordFocusBoundary;  // UTF-8 byte offset where the regular suffix starts; 0 = no split
  std::vector<bool> wordGuideDotBefore;    // true = virtual guide dot belongs between previous token and this one
  std::vector<uint8_t> wordBackgroundBlack;
  // Layout-only text coordinates. The rendered page never retains these; use
  // compact deltas while a paragraph is pending to protect C3 heap headroom.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  // Populated only during a stable-page jump build. Chunked storage avoids a
  // large contiguous request for long CJK paragraphs on constrained devices.
  std::deque<uint32_t> wordReferenceOffsets;
  std::deque<std::string> rubyTexts;
  bool extraParagraphSpacing;
  bool forceParagraphIndents;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool guideReadingEnabled;
  uint8_t wordSpacing;
  BlockStyle blockStyle;
  bool hasRtlWord;
  bool trackReferenceOffsets;
  // True after an intermediate flush leaves the rest of the same paragraph
  // buffered. The next layout pass must not apply first-line paragraph rules.
  bool isContinuation_ = false;
  bool allowCharacterBreaks_ = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<bool> reorderedGuideDotBeforeScratch;
  std::vector<uint8_t> reorderedBackgroundBlackScratch;
  std::vector<std::string> lineWordsScratch;
  std::vector<EpdFontFamily::Style> lineStylesScratch;
  std::vector<uint16_t> lineWidthsScratch;
  std::vector<uint8_t> lineFocusBoundaryScratch;
  std::vector<bool> lineGuideDotBeforeScratch;
  std::vector<bool> lineHasSpaceBeforeScratch;
  std::vector<uint8_t> lineBackgroundBlackScratch;
  std::vector<uint16_t> visualOrderScratch;
  // Lingua: word indices (in the CURRENT layout call's PRE-layout index space) whose final resting
  // place the caller wants reported back. Ascending. Empty for every layout but Interlinear.
  std::vector<uint16_t> trackedWords;
  // Destination for those reports, borrowed for the duration of one layoutAndExtractLines call.
  std::vector<TrackedWordPos>* trackedOut = nullptr;
  // Index of the line extractLine is about to emit, among the lines emitted by this layout call.
  size_t emittedLineOrdinal = 0;

  // Lingua: paragraph direction / SD-font preparation shared by whole-block and per-line layout.
  void prepareForLayout(const GfxRenderer& renderer, int fontId);
  // Drop the first `consumed` tokens from every parallel container once their line is out.
  void consumeWords(size_t consumed);
  // Shift tracked indices past a token inserted at `insertedIndex` (hyphenation/forced splits).
  void rebaseTrackedWordsAfterInsert(size_t insertedIndex);

  void reserveTokenCapacity(size_t additionalTokens);
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  bool calculateGapMetrics(ArenaVector<int16_t>& naturalGaps, ArenaVector<uint8_t>& gapSlots,
                           const GfxRenderer& renderer, int fontId);
  bool computeLineBreaks(Arena& scratchArena, const GfxRenderer& renderer, int fontId, int pageWidth,
                         ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                         std::vector<bool>& noSpaceBeforeVec, ArenaVector<int16_t>& naturalGaps,
                         ArenaVector<uint8_t>& gapSlots, ArenaVector<size_t>& lineBreakIndices);
  bool computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                   std::vector<bool>& noSpaceBeforeVec, ArenaVector<size_t>& lineBreakIndices);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            ArenaVector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  bool splitTokenAtCodepointBoundary(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                                     ArenaVector<uint16_t>& wordWidths);
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  bool extractLine(Arena& scratchArena, size_t breakIndex, int pageWidth, const ArenaVector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const ArenaVector<int16_t>& naturalGaps, const ArenaVector<uint8_t>& gapSlots,
                   const ArenaVector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  bool calculateWordWidths(ArenaVector<uint16_t>& wordWidths, const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool forceParagraphIndents = false,
                      const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const bool guideReadingEnabled = false, const uint8_t wordSpacing = 0,
                      const BlockStyle& blockStyle = BlockStyle(), const bool trackReferenceOffsets = false)
      : extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        wordSpacing(wordSpacing),
        blockStyle(blockStyle),
        hasRtlWord(false),
        trackReferenceOffsets(trackReferenceOffsets) {}
  ~ParsedText() = default;

  // Lingua: the reader's style-free first-line paragraph indent for text that carries no CSS at all
  // (the Page Translation overlay): three space widths when Extra Paragraph Spacing is off, none when on.
  static int defaultFirstLineIndent(const GfxRenderer& renderer, int fontId, bool extraParagraphSpacing);

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool backgroundBlack = false, uint8_t linkId = 0, uint32_t visibleTextOffset = 0,
               uint32_t referenceTextOffset = 0);
  // Lingua: grow the parallel token vectors for `additionalTokens` more entries in one step.
  void reserveAdditionalWords(size_t additionalTokens) { reserveTokenCapacity(additionalTokens); }
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  // Lingua: read-only access to a word still in LOGICAL order (before layout consumes the block).
  const std::string& wordAt(const size_t index) const {
    static const std::string kEmpty;
    return index < words.size() ? words[index] : kEmpty;
  }
  // Lingua: token boundary marker that must be carried when re-emitting a span into another block.
  bool wordAttachesToPrevious(const size_t index) const { return index < wordContinues.size() && wordContinues[index]; }
  // Focus-reading splits are byte boundaries inside one token here, so there are no suffix tokens.
  bool wordIsFocusSuffixAt(const size_t index) const {
    (void)index;
    return false;
  }
  // True when at least one word added to this block starts with an RTL codepoint.
  bool containsRtlWord() const { return hasRtlWord; }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }
  // Lingua: ask layout to REPORT where each listed (ascending, pre-layout) word ends up. Scoped to ONE
  // layout call; the list is re-based internally whenever hyphenation inserts a remainder word.
  void setTrackedWords(std::vector<uint16_t> wordIndices) { trackedWords = std::move(wordIndices); }
  // `trackedOutParam`, when non-null, is filled with ONE entry per index passed to setTrackedWords.
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t, uint32_t)>& processLine,
                             bool includeLastLine = true, std::vector<TrackedWordPos>* trackedOutParam = nullptr);
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, std::vector<TrackedWordPos>* trackedOutParam = nullptr) {
    return layoutAndExtractLines(
        renderer, fontId, viewportWidth,
        [&processLine](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) { processLine(std::move(line)); },
        includeLastLine, trackedOutParam);
  }
  // Lingua: lay out exactly ONE line at `width` and consume only that line's words, leaving the rest
  // of the block for a later call (possibly at a different width). Returns true when a line reached
  // `processLine`. The words are consumed either way, so a loop on size() always terminates.
  bool extractNextLine(const GfxRenderer& renderer, int fontId, uint16_t width,
                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  bool layoutAndExtractLinesPreservingSource(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                             bool allowCharacterBreaks = false) const;
};
