#pragma once

#include <Arena.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <expat.h>

#include <algorithm>
#include <array>
#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/EpubRenderMode.h"
#include "Epub/FootnoteEntry.h"
#include "Epub/Page.h"
#include "Epub/PageFontSet.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"
#include "Epub/tables/CompactTableLayout.h"
#include "modules/lingua/layout/LinguaLayout.h"
#include "modules/lingua/modes/interlinear/InterlinearAnnotation.h"

class GfxRenderer;
class Epub;
#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
 public:
  enum class ParseStatus { More, Done, Error };

 private:
  static constexpr uint8_t MAX_SIMPLE_TABLE_COLUMNS = 8;
  static constexpr uint16_t MAX_SIMPLE_TABLE_CELL_WORDS = 160;
  static constexpr uint8_t TABLE_CELL_PADDING = 6;
  static constexpr size_t MAX_INLINE_STYLE_DEPTH = 64;
  static constexpr size_t MAX_BLOCK_STYLE_DEPTH = 16;

  Epub* epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int skipEndElementStateUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int strikethroughUntilDepth = INT_MAX;
  int headingDepth = -1;
  bool headingOpenerActive = false;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t visibleTextOffset = 0;
  uint32_t partWordReferenceOffset = 0;
  uint32_t referenceTextOffset = 0;
  bool referenceTextStarted = false;
  bool referenceWhitespacePending = false;
  bool trackReferenceCharacters = false;
  int referenceExcludedUntilDepth = INT_MAX;
  uint16_t currentTextRunBytes = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  uint32_t currentPageVisibleOffset = 0;
  uint32_t currentPageReferenceOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  bool forceParagraphIndents;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool guideReadingEnabled;
  uint8_t wordSpacing;
  CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;
  bool lowMemoryImageFallback = false;
  bool lowMemoryAbort = false;
  bool attemptedTextLayoutFontCacheRelease = false;
  EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault;

  // ---- Lingua (CrossLingua translation module) -------------------------------------------------
  // The page layout to produce. The parser never sees the user's display mode --
  // CrossPointSettings::linguaLayoutForDisplayMode() collapses the modes onto these layouts.
  LinguaLayout linguaLayout = LinguaLayout::Both;
  // Font the TRANSLATED text is laid out in, or 0 for "same as fontId" (the ONLY unset sentinel).
  int translationFontId = 0;
  // (Interlinear) font the small ANNOTATION rows are laid out in, or 0 for "same as fontId".
  int annotationFontId = 0;
  // (Interlinear) the app's sentence aligner; nullptr means "no annotations".
  InterlinearPairFn interlinearPairFn = nullptr;
  // Reusable annotation buffer for the interlinear pass, sized once on the first annotated paragraph.
  std::vector<InterlinearAnnotation> interlinearAnnotations;
  // Monotonic index over ORIGINAL content paragraphs (see ParagraphBoundary.h SSOT shared with the
  // PageTranslationOverlay/TooltipOverlay reparsers). Translated blocks do NOT advance it.
  int16_t paragraphCounter = 0;
  // Paragraph index stamped on the currently-open block's laid-out lines; -1 before any block opens.
  int16_t currentBlockParagraphIdx = -1;
  // True once the current physical text block has been assigned a paragraph index.
  bool currentBlockIndexAssigned = false;
  // True when the currently-open block has lang= differing from bookPrimaryLang.
  bool currentBlockIsTranslated = false;
  // Depth of the outermost open element that introduced a translated lang= (INT_MAX = none). Every
  // word parsed while it is open belongs to a translated block (mirrors boldUntilDepth & co.).
  int translatedFromDepth = INT_MAX;
  std::string bookPrimaryLang;      // Book's content.opf language; a differing lang= marks a translated block
  std::string translatedHyphenLang;  // Last lang= applied to the Hyphenator's translated slot
  // (SideBySide and Interlinear) the current ORIGINAL block is buffered here until its paired
  // translation arrives, together with its paragraph index and its footnote ledger.
  std::unique_ptr<ParsedText> bufferedOriginalBlock = nullptr;
  int16_t bufferedOriginalParagraphIdx = -1;
  std::vector<std::pair<int, FootnoteEntry>> bufferedOriginalFootnotes;
  int bufferedOriginalWordsExtracted = 0;
  bool inTranslatedText() const { return translatedFromDepth != INT_MAX; }
  // --------------------------------------------------------------------------------------------

  std::string previewAnchor;
  uint16_t previewMaxPages = 0;
  bool previewAnchorFound = false;
  bool previewStopRequested = false;
  // Element ordinals (1-based, counting every startElement) used to start a footnote preview at the
  // block enclosing the anchor rather than at the anchor itself. 0 means "no block located".
  uint32_t previewStartOrdinal = 0;
  uint32_t previewElementOrdinal = 0;
  bool malformedMarkupTruncated = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  XML_Parser activeParser = nullptr;
  FsFile parseFile_;
  size_t parseFileOffset_ = 0;
  size_t parseFileSize_ = 0;
  uint32_t parseStartTime_ = 0;

  bool ensureInputFileOpen();
  uint32_t consumeReferenceCodepoint(uint32_t codepoint);
  void consumeReferenceCharacters(const XML_Char* text, int length);
  void clearReferenceExclusionIfClosed();

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
    bool hasBackgroundBlack = false, backgroundBlack = false;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasSmallCaps = false, smallCaps = false;
  };
  // Arena-backed style stacks. Initialized in parseAndBuildPages(); pointers are
  // null before and after each parse. StyleStackEntry and BlockStyle are trivially
  // destructible, so clear() on the arena is sufficient cleanup.
  Arena parseArena_;
  StyleStackEntry* inlineStyleBuf_ = nullptr;
  size_t inlineStyleCount_ = 0;
  BlockStyle* blockStyleBuf_ = nullptr;
  size_t blockStyleCount_ = 0;
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  bool effectiveBackgroundBlack = false;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;
  bool effectiveSmallCaps = false;

  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    std::vector<std::pair<int, FootnoteEntry>> footnotes;
    uint32_t visibleTextOffset = 0;
    bool isHeader = false;
    uint8_t colSpan = 1;
  };

  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool hasHeaderCell = false;
    bool hasDataCell = false;
    uint16_t effectiveColumnCount = 0;
  };

  struct BufferedTable {
    BlockStyle blockStyle;
    std::vector<BufferedTableRow> rows;
    uint16_t maxCols = 0;
    uint16_t totalCells = 0;
    bool unsupported = false;
    // When the whole-table reservation is unavailable, retain only the current
    // source row plus the render-ready rows that fit on the active page.
    bool streaming = false;
    bool streamingFlattened = false;
    bool streamingTopSpacingApplied = false;
    uint8_t streamingColumnCount = 0;
    uint8_t streamingFragmentColumnCount = 0;
    uint16_t streamingFragmentHeight = 1;
    uint32_t streamingFragmentVisibleOffset = 0;
    std::vector<TableFragmentRow> streamingFragmentRows;
    std::vector<FootnoteEntry> streamingFragmentFootnotes;
  };

  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;
  int pendingListMarkerDepth = -1;
  struct ListContext {
    bool ordered = false;
    bool styleNone = false;
    int32_t nextValue = 1;
    int depth = 0;
  };
  std::array<ListContext, MAX_BLOCK_STYLE_DEPTH> listContexts_{};
  size_t listContextCount_ = 0;
  bool currentTableCellIsHeader = false;
  uint8_t currentTableCellColSpan = 1;
  uint32_t currentTableCellVisibleOffset = 0;
  std::unique_ptr<BufferedTable> currentTableBuffer = nullptr;
  std::unique_ptr<CompactTableLayout> currentCompactTable = nullptr;
  bool compactTableFlattened = false;
  bool compactTableUnsupported = false;
  bool compactTableTopSpacingApplied = false;
  uint8_t compactFragmentColumnCount = 0;
  uint16_t compactFragmentHeight = 1;
  uint32_t compactFragmentVisibleOffset = 0;
  std::vector<TableFragmentRow> compactFragmentRows;
  std::vector<FootnoteEntry> compactFragmentFootnotes;
  std::vector<CssAncestorEntry> ancestorStack_;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  bool pendingAnchorFromInlineA = false;
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  uint16_t currentTextBlockParagraphIndex = 0;
  uint16_t currentTextBlockListItemIndex = 0;
  uint16_t currentPageParagraphIndex = 0;
  uint16_t currentPageListItemIndex = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  uint8_t nextFootnoteLinkId = 1;
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  struct PendingPublisherPageMarker {
    int wordIndex = 0;
    char label[16] = {};
  };
  std::vector<PendingPublisherPageMarker> pendingPublisherPageMarkers;

  void updateEffectiveInlineStyle();
  void skipCurrentElement();
  void skipDescendantsOfCurrentElement();
  bool shouldAbortForLowMemory(const char* stage);
  bool startNewPage(const char* reason);
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void addPendingPublisherPageMarker(const char* label);
  void attachPendingPublisherPageMarkers(int yPos);
  void flushPartWordBuffer();
  // Lingua: true when the block currently being parsed is one the active linguaLayout drops.
  bool wordIsFiltered() const;
  // Lingua: settle a <br>-opened block's translation state and paragraph index from its first word.
  void classifyBrOpenedBlock();
  // Lingua: stamp the just-opened block's translation state and paragraph index.
  void stampLinguaBlockOpen(bool translated);
  // Lingua: the role every line of the currently-open block carries.
  LineFontRole currentLineRole() const;
  // The font id a role is MEASURED and ADVANCED with (same resolver the renderer uses).
  int fontIdForRole(const LineFontRole role) const {
    return PageFontSet(fontId, translationFontId, annotationFontId).forRole(role);
  }
  // Deliver every anchor still pending for the block just laid out to the page currently being built.
  void flushPendingFootnotesToCurrentPage();
  // Lingua: one block's footnote ledger -- the anchors pending for it and their word base.
  struct FootnoteLedger {
    std::vector<std::pair<int, FootnoteEntry>> pending;
    int wordBase = 0;
  };
  [[nodiscard]] FootnoteLedger adoptBufferedFootnoteLedger();
  void releaseFootnoteLedger(FootnoteLedger& parked);
  // Lingua (SideBySide / Interlinear) pairing builders; defined under src/modules/lingua.
  void makePagesTableMode();
  void flushBufferedOriginal();
  void renderSideBySide(std::unique_ptr<ParsedText> sourceBlock, std::unique_ptr<ParsedText> transBlock);
  void appendSideBySideNoTranslationMarkerIfUnpaired();
  void makePagesInterlinearMode();
  void renderInterlinear(std::unique_ptr<ParsedText> origBlock, std::unique_ptr<ParsedText> transBlock);
  struct InterlinearRun {
    std::shared_ptr<TextBlock> row;  // null is legal: the strip is reserved and nothing is drawn
    int16_t x = 0;                   // EXTRA offset added to the block's left inset at placement
    int16_t rightEdge = 0;           // right edge of this run's ink, measure-relative
    uint16_t slot = 0;               // which of the sentence's source lines this row belongs over
  };
  struct InterlinearBands {
    const std::vector<std::shared_ptr<TextBlock>>* srcLines = nullptr;
    size_t firstLine = 0;
    size_t slots = 0;
    int16_t headX = 0;
    int16_t tailX = 0;
    int16_t floorX = 0;
  };
  void buildAnnotationRuns(const InterlinearAnnotation& annotation, const ParsedText& transBlock,
                           const InterlinearBands& bands, uint16_t measureWidth, int annotationFont,
                           std::vector<InterlinearRun>& runs, ParsedText& stream);
  void placeInterlinearRow(const std::shared_ptr<TextBlock>& row, int16_t xPos, int16_t yPos, LineFontRole role);
  void emitInterlinearPair(const std::vector<InterlinearRun>& runs, const std::shared_ptr<TextBlock>& srcLine,
                           int stripHeight, int srcRowHeight, int16_t leftInset, uint32_t sourceOffset);
  // Lingua: line advance for a font, with the reader's line compression applied.
  int linguaLineHeight(int lineFontId) const;
  // Lingua: route a flushed block through the pairing builder that owns the active layout.
  void makePagesForLayout();
  void flushLongTextRunIfNeeded(bool force = false);
  size_t bufferedWordsBeforeLayoutLimit() const;
  uint16_t textRunBytesBeforeLayoutLimit() const;
  void markCurrentPageFromCurrentTextBlock();
  void markCurrentPageFromCurrentElement();
  void setCurrentPageVisibleOffset(uint32_t offset, uint32_t referenceOffset = UINT32_MAX);
  void completeCurrentPage();
  void makePages();
  int effectiveLineHeight() const;
  bool isPreviewBuild() const { return !previewAnchor.empty() && previewMaxPages > 0; }
  bool isScanningForPreviewAnchor() const { return isPreviewBuild() && !previewAnchorFound; }
  bool handlePreviewScanStart(const XML_Char** atts);
  void locatePreviewBlockStart();
  void startPreviewAtAnchor();
  void stopPreviewIfPageLimitReached();
  bool usesSimpleCssLookup() const { return renderMode != EpubRenderMode::CrossInkDefault; }
  bool flattensTables() const { return renderMode != EpubRenderMode::CrossInkDefault; }
  bool isLightMode() const { return renderMode == EpubRenderMode::Light; }
  bool honorsPublisherDecorations() const { return renderMode != EpubRenderMode::Light; }
  void pushCssAncestor(int depth, const char* tag, std::string_view classAttr);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applySmallCapsToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  void finalizeCurrentTableCell();
  void emitBufferedTableAsParagraphs(BufferedTable& table);
  void emitBufferedTableAsFragments(BufferedTable& table);
  bool streamCurrentTableRow();
  bool flushStreamingTableFragment(BufferedTable& table);
  void emitStreamingTableRowsAsParagraphs(BufferedTable& table);
  void finishStreamingTable(BufferedTable& table);
  bool flushCompactTableFragment();
  bool emitCompactTableRow(TableFragmentRow& row, std::vector<std::shared_ptr<TextBlock>>& flatLines,
                           const std::vector<FootnoteEntry>& footnotes, uint32_t visibleTextOffset,
                           uint8_t fragmentColumnCount, bool flatten);
  void finishCompactTable();
  void fallbackStreamingTableToParagraphs(const char* reason);
  void emitCurrentTableBuffer();
  void fallbackCurrentTableBufferToParagraphs(const char* reason);
  void flushMalformedPartialContent();
  bool appendMalformedMarkupWarningPage();
  void prewarmSectionAdvanceTable(FsFile& file) const;
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      Epub& epub, const std::string& filepath, GfxRenderer& renderer, const int fontId, const float lineCompression,
      const bool extraParagraphSpacing, const bool forceParagraphIndents, const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled, const bool guideReadingEnabled, const uint8_t wordSpacing,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, CssParser* cssParser = nullptr,
      const EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault, std::string previewAnchor = {},
      const uint16_t previewMaxPages = 0, const bool trackReferenceCharacters = false)

      : epub(&epub),
        filepath(filepath),
        renderer(renderer),
        completePageFn(completePageFn),
        popupFn(popupFn),
        trackReferenceCharacters(trackReferenceCharacters),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        wordSpacing(wordSpacing > 4 ? 4 : wordSpacing),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        renderMode(renderMode),
        previewAnchor(std::move(previewAnchor)),
        previewMaxPages(previewMaxPages),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();
  uint32_t getVisibleTextLength() const { return visibleTextOffset; }
  bool parseAndBuildPages();
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)
  void releaseInputFile();

  // Lingua: configure the translation layout before beginParse(). `bookPrimaryLang` is the book's
  // content.opf language; a block whose lang= differs is a translated block.
  void configureLingua(LinguaLayout layout, const std::string& primaryLang, int translationFont, int annotationFont,
                       InterlinearPairFn pairFn) {
    linguaLayout = layout;
    bookPrimaryLang = primaryLang;
    translationFontId = translationFont;
    annotationFontId = annotationFont;
    interlinearPairFn = pairFn;
  }

  // `role` is the role the caller MEASURED this line with: it decides both the vertical advance and
  // the byte stamped on the emitted PageLine.
  void addLineToPage(std::shared_ptr<TextBlock> line, LineFontRole role, uint32_t visibleOffset,
                     uint32_t referenceOffset);
  void addLineToPage(std::shared_ptr<TextBlock> line, uint32_t visibleOffset, uint32_t referenceOffset) {
    addLineToPage(std::move(line), LineFontRole::Body, visibleOffset, referenceOffset);
  }
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  bool wasLowMemoryFallbackTriggered() const { return lowMemoryImageFallback; }
  bool wasLowMemoryAbortTriggered() const { return lowMemoryAbort; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : parseFileOffset_; }
  size_t parseTotalBytes() { return parseFileSize_; }
};
