#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <Epub/parsers/ParagraphBoundary.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "fontIds.h"
#include "modules/lingua/services/TranslatedContentDetector.h"
void ChapterHtmlSlimParser::makePagesInterlinearMode() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  if (currentBlockIsTranslated) {
    // Translation paragraph: pair it with the buffered original if one is waiting, otherwise fall
    // back to a full-width layout (a translation with no preceding original — unusual).
    if (bufferedOriginalBlock) {
      // Same ledger handover as makePagesTableMode: emitInterlinearPair drains against the SOURCE
      // rows, i.e. the buffered original, so that block's ledger is the one installed while it runs.
      FootnoteLedger parkedFootnotes = adoptBufferedFootnoteLedger();
      renderInterlinear(std::move(bufferedOriginalBlock), std::move(currentTextBlock));
      releaseFootnoteLedger(parkedFootnotes);
    } else {
      makePages();
    }
  } else {
    // Original paragraph: a previous original still buffered never got a translation, so lay it out
    // full-width (flushBufferedOriginal; the "not translated" marker is SideBySide-only, so an
    // unpaired original here simply appears with no annotation row) before buffering this one.
    if (bufferedOriginalBlock) {
      flushBufferedOriginal();
    }
    bufferedOriginalParagraphIdx = currentBlockParagraphIdx;
    // The block's footnote ledger goes into the buffer WITH it — see makePagesTableMode.
    bufferedOriginalFootnotes.swap(pendingFootnotes);
    bufferedOriginalWordsExtracted = wordsExtractedInBlock;
    bufferedOriginalBlock = std::move(currentTextBlock);
  }
}

namespace {

// The WORD stream of a block whose token stream may not be one token per word.
//
// Focus Reading (ParsedText::addWord) splits every word into a BOLD PREFIX plus a regular tail
// marked wordIsFocusSuffix, so "Ok" is stored as "O" + "k". extractLine concatenates the tail back
// before a line leaves ParsedText, which is why the tooltip — which reads laid-out page words — and
// every earlier version of the Interlinear pairing saw whole words. Feeding the raw token array to
// the sentence pairing instead gives it a DIFFERENT text: "Ok." becomes "O" "k" ".", whose match key
// is "O k" (3 bytes) rather than "Ok" (2), so mergeJunkSentences stops folding it and the sentence
// gets an annotation row and a forced line break the tooltip does not give it. The two consumers
// SentencePairing.h promises one answer to then disagree, and only when Focus Reading is on.
//
// So: merge exactly what extractLine merges, and keep the token index of each merged word so the
// annotations coming back in merged-word space can be turned into the token indices layout needs.
struct MergedWordStream {
  std::string arena;              // NUL-separated merged words; EMPTY on the no-split fast path
  std::vector<const char*> ptrs;  // one entry per merged WORD, pointing at arena or at the block
  std::vector<uint16_t> toToken;  // merged index -> first token index; EMPTY means the two coincide

  // Token index for merged index `merged`. Also correct for an EXCLUSIVE end index: merged word
  // `merged` starts at the token one past the end of merged word `merged - 1`, and a past-the-end
  // index maps to the token count.
  size_t tokenAt(const size_t merged, const size_t tokenCount) const {
    if (toToken.empty()) return std::min(merged, tokenCount);
    return merged < toToken.size() ? toToken[merged] : tokenCount;
  }
};

void buildMergedWordStream(const ParsedText& block, MergedWordStream& out) {
  const size_t tokenCount = block.size();
  out.arena.clear();
  out.ptrs.clear();
  out.toToken.clear();

  size_t suffixTokens = 0;
  size_t textBytes = 0;
  for (size_t w = 0; w < tokenCount; w++) {
    textBytes += block.wordAt(w).size();
    if (block.wordIsFocusSuffixAt(w)) suffixTokens++;
  }

  // FAST PATH — Focus Reading off, or a block it did not split (already-bold or CJK text). Nothing
  // to merge, so point straight at the block's own word strings and copy no bytes at all. This is
  // every reader who has not turned the setting on, i.e. the default.
  if (suffixTokens == 0) {
    out.ptrs.reserve(tokenCount);
    for (size_t w = 0; w < tokenCount; w++) out.ptrs.push_back(block.wordAt(w).c_str());
    return;
  }

  // SLOW PATH — one flat NUL-separated arena rather than a vector<std::string>: the paragraph's own
  // text is a few KB, whereas one std::string header per word is 24 B before any heap the long words
  // would need. Sized EXACTLY (every byte of text, one NUL per merged word) and reserved up front, so
  // no append can reallocate; the pointers are still built from offsets afterwards rather than as we
  // go, which keeps that a performance property and not a correctness one.
  const size_t mergedCount = tokenCount - suffixTokens;
  out.arena.reserve(textBytes + mergedCount);
  out.toToken.reserve(mergedCount);
  out.ptrs.reserve(mergedCount);
  std::vector<uint32_t> offsets;
  offsets.reserve(mergedCount);
  for (size_t w = 0; w < tokenCount; w++) {
    if (block.wordIsFocusSuffixAt(w) && !offsets.empty()) {
      out.arena.pop_back();  // drop the terminator: this token continues the word before it
      out.arena += block.wordAt(w);
      out.arena.push_back('\0');
      continue;
    }
    offsets.push_back(static_cast<uint32_t>(out.arena.size()));
    out.toToken.push_back(static_cast<uint16_t>(w));
    out.arena += block.wordAt(w);
    out.arena.push_back('\0');
  }
  for (const uint32_t offset : offsets) out.ptrs.push_back(out.arena.data() + offset);
}

}  // namespace

