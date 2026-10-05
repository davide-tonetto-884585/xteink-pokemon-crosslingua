#pragma once
#include <Print.h>
#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

#include "modules/lingua/engines/TranslationEnginePolicy.h"
#include "modules/lingua/services/TranslationHttpSession.h"

/**
 * SAX-based HTML rewriter that inserts machine-translated paragraphs after
 * each block element in an EPUB chapter.
 *
 * Strategy: reconstruct the original markup from expat callbacks, and after
 * each closing block tag (p, h1-h6, li, blockquote) append a translated
 * paragraph marked with a lang attribute (<p lang="xx">).
 */
class TranslationHtmlRewriter {
 public:
  struct Result {
    int paragraphsTranslated = 0;
    int paragraphsSkipped = 0;
    // Paragraphs whose translation genuinely failed after retries were exhausted (or the
    // chapter was aborted mid-translation). Distinct from paragraphsSkipped, which also
    // counts already-translated/empty-source blocks that were never sent to the engine.
    int translateFailures = 0;
    bool cancelled = false;
    bool abortedOnErrors = false;
    // Subtype of abortedOnErrors: the run was aborted specifically because the
    // heap could not sustain the TLS handshake after repeated bounded waits.
    // Lets the activity show the specific "not enough memory" message instead of
    // a generic failure, while reusing the identical abort/partial-preservation
    // path (abortedOnErrors stays true).
    bool abortedLowMemory = false;
    int alreadyTranslated = 0;  // fill-missing mode: originals that already had a translation
    char errorDetail[64] = {};  // last error message when abortedOnErrors
  };

  // Count translatable block elements in a file without translating.
  // Used for progress bar total.
  static int countBlocksInFile(const std::string& inputPath);

  // "Fill missing" mode: the input is an already-translated chapter. Existing translation blocks
  // (lang=) are kept verbatim and only the original paragraphs WITHOUT a translation right after
  // them are sent to the engine. Must be set before rewrite*/; stays set for the instance.
  void setFillMissingMode(bool on) { keepExistingTranslations = on; }
  // Optional live counters (updated from the worker task during the run) for the progress UI.
  void setLiveCounters(volatile int* translated, volatile int* failed) {
    liveTranslated = translated;
    liveFailed = failed;
  }

  // "Does this chapter HTML already contain translations?" lives in
  // "modules/lingua/services/TranslatedContentDetector.h" (lingua::content::htmlHasTranslatedBlock). It used to be
  // duplicated here as hasEmbeddedTranslations(), keyed on "a block element has ANY lang
  // attribute" -- a different rule from the one the layout engine renders by, which is a
  // lang MISMATCH against the book's language. Section's per-chapter gate needs the same
  // answer, so the check was moved into lib/Epub where both layers can share one definition.

  // Rewrite HTML from `inputBuf` (size `inputSize`) into `out`.
  // Returns summary of what happened.
  Result rewrite(const char* inputBuf, size_t inputSize, Print& out, const char* sourceLang, const char* targetLang,
                 uint8_t engine, const char* apiKey, volatile const bool* cancelled,
                 volatile int* progressOut = nullptr);

  // Invoked at every batch boundary (after a batch's translations are written and
  // progressOut is updated, with the batch's TLS/HTTP transients already torn down).
  // Runs on the calling task. Lets a caller repaint progress at a point where the heap
  // has a clean hole. C-style pointer + context to avoid std::function heap/bloat.
  using BatchBoundaryCb = void (*)(void* ctx);

  // Rewrite HTML from a file on SD card into `out`, reading in 1KB chunks.
  // This avoids loading the entire chapter HTML into memory.
  // Optional boundaryCb fires between batches (see BatchBoundaryCb); pass nullptr to
  // opt out (whole-file/book callers that repaint only at their own boundaries).
  Result rewriteFromFile(const std::string& inputPath, Print& out, const char* sourceLang, const char* targetLang,
                         uint8_t engine, const char* apiKey, volatile const bool* cancelled,
                         volatile int* progressOut = nullptr, BatchBoundaryCb boundaryCb = nullptr,
                         void* boundaryCtx = nullptr);

 private:
  Print* out = nullptr;
  const char* sourceLang = nullptr;
  const char* targetLang = nullptr;
  uint8_t engine = 0;
  const TranslationEnginePolicy* enginePolicy = nullptr;
  const char* apiKey = nullptr;
  volatile const bool* cancelled = nullptr;
  volatile int* progressOut = nullptr;
  BatchBoundaryCb onBatchBoundary = nullptr;  // between-batch repaint hook; null = disabled
  void* batchBoundaryCtx = nullptr;

  // Reusable keep-alive connection shared by every translate() call of one
  // rewriteFromFile() run, so a whole chapter pays for a single TLS handshake
  // instead of one per paragraph. Owned as a local in rewriteFromFile(); this is
  // a non-owning pointer, valid only for that call's duration. Null on the
  // buffer rewrite() path and whenever no session was set up (each translate()
  // then falls back to the stateless HttpDownloader statics — prior behavior).
  TranslationHttpSession* httpSession = nullptr;

  int depth = 0;
  int blockDepth = -1;  // depth where current block element began; -1 = not in block
  bool insideHead = false;
  bool wroteXmlDecl = false;

  std::string blockHtml;     // Reconstructed markup of current block (for output)
  std::string blockText;     // Plain text of current block (for translation)
  std::string blockTagName;  // tag name of current block (e.g., "p", "h1")
  std::string blockClass;    // class attribute of current block