void ChapterHtmlSlimParser::buildAnnotationRuns(const InterlinearAnnotation& annotation, const ParsedText& transBlock,
                                                const InterlinearBands& bands, const uint16_t measureWidth,
                                                const int annotationFont, std::vector<InterlinearRun>& runs) {
  runs.clear();
  const size_t slots = bands.slots;
  // A sentence whose translation is empty contributes no run at all; its slots stay blank.
  if (annotation.transEndWord <= annotation.transStartWord || slots == 0) return;

  BlockStyle annStyle;
  // ALWAYS Left. The row is positioned by an x read off the real laid-out source — the source
  // block's alignment is already baked into that number, so inheriting it here would apply it twice.
  //
  // textAlignDefined is deliberately left FALSE. It is read in exactly one place — extractLine's
  // "resolved RTL + no explicit text-align + Left" rule, which flips the row to Right — and that is
  // exactly what an RTL TARGET language needs here: the row is laid out at its BAND's width and then
  // placed as a whole box at the band's start, so flipping it right puts a Hebrew / Arabic / Persian
  // translation's first glyph on the band's right edge, i.e. on the reading-order start of the span
  // its source sentence occupies. An RTL SOURCE paragraph never reaches here at all:
  // renderInterlinear's guard refuses to annotate one.
  annStyle.alignment = CssTextAlign::Left;
  // NO first-line indent. Sentence sync used to be an indent on chunk 0, which only works for an LTR
  // target (an RTL row is measured against `measure - indent` and flipped right, so the indent moved
  // it backwards, away from its sentence — and resolveFirstLineIndent discarded it anyway, because
  // isNaturalAlign is false for a Left-aligned RTL block). Placing the whole box at the band start
  // instead is direction-agnostic and gives every row, not just the first, a band of its own.
  //
  // textIndentDefined MUST stay true. With it false and extraParagraphSpacing false,
  // resolveFirstLineIndent falls through to its three-space default and every row is inset.
  annStyle.textIndent = 0;
  annStyle.textIndentDefined = true;

  // extraParagraphSpacing=false keeps resolveFirstLineIndent on the branch that returns the explicit
  // value above verbatim rather than the paragraph-gap branch. hyphenationEnabled=false keeps a long
  // compound wrapping early instead of being broken at 8pt, and focusReading is a body-text
  // affordance that has no business in an annotation.
  ParsedText annotationText(/*extraParagraphSpacing=*/false, /*hyphenationEnabled=*/false,
                            /*focusReadingEnabled=*/false, annStyle);
  // One growth step for the whole span. This block is constructed per SENTENCE, so without it five
  // parallel vectors double from zero for every sentence of every paragraph on the background build
  // path — the variable-size DRAM churn the reserve-before-push_back rule exists to prevent. The span
  // length is the exact token count for every target but CJK, where per-character splitting can add
  // more and the vectors simply fall back to doubling.
  annotationText.reserveAdditionalWords(static_cast<size_t>(annotation.transEndWord) -
                                        static_cast<size_t>(annotation.transStartWord));
  for (uint16_t w = annotation.transStartWord; w < annotation.transEndWord && w < transBlock.size(); w++) {
    // REGULAR explicitly: the 8pt family ships a single face, so bold/italic would resolve back to
    // regular anyway, and dropping the inherited EpdFontFamily::TRANSLATED bit keeps the row plain
    // black rather than the Interleaved gray.
    //
    // attachToPrevious MUST be carried across the re-emit, exactly as flushPartWordBuffer carries it
    // for the parser's own re-emit: the buffered translation is not one token per visual word (see
    // ParsedText::wordAttachesToPrevious), and a dropped flag renders every continuation boundary in
    // the span as a full space. The span's FIRST word is forced false — there is no previous word in
    // this row for it to attach to, so a span that happens to start mid-run does not open with a
    // stray glue.
    const bool attach = w > annotation.transStartWord && transBlock.wordAttachesToPrevious(w);
    annotationText.addWord(transBlock.wordAt(w), EpdFontFamily::REGULAR, /*underline=*/false, attach);
  }
  if (annotationText.isEmpty()) return;

  const int measure = static_cast<int>(measureWidth);
  const int gap = renderer.getSpaceWidth(annotationFont, EpdFontFamily::REGULAR);

  // ONE measuring pass over the whole span, before anything is laid out. `remaining` drives the
  // proportional fill below and `widestToken` is the floor every band width is held to: a width
  // under it sends computeLineBreaks into its oversized-word pre-pass, which splits the word and
  // appends a HYPHEN — a fabricated "continues overleaf" mark on text that does not continue.
  int remaining = 0;
  int widestToken = 0;
  for (size_t w = 0; w < annotationText.size(); w++) {
    const int advance =
        renderer.getTextAdvanceX(annotationFont, annotationText.wordAt(w).c_str(), annotationText.getWordStyleAt(w));
    remaining += advance;
    if (w > 0 && !annotationText.wordAttachesToPrevious(w)) remaining += gap;
    if (advance > widestToken) widestToken = advance;
  }

  // The x a CONTINUATION row syncs to: the left edge of the source line it sits over, so the strip
  // tracks the source line by line. Zero for every left-aligned and justified block — the
  // overwhelming majority of body text — and non-zero only for a centred or right-aligned one.
  const auto srcLineStart = [&bands](const size_t slot) -> int {
    if (bands.srcLines == nullptr) return 0;
    const size_t index = bands.firstLine + slot;
    if (index >= bands.srcLines->size()) return 0;
    const std::shared_ptr<TextBlock>& line = (*bands.srcLines)[index];
    if (!line || line->wordCount() == 0) return 0;
    return std::max<int>(0, line->wordXpos(0));
  };

  runs.reserve(std::min<size_t>(slots, 4));
  for (size_t j = 0; j < slots && !annotationText.isEmpty(); j++) {
    const bool lastSlot = j + 1 == slots;
    // Slot 0 syncs to the SENTENCE's x and may have to clear ink the previous sentence left on that
    // strip; every later slot sits on a strip this sentence owns alone and syncs to the source
    // LINE's own left edge. Only the LAST slot can be shortened by the next source sentence's head,
    // because that is the only line the two can share.
    const int floorX = (j == 0 && bands.floorX > 0) ? bands.floorX + gap : 0;
    const int syncX = std::max<int>((j == 0) ? bands.headX : srcLineStart(j), floorX);
    const int softEnd = lastSlot ? std::max<int>(bands.tailX, syncX + 1) : measure;
    const int slotRoom = std::max(1, softEnd - syncX);
    // How far right this row may run when it stretches. The panel edge always — TextBlock::render
    // does no x culling and GfxRenderer::drawPixel LOG_ERRs every out-of-panel pixel — and only
    // HALFWAY into the next sentence's band when that sentence's head is due on this same strip. A
    // row allowed to eat the whole strip would leave that head a width no token fits, and the DP
    // answers such a width by forcing the oversized token onto the line anyway, past the margin.
    const int hardEnd = (lastSlot && bands.tailX < measure) ? (static_cast<int>(bands.tailX) + measure) / 2 : measure;
    const int hardRoom = hardEnd - floorX;
    // Nothing worth placing is left on this strip. Leaving the slot blank keeps the text in the
    // block for the next one instead of wrapping it at a width no token can fit.
    if (hardRoom <= gap) continue;

    // PROPORTIONAL FILL. Every remaining slot takes the same fraction of its own band, so a
    // translation that would wrap into two rows at the full measure is instead spread over all three
    // strips its source sentence occupies. Packing it into the fewest rows is what left a blank band
    // above roughly every third source line, which reads as two source lines with a gap between
    // them. Recomputed per slot, so an under-full row is made up by the ones after it.
    int capacity = slotRoom;
    if (!lastSlot) capacity += static_cast<int>(slots - j - 2) * measure + std::max<int>(bands.tailX, 1);

    int width;
    if (remaining >= capacity) {
      // The translation is wider than every band this sentence owns. Rule: let it stretch. Take what
      // the text actually needs, up to the whole strip, rather than lose its tail at the band edge —
      // it eats the space the next sentence's annotation would have had, and that sentence is pushed
      // right off this run's rightEdge instead of being overprinted. Asking for exactly `remaining`
      // rather than the whole strip keeps a mildly over-long row on its sentence's x; only a row
      // that cannot fit either way is pulled left off it.
      width = std::min(hardRoom, std::max(slotRoom, remaining));
    } else {
      width = static_cast<int>((static_cast<int64_t>(slotRoom) * remaining) / std::max(capacity, 1));
    }
    width = std::max(width, widestToken);
    width = std::min(width, hardRoom);
    width = std::max(width, 1);

    std::shared_ptr<TextBlock> row;
    annotationText.extractNextLine(renderer, annotationFont, static_cast<uint16_t>(width),
                                   [&row](const std::shared_ptr<TextBlock>& line) { row = line; });
    if (!row || row->wordCount() == 0) continue;  // dropped to an arena OOM: the slot stays blank

    // Ink extent, scanned rather than read off word 0 and word N-1: an RTL row's x table descends
    // when BidiUtils declines to reorder it, so neither end is at a fixed index.
    int inkLeft = row->wordXpos(0);
    int inkRight = inkLeft;
    for (uint16_t i = 0; i < row->wordCount(); i++) {
      const int wordX = row->wordXpos(i);
      if (wordX < inkLeft) inkLeft = wordX;
      const int wordRight = wordX + renderer.getTextAdvanceX(annotationFont, row->wordText(i), row->wordStyle(i));
      if (wordRight > inkRight) inkRight = wordRight;
    }

    // Place the whole BOX at the band start. The row was wrapped at the band's width, so an LTR
    // row's first glyph lands on syncX and an RTL row's last one lands on the band's right edge.
    // Clamped against the INK rather than the box: the DP forces an unbreakable oversized token onto
    // a line when nothing can be hyphenated to fit it, so a row can be wider than what it was
    // measured against, and it is the ink that must stay on the panel.
    // Pulled left against hardEnd rather than the panel edge, so a stretched row also stays out of
    // the half of the next sentence's band that was reserved for it. `width <= hardEnd - floorX` and
    // `inkRight <= width` for every row the DP could actually fit, so this can never land left of
    // floorX; the clamp below is for the row it could not.
    int x = syncX;
    if (x + inkRight > hardEnd) x = hardEnd - inkRight;
    if (x < floorX) x = floorX;
    if (x < 0) x = 0;
    if (x + inkRight > measure) {
      // Unplaceable: a token wider than everything left on this strip. Drawing it would show nothing
      // legible and cost one GfxRenderer LOG_ERR per out-of-panel pixel.
      LOG_ERR("ILN", "Annotation row wider than its strip; row dropped");
      continue;
    }

    InterlinearRun run;
    run.row = std::move(row);
    run.x = static_cast<int16_t>(x);
    run.rightEdge = static_cast<int16_t>(std::min(x + inkRight, measure));
    run.slot = static_cast<uint16_t>(j);
    runs.push_back(std::move(run));

    remaining -= (inkRight - inkLeft) + gap;
    if (remaining < 0) remaining = 0;
  }

  // The only place a translation can still lose text: it did not fit the sentence's whole on-page
  // region even with every row stretched to the panel edge. A further row is not available -- it
  // would put two annotation rows over one source line, the doubling this layout forbids.
  if (!annotationText.isEmpty()) {
    LOG_ERR("ILN", "Annotation overflows %u line(s); %u token(s) dropped", static_cast<unsigned>(slots),
            static_cast<unsigned>(annotationText.size()));
  }
}

void ChapterHtmlSlimParser::placeInterlinearRow(const std::shared_ptr<TextBlock>& row, const int16_t xPos,
                                                const int16_t yPos, const LineFontRole role) {
  auto pageLine = std::make_shared<PageLine>(row, xPos, yPos);
  // Both the source lines and the annotation strips carry the ORIGINAL paragraph's index, so the
  // line->paragraph mapping the overlays and the reader rely on still resolves (same as
  // renderSideBySide: currentBlockParagraphIdx has already advanced past the pair by now).
  pageLine->paragraphIdx = bufferedOriginalParagraphIdx;
  pageLine->fontRole = role;
  if (bufferedOriginalParagraphIdx >= 0) {
    if (currentPage->firstParagraphIdx < 0) {
      currentPage->firstParagraphIdx = bufferedOriginalParagraphIdx;
    }
    currentPage->lastParagraphIdx = bufferedOriginalParagraphIdx;
  }
  currentPage->elements.push_back(std::move(pageLine));
}

void ChapterHtmlSlimParser::emitInterlinearPair(const std::vector<InterlinearRun>& runs,
                                                const std::shared_ptr<TextBlock>& srcLine, const int stripHeight,
                                                const int srcRowHeight, const int16_t leftInset) {
  // ATOMIC FIT, over a FIXED group: exactly one strip plus one source line, every time. The
  // !elements.empty() guard is the one emitHorizontalRule already uses -- an EMPTY page must never be
  // completed or it reaches section.bin as a blank page the reader then displays, and it is also the
  // anti-loop guard: an empty page never breaks, so a group taller than the whole viewport still
  // lands instead of spinning. With the group height now constant there is no degenerate per-row
  // fallback to keep.
  const int groupHeight = stripHeight + srcRowHeight;
  if (!currentPage->elements.empty() && currentPageNextY + groupHeight > viewportHeight) {
    setCurrentPageVisibleOffset(visibleTextOffset);
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex, currentPageVisibleOffset);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // THE STRIP. Reserved unconditionally, drawn only if something landed on it. A blank strip emits
  // no PageLine at all -- no arena, no serialized bytes, no render call -- while still advancing y,
  // which is what keeps the annotation/source pitch dead even and stops two source lines ever
  // becoming adjacent. Two runs at ONE y is the shape renderSideBySide already writes for its two
  // columns, and it is still one visual annotation line.
  const int16_t stripY = static_cast<int16_t>(currentPageNextY);
  for (const InterlinearRun& run : runs) {
    if (run.row)
      placeInterlinearRow(run.row, static_cast<int16_t>(leftInset + run.x), stripY, LineFontRole::Annotation);
  }
  currentPageNextY += stripHeight;

  placeInterlinearRow(srcLine, leftInset, static_cast<int16_t>(currentPageNextY), LineFontRole::Body);
  currentPageNextY += srcRowHeight;

  // FOOTNOTES, attributed to the page carrying the anchor exactly as addLineToPage (:1828-1834) does
  // it. Interlinear needs its own copy because a PAIRED paragraph never reaches addLineToPage, and
  // under this layout essentially every paragraph pairs: without this, pendingFootnotes accumulated
  // for the whole chapter and was either dumped wholesale onto whatever page happened to be current at
  // the next unpaired paragraph (the end-of-block net, flushPendingFootnotesToCurrentPage) or never
  // delivered at all, since finishParse does not drain it.
  //
  // Driven by the SOURCE line only: an annotation strip is synthetic and must not consume source
  // word indices.
  //
  // Every entry here comes from an ORIGINAL block. A translated paragraph is written into the sidecar
  // as a fresh <p> holding nothing but the ESCAPED plain text of the translation
  // (TranslationHtmlRewriter's write-out loop, via appendEscaped), so it carries no
  // <a epub:type="noteref"> and can never push a pending footnote — which is why one counter over the
  // source lines is the whole story here.
  //
  // The indices and this counter share the SOURCE block's base because makePagesInterlinearMode
  // installed that block's ledger (adoptBufferedFootnoteLedger) before calling renderInterlinear: both
  // the entries and the base below are the buffered original's own, and the in-flight block's are parked
  // out of reach. See the matching drain in renderSideBySide for why the old "startNewTextBlock zeroed
  // it" reasoning was not sound. The pre-layout anchor index vs post-layout wordCount() mismatch is
  // addLineToPage's own approximation, kept identical here.
  wordsExtractedInBlock += srcLine->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);
}