  int skipBlockDepth = -1;  // depth of a block being skipped (existing translation)
  bool blockIsDiv = false;  // current tracked block is a <div>: abandoned if a nested block opens
  bool keepExistingTranslations = false;  // fill-missing mode (see setFillMissingMode)
  bool flushDue = false;  // batch reached its target size; flushed when the NEXT block opens
  int alreadyTranslated = 0;
  volatile int* liveTranslated = nullptr;
  volatile int* liveFailed = nullptr;
  // A paragraph longer than the engine's per-request limit is translated in sentence-bounded
  // chunks (translateText); beyond this it is left untranslated to bound RAM.
  static constexpr size_t MAX_BLOCK_TEXT_BYTES = 12000;
  bool translateWithRetry(const std::string& text, std::string& out);
  bool translateText(const std::string& text, std::string& out);
  void resetRunState();
  void publishLiveCounters(int pendingOk = 0);

  int paragraphsTranslated = 0;
  int paragraphsSkipped = 0;
  int translateFailures = 0;  // genuine translate failures (see Result::translateFailures)
  int blocksProcessed = 0;    // all batch entries including empty blocks (for progress bar)
  bool wasCancelled = false;
  int consecutiveFailures = 0;   // reset on success, increment on failure
  bool abortedOnErrors = false;  // set when consecutiveFailures hits threshold
  std::string lastError;         // last translation error message
  static constexpr int MAX_CONSECUTIVE_FAILURES = 20;

  // ─── Heap backpressure (low-memory wait-then-retry) ─────────────────────────
  // Before firing a batch's TLS request we wait (bounded) for the heap to support
  // the handshake rather than allocating into a low/fragmented heap and crashing.
  // Each exhausted wait (heap never recovered within HEAP_WAIT_TIMEOUT_MS)
  // increments consecutiveHeapTimeouts; a healthy wait resets it. After
  // MAX_CONSECUTIVE_HEAP_TIMEOUTS in a row the run aborts cleanly through the same
  // abortedOnErrors machinery, flagged abortedLowMemory so the caller shows the
  // specific message. Guarantee: progress pauses at most
  // MAX_CONSECUTIVE_HEAP_TIMEOUTS * HEAP_WAIT_TIMEOUT_MS, then continues or ends.
  int consecutiveHeapTimeouts = 0;
  bool abortedLowMemory = false;
  static constexpr int MAX_CONSECUTIVE_HEAP_TIMEOUTS = 3;
  static constexpr uint32_t HEAP_WAIT_TIMEOUT_MS = 12000;  // ~12 s per exhausted wait

  // ─── Network retry/backoff (see HttpDownloader::lastHttpCode) ───────────────
  int consecutive429 = 0;  // consecutive HTTP 429 responses; reset on any non-429 outcome

  // Classify a failed translate attempt (httpCode = HttpDownloader::lastHttpCode captured
  // right after the failing call). Updates consecutive429 and, on a non-retryable outcome,
  // sets abortedOnErrors (auth failure or too many consecutive 429s).
  // Returns true if the attempt loop should retry.
  bool shouldRetryAfterFailure(int httpCode);

  // ─── Batch buffering ─────────────────────────────────────────────────────
  struct BatchEntry {
    std::string htmlBefore;    // all HTML output accumulated before this entry's translation slot
    std::string trimmedText;   // plain text to translate (empty = untranslatable, skip)
    std::string blockTagName;  // tag name of original block (e.g., "p", "h1")
    std::string blockClass;    // class attribute of original block (for CSS spacing)
  };
  std::vector<BatchEntry> batch;
  std::string pendingHtml;    // accumulates writeOut() calls between block flushes
  size_t batchTextBytes = 0;  // running total of trimmedText bytes in current batch
  // Running total of every byte the batch holds (markup + text). batchTextBytes alone only counts
  // text still to translate: in fill-missing mode a mostly-translated chapter never reaches the text
  // target, and the whole chapter (originals + kept translations) piled up in RAM until the heap
  // was exhausted on the X4 Pro. The batch is also flushed on these bounds.
  size_t batchBufferedBytes = 0;
  static constexpr size_t MAX_BATCH_BUFFERED_BYTES = 8 * 1024;
  static constexpr size_t MAX_BATCH_ENTRIES = 32;

  static const char* BLOCK_TAGS[];
  static const int NUM_BLOCK_TAGS;

  static bool isBlockTag(const char* name);

  // Append XML-escaped version of `s` to `buf`
  static void appendEscaped(const char* s, size_t len, std::string& buf);

  // Write to pendingHtml buffer (not directly to out)
  void writeOut(const char* s, size_t len);
  void writeOut(const std::string& s);

  // Write directly to output Print stream (bypasses buffer)
  void writeRaw(const char* s, size_t len);
  void writeRaw(const std::string& s);

  // Build opening tag string "<name attr1=...>"
  static std::string makeOpenTag(const XML_Char* name, const XML_Char** atts);

  // Called when a block element closes: accumulate into batch
  void flushBlock(const char* endTagName);

  // Translate all accumulated batch entries in one API call, write to out
  void flushBatch();

  // Split string by "\n\n" separator
  static std::vector<std::string> splitByDoubleLF(const std::string& s);

  static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL onEnd(void* ud, const XML_Char* name);
  static void XMLCALL onChars(void* ud, const XML_Char* s, int len);
  static void XMLCALL onDefault(void* ud, const XML_Char* s, int len);

  // Counting-only callbacks (for countBlocksInFile)
  static void XMLCALL onStartCount(void* ud, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL onEndCount(void* ud, const XML_Char* name);
};