// Lay an original paragraph and its paired translation out as ONE full-width flow of STRICTLY
// ALTERNATING rows: annotation strip, source line, annotation strip, source line, all the way down.
//
// THE MODEL, in four rules that are load-bearing and not preferences:
//
// 1. THE SOURCE FLOWS COMPLETELY NORMALLY. It is laid out exactly as LinguaLayout::OriginalOnly would
//    lay it out — same DP, same hyphenation, same justification, same indent, no constraint of any
//    kind. Sentences do NOT each start a new line. Line breaking of the source is left alone and the
//    TRANSLATION is the thing made to fit.
// 2. STRICT ALTERNATION. Every source line of an annotated paragraph gets a strip above it, blank or
//    not, so a source line never directly follows a source line and a strip never follows a strip.
//    The eye tracks one fixed pitch down the page, which is the whole reason an interlinear layout
//    is readable.
// 3. EXACTLY ONE STRIP PER SOURCE LINE — not one per sentence. A sentence spanning three source
//    lines has its translation DISTRIBUTED across the three strips above them: the translation flows
//    too, in small type, one line of it per line of source. Distributed, not packed — wrapping the
//    translation at the full measure and letting it stop when it runs out fills two of those three
//    strips and leaves the third an empty band, which on the page reads as two source lines with a
//    gap between them. buildAnnotationRuns therefore wraps each row at a fraction of its own band, so
//    the text reaches the sentence's last source line.
// 4. SENTENCE SYNC. A sentence's translation begins at the x its SOURCE sentence begins, not wherever
//    the previous translation happened to stop. When a source line holds the tail of one sentence and
//    the head of the next, its strip carries two runs at one y — still one strip, still one y advance
//    — and the two runs are held to disjoint horizontal BANDS, mirroring the split the source line
//    itself has at that x, so they can never print on top of each other.
// 5. OVERFLOW STRETCHES; IT IS NEVER A SECOND ROW AND IS NEVER SILENTLY CUT. A translation longer than
//    its band takes the room it needs, up to the free width of its strip: it eats the space the next
//    sentence's annotation would have had, and that sentence's run is pushed right off this one's
//    rightEdge. A second row is forbidden — it would break rule 2 — so text is lost only when the
//    translation exceeds the sentence's ENTIRE on-page region with every row already stretched to the
//    panel edge, and that case is logged rather than passing silently.
//
// WHY THE PAIRING STILL RUNS FIRST: sentence boundaries are what rule 4 is defined in terms of, and
// they can only be read off the PRE-layout token stream in logical order (buildMergedWordStream).
// What changed is only what the resulting indices are USED for — they no longer constrain line
// breaking, they are handed to layout as TRACKED WORDS (ParsedText::setTrackedWords) whose final
// line and x layout reports back.
//
// GEOMETRY: the strip is placed at the pre-advance currentPageNextY and y advances by exactly the
// strip height whether or not anything was drawn on it; the source line then follows at the new y.
// Same tiling invariant addLineToPage documents, so the two type sizes meet edge to edge with no
// overlap and no gap.
//
// COST: exactly (bodyLineHeight + annotationLineHeight) / bodyLineHeight, about +57% pages at the
// 14pt/8pt portrait default. It no longer degrades with short-sentence prose, because nothing
// short-changes a line any more.
void ChapterHtmlSlimParser::renderInterlinear(std::unique_ptr<ParsedText> origBlock,
                                              std::unique_ptr<ParsedText> transBlock) {
  if (!origBlock || !transBlock) return;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int annotationFont = fontIdForRole(LineFontRole::Annotation);
  // BY VALUE, not by reference: origBlock is released the moment its lines are out (see there), and
  // the spacing reads below would dangle against a reference into the freed block.
  const BlockStyle blockStyle = origBlock->getBlockStyle();
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const int16_t leftInset = blockStyle.leftInset();

  // Guards, evaluated BEFORE layout now that the pairing has to run first.
  //
  // RTL source: extractLine permutes words into VISUAL order for BiDi, so a flat word index is not
  // the logical sentence order — the paragraph renders as plain unannotated source rather than being
  // cut in the wrong places. BOTH RTL flags are needed, because extractLine reorders on
  // `isRtl || hasRtlWord`: blockStyle.isRtl covers a wholly RTL paragraph, containsRtlWord() covers
  // an LTR paragraph carrying an inline Hebrew/Arabic span, which the paragraph probe (first
  // RTL_PARAGRAPH_PROBE_WORDS words only) misses.
  //
  // Reading isRtl here rather than after layout is EQUIVALENT, not an approximation:
  // layoutAndExtractLines only ever auto-resolves isRtl inside `!directionDefined && hasRtlWord`, so
  // when hasRtlWord is false isRtl cannot change, and when it is true the conjunction is already
  // false either way. containsRtlWord() is final as soon as the words are in.
  //
  // The token-count bound is the other half of a uint16_t contract: every index that leaves the
  // pairing (InterlinearAnnotation's three fields, and therefore the tracked-word list) is a
  // uint16_t, and a token index past 65535 would wrap into a valid-looking small one. A paragraph
  // that large cannot occur — the parser soft-flushes at TEXT_BLOCK_SOFT_FLUSH_WORDS — so this is a
  // guard, not a code path.
  const bool annotate = interlinearPairFn != nullptr && !blockStyle.isRtl && !origBlock->containsRtlWord() &&
                        !transBlock->isEmpty() && origBlock->size() <= UINT16_MAX && transBlock->size() <= UINT16_MAX;

  // STEP 0 — pair sentences, then hand their start indices to layout as TRACKED words. They
  // constrain nothing; layout simply reports back which line each landed on and at what x.
  //
  // The word arrays are read PRE-layout, straight out of both blocks in logical order. For the
  // source that is a change of input (it used to be the laid-out lines) and a strictly cleaner one:
  // no "dun-" hyphen fragments exist yet, which is the caveat the post-layout comment used to
  // explain away. What it must NOT change is the tokenization, so both sides go through
  // buildMergedWordStream: with Focus Reading on the raw token array is not words (see there), and
  // the pairing has to be given the same words the tooltip splits or the two disagree about where a
  // sentence begins. Both blocks, and the arenas, outlive this scope's use of the pointers and die
  // well before layoutAndExtractLines mutates or erases the words.
  int annotationCount = 0;
  // Filled by layout: where annotation k's source sentence actually starts (line, x, and whether it
  // opens that line).
  std::vector<ParsedText::TrackedWordPos> sentencePos;
  if (annotate) {
    MergedWordStream srcWords;
    buildMergedWordStream(*origBlock, srcWords);
    MergedWordStream transWords;
    buildMergedWordStream(*transBlock, transWords);

    if (interlinearAnnotations.empty()) interlinearAnnotations.resize(INTERLINEAR_MAX_ANNOTATIONS);
    if (!srcWords.ptrs.empty() && !transWords.ptrs.empty()) {
      annotationCount = interlinearPairFn(srcWords.ptrs.data(), static_cast<int>(srcWords.ptrs.size()),
                                          transWords.ptrs.data(), static_cast<int>(transWords.ptrs.size()),
                                          interlinearAnnotations.data(), INTERLINEAR_MAX_ANNOTATIONS);
    }

    // Back from merged-word space into TOKEN space, which is the only space the rest of this
    // function speaks: tracked words index the block's tokens, and buildAnnotationChunks re-emits
    // transBlock tokens. A no-op when nothing was merged (toToken empty => identity). The mapping is
    // strictly increasing, so the ascending contract checked below is neither created nor destroyed
    // by it, and an empty translation span (start == end) stays empty.
    const size_t srcTokenCount = origBlock->size();
    const size_t transTokenCount = transBlock->size();
    for (int k = 0; k < annotationCount; k++) {
      InterlinearAnnotation& annotation = interlinearAnnotations[k];
      annotation.sourceStartWord = static_cast<uint16_t>(srcWords.tokenAt(annotation.sourceStartWord, srcTokenCount));
      annotation.transStartWord = static_cast<uint16_t>(transWords.tokenAt(annotation.transStartWord, transTokenCount));
      annotation.transEndWord = static_cast<uint16_t>(transWords.tokenAt(annotation.transEndWord, transTokenCount));
    }

    // ONE tracked word per annotation, in the same order, including a leading 0 (which reports
    // line 0 / x = the paragraph indent, and still needs its slot so annotation k lines up with
    // sentencePos[k]). The engine contracts for ascending order; verify rather than trust it,
    // because a non-ascending list would silently break the binary search in ParsedText. A violation
    // drops the whole paragraph back to plain source, the same graceful answer the RTL guard gives.
    std::vector<uint16_t> tracked;
    tracked.reserve(static_cast<size_t>(annotationCount));
    for (int k = 0; k < annotationCount; k++) {
      if (k > 0 && interlinearAnnotations[k].sourceStartWord <= interlinearAnnotations[k - 1].sourceStartWord) {
        LOG_ERR("ILN", "Non-ascending sentence starts; paragraph left unannotated");
        annotationCount = 0;
        tracked.clear();
        break;
      }
      tracked.push_back(interlinearAnnotations[k].sourceStartWord);
    }
    origBlock->setTrackedWords(std::move(tracked));  // layout sizes + fills sentencePos itself
  }

  // STEP 1 — lay the SOURCE out, completely unconstrained. This is byte-for-byte the layout
  // LinguaLayout::OriginalOnly would produce for the same paragraph; the tracked words only ride along.
  std::vector<std::shared_ptr<TextBlock>> srcLines;
  srcLines.reserve(8);
  origBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [&srcLines](const std::shared_ptr<TextBlock>& line) { srcLines.push_back(line); },
      /*includeLastLine=*/true, &sentencePos);
  if (srcLines.empty()) return;

  // The out-param is 1:1 with the tracked list by construction; if it somehow is not, drop the
  // annotations rather than index past the end of it below.
  if (sentencePos.size() != static_cast<size_t>(annotationCount)) {
    annotationCount = 0;
  }

  // Release the SOURCE block now. layoutAndExtractLines erases the consumed words but `erase` never
  // shrinks capacity, so its five parallel token vectors — including one std::string header per word,
  // 24 B each before any long word's own heap block — would otherwise stay pinned through every page
  // this paragraph serializes. Nothing below reads it: the block style was copied by value above and
  // all remaining geometry comes from srcLines and sentencePos.
  origBlock.reset();

  // Top spacing comes from the original block, before the paragraph's first annotation strip, so that
  // strip sits below the margin rather than inside it. It COLLAPSES at the very top of a page: there is
  // nothing above it there for the margin to separate the paragraph from, and it is the ONLY way
  // currentPageNextY can be > 0 on a page with no committed element -- which is precisely the state
  // that would make the atomic fit test below complete a BLANK page (a forced break, e.g. a TOC
  // anchor in flushPendingAnchor, leaves exactly such a page). Collapsing here is also what the
  // common case already does implicitly: when the previous paragraph ended at the page bottom, this
  // spacing is added to the OLD page's y and thrown away with it by the break below.
  if (!currentPage->elements.empty()) {
    if (blockStyle.marginTop > 0) currentPageNextY += blockStyle.marginTop;
    if (blockStyle.paddingTop > 0) currentPageNextY += blockStyle.paddingTop;
  }

  // THE STRIP HEIGHT, and the one place the alternation rule is switched on. A paragraph with NO
  // annotation at all (no pairing function, an RTL source, an empty translation, an unpaired
  // original) reserves nothing and flows as plain source: rules 2 and 3 govern the PAIR, and
  // reserving a blank strip over every line of an untranslated paragraph would burn half the page
  // for nothing.
  const int stripHeight = (annotationCount > 0) ? renderer.getLineHeight(annotationFont, lineCompression) : 0;
  const int bodyLineHeight = renderer.getLineHeight(fontId, lineCompression);
  const int bodyAscender = renderer.getFontAscenderSize(fontId);

  // Runs waiting for the source line they belong to. At most one per sentence that starts on that
  // line — normally one, two when a line carries a tail and a head.
  std::vector<InterlinearRun> pending;
  pending.reserve(2);
  // Right edge of the ink already on the strip `nextLine` is waiting for. What the next sentence's
  // opening row has to clear, so a run that stretched past its band pushes the following sentence
  // right instead of printing on top of it.
  int16_t pendingRightEdge = 0;
  std::vector<InterlinearRun> sentenceRuns;
  sentenceRuns.reserve(4);
  size_t nextLine = 0;  // the next source line to emit

  // Emit source lines up to (not including) `upTo`, each with whatever runs have accumulated for it.
  const auto flushUpTo = [&](const size_t upTo) {
    while (nextLine < upTo && nextLine < srcLines.size()) {
      // Unchanged source pitch, ruby shift included, so furigana headroom survives on annotated
      // lines. Annotation blocks carry no ruby, so a strip's pitch is just the small line height.
      const int srcRowHeight = bodyLineHeight + srcLines[nextLine]->getRubyShift(bodyAscender);
      emitInterlinearPair(pending, srcLines[nextLine], stripHeight, srcRowHeight, leftInset);
      pending.clear();
      pendingRightEdge = 0;
      // The page owns this line now, so release our reference instead of pinning every line of the
      // paragraph until the call returns. Nothing below reads an earlier line. That restores the
      // makePages peak -- one page's worth of TextBlocks, freed as onPageComplete serializes each
      // page -- for a paragraph long enough to span several pages, which is exactly the shape this
      // layout produces most of (see the page-cost estimate in LinguaLayout.h).
      srcLines[nextLine].reset();
      nextLine++;
    }
  };

  // STEP 2 — per sentence, read its band geometry off the SOURCE, then flow its translation through
  // the strips above its own source lines, one row per line.
  for (int s = 0; s < annotationCount; s++) {
    const ParsedText::TrackedWordPos& here = sentencePos[s];
    if (here.line == ParsedText::TrackedWordPos::NOT_PLACED || here.line >= srcLines.size()) continue;
    const size_t firstLine = here.line;

    // The next PLACED sentence bounds this one. `startsLine` is the whole shared-line question and
    // it is exact — no x comparison, no heuristic: if the next sentence opens its line then this one
    // ended on the line before, otherwise the two share that line, and the x the next sentence
    // starts at is where this one's last annotation row wants to stop.
    size_t lastLine = srcLines.size() - 1;
    int16_t tailX = static_cast<int16_t>(effectiveWidth);
    for (int n = s + 1; n < annotationCount; n++) {
      const ParsedText::TrackedWordPos& next = sentencePos[n];
      if (next.line == ParsedText::TrackedWordPos::NOT_PLACED || next.line >= srcLines.size()) continue;
      if (next.startsLine && next.line > firstLine) {
        lastLine = next.line - 1;  // this sentence ended on the line before, and owns it to the margin
      } else {
        lastLine = std::max<size_t>(next.line, firstLine);
        tailX = next.x;  // the two share `lastLine`
      }
      break;
    }
    // Ascending sentence starts were validated before layout and layout preserves their order, so
    // this cannot fire; clamp anyway rather than let `slots` underflow a size_t.
    if (lastLine < firstLine) lastLine = firstLine;
    const size_t slots = lastLine - firstLine + 1;

    InterlinearBands bands;
    bands.srcLines = &srcLines;
    bands.firstLine = firstLine;
    bands.slots = slots;
    bands.headX = here.x;
    bands.tailX = tailX;
    // The strip this sentence opens on may already carry the previous sentence's closing row.
    bands.floorX = (nextLine == firstLine && !pending.empty()) ? pendingRightEdge : 0;

    buildAnnotationRuns(interlinearAnnotations[s], *transBlock, bands, effectiveWidth, annotationFont, sentenceRuns);

    for (InterlinearRun& run : sentenceRuns) {
      // run.slot, not the run's index: a slot whose strip had no room left is skipped, so the two
      // are not the same number once that happens.
      const size_t line = firstLine + run.slot;
      if (line >= srcLines.size()) break;  // defensive: slots are bounded by lastLine
      if (line < nextLine) continue;       // defensive: that line already went out
      flushUpTo(line);                     // everything before it is complete, with its own runs
      pending.push_back(std::move(run));
      pendingRightEdge = pending.back().rightEdge;
    }
    sentenceRuns.clear();
  }

  // Trailing source lines: an unannotated paragraph, the tail past the last sentence, and the line
  // the last pending run is waiting for.
  flushUpTo(srcLines.size());

  // Same end-of-block net makePages keeps: every entry in the installed ledger belongs to the SOURCE
  // paragraph just emitted (the caller parked the in-flight block's ledger before this call), so
  // flushing it to the current page can never touch another paragraph's entry. The per-line drain
  // already covers the normal case — an anchor index can never exceed the block's pre-layout word
  // count, and hyphenation only ADDS post-layout words — so this fires only when a line was dropped (a
  // TextBlock arena OOM).
  flushPendingFootnotesToCurrentPage();

  // Bottom spacing + the usual half-line paragraph gap after the pair.
  if (blockStyle.marginBottom > 0) currentPageNextY += blockStyle.marginBottom;
  if (blockStyle.paddingBottom > 0) currentPageNextY += blockStyle.paddingBottom;
  if (extraParagraphSpacing) {
    currentPageNextY += bodyLineHeight / 2;
  }
}
