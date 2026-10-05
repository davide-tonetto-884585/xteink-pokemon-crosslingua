#include "modules/lingua/activities/BookTranslationActivity.h"

#include <Epub/Section.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "modules/lingua/LinguaModeCatalog.h"
#include "modules/lingua/activities/LanguagePickerActivity.h"
#include "modules/lingua/services/HeapBackpressure.h"
#include "modules/lingua/ui/LinguaTouch.h"
#include "modules/lingua/ui/TranslationProgressUi.h"
#include "network/HttpDownloader.h"

// Sentinel value LanguagePickerActivity returns for the synthetic "Auto-detect" entry.
// Matches CrossPointSettings::sourceTranslationLanguage's 0xFF sentinel.
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

// Bounded wait for TLS-ready heap before a chapter's handshake. The per-batch
// backpressure inside the rewriter is the finer gate and owns the low-memory abort;
// this just gives a heap fragmented by the previous chapter a chance to recover before
// the next handshake begins.
static constexpr uint32_t CHAPTER_START_HEAP_WAIT_MS = 12000;

// ─── epub (re)loading ───────────────────────────────────────────────────────

bool BookTranslationActivity::ensureEpubLoaded() {
  if (epub) return true;
  LOG_DBG("BKT", "Loading lean epub (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS, and don't rebuild the cache if it is missing.
  if (!epub->load(false, true)) {
    LOG_ERR("BKT", "Failed to load epub: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  LOG_DBG("BKT", "Lean epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  return true;
}

void BookTranslationActivity::returnToCaller() {
  if (returnTarget == TranslationReturnTarget::FILE_BROWSER) {
    // Started from the file browser (no reader was open) — return to the book's folder.
    activityManager.goToFileBrowser(FsHelpers::extractFolderPath(epubPath));
    return;
  }
  activityManager.goToReader(epubPath);
}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void BookTranslationActivity::onEnter() {
  Activity::onEnter();

  if (!ensureEpubLoaded()) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    requestUpdate();
    return;
  }

  totalChapters = epub->getSpineItemsCount();
  launchSourcePicker();
}

void BookTranslationActivity::onExit() {
  Activity::onExit();

  // Signal the worker to bail at the next batch boundary and wait briefly for it.
  // The 5-second cap is enough for a partially-translated chapter to abort cleanly;
  // longer waits would block UI navigation.
  cancelFlag = true;
  if (taskHandle) {
    for (int i = 0; i < 50 && !taskDone && !taskFailed; i++) {
      delay(100);
    }
    taskHandle = nullptr;
  }

  // Drop the WiFi link to free heap before the next activity. Mirrors ChapterTranslation.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Always hand the framebuffer back before leaving: returnToCaller() relaunches an
  // activity that renders immediately. onExit() runs while ActivityManager holds the
  // RenderLock, so restore without taking it again. Idempotent — a no-op if we never
  // released or already restored. cancelFlag (set above) has already broken any
  // boundary spin, so the worker is not holding the run open. Done after the WiFi
  // teardown so the realloc has the most free heap available.
  restoreFramebuffer(/*alreadyLocked=*/true);
}

// ─── language pickers ─────────────────────────────────────────────────────────

void BookTranslationActivity::launchSourcePicker() {
  state = SOURCE_LANG_SELECTION;

  // Seed the picker with the user's last source-language choice (0xFF => "auto").
  const uint8_t initial = SETTINGS.sourceTranslationLanguage;
  startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                  /*includeAutoDetect=*/true,
                                                                  /*initialSelection=*/initial,
                                                                  /*customTitle=*/tr(STR_SOURCE_LANGUAGE)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToCaller();
                             return;
                           }
                           const auto& menu = std::get<MenuResult>(result.data);
                           onSourceLangSelected(static_cast<uint8_t>(menu.action));
                         });
}

void BookTranslationActivity::launchTargetPicker() {
  state = LANG_SELECTION;

  // Seed with the user's persisted target choice if any (0xFF => default to 0).
  const uint8_t initial = SETTINGS.translationLanguage == 0xFF ? 0 : SETTINGS.translationLanguage;
  startActivityForResult(std::make_unique<LanguagePickerActivity>(renderer, mappedInput,
                                                                  /*includeAutoDetect=*/false,
                                                                  /*initialSelection=*/initial,
                                                                  /*customTitle=*/tr(STR_TARGET_LANGUAGE)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToCaller();
                             return;
                           }
                           const auto& menu = std::get<MenuResult>(result.data);
                           onTargetLangSelected(static_cast<uint8_t>(menu.action));
                         });
}

void BookTranslationActivity::onSourceLangSelected(uint8_t resultIndex) {
  if (resultIndex == AUTO_DETECT_SENTINEL) {
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
    SETTINGS.sourceTranslationLanguage = AUTO_DETECT_SENTINEL;
  } else if (resultIndex < LanguagePickerActivity::NUM_LANGUAGES) {
    sourceLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
    sourceLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
    SETTINGS.sourceTranslationLanguage = resultIndex;
  } else {
    // Defensive: out-of-range. Fall back to auto-detect.
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
  }
  SETTINGS.saveToFile();
  LOG_DBG("BKT", "Source language: %s (%s)", sourceLangName.c_str(), sourceLangCode.c_str());
  launchTargetPicker();
}

void BookTranslationActivity::onTargetLangSelected(uint8_t resultIndex) {
  if (resultIndex >= LanguagePickerActivity::NUM_LANGUAGES) {
    // Defensive: out-of-range. Cancel rather than translate to an unknown code.
    LOG_ERR("BKT", "Target language out of range: %d", resultIndex);
    returnToCaller();
    return;
  }
  targetLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
  targetLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
  SETTINGS.translationLanguage = resultIndex;
  SETTINGS.saveToFile();
  LOG_DBG("BKT", "Target language: %s (%s)", targetLangName.c_str(), targetLangCode.c_str());

  // Scan for already-translated chapters before connecting WiFi.
  scanAlreadyTranslated();
}

// ─── lingua scan ─────────────────────────────────────────────────────

void BookTranslationActivity::scanAlreadyTranslated() {
  alreadyTranslatedCount = 0;
  const auto& cachePath = epub->getCachePath();
  for (int i = 0; i < totalChapters; i++) {
    const std::string path = cachePath + "/sections/" + std::to_string(i) + ".translated.html";
    // The final file appears only via the atomic ".part" -> rename commit, so its mere
    // existence means a complete translation (an interrupted run leaves only a ".part").
    if (Storage.exists(path.c_str())) {
      alreadyTranslatedCount++;
    }
  }

  LOG_DBG("BKT", "Pre-scan: %d / %d chapters already translated", alreadyTranslatedCount, totalChapters);

  if (alreadyTranslatedCount > 0) {
    state = CONFIRM_RETRANSLATE;
    confirmSelection = 0;
    requestUpdate();
  } else {
    // No existing translations -- go straight to WiFi.
    launchWifiOrStart();
  }
}

// ─── WiFi gate ────────────────────────────────────────────────────────────────

void BookTranslationActivity::launchWifiOrStart() {
  state = WIFI_SELECTION;
  requestUpdate();

  WiFi.mode(WIFI_STA);
#if defined(SIMULATOR)
  // The simulator's fake scan is not understood by CrossInk's WiFi picker (it reports the scan as
  // finished instead of running), so join the fake network directly. Requests still go out for real.
  if (WiFi.status() != WL_CONNECTED) WiFi.begin();
#endif
  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiConnected(!result.isCancelled); });
}

void BookTranslationActivity::onWifiConnected(bool success) {
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

// ─── translation task ─────────────────────────────────────────────────────────

void BookTranslationActivity::startTranslation() {
  state = TRANSLATING;
  cancelFlag = false;
  taskDone = false;
  taskFailed = false;
  lowMemoryAbort = false;
  currentChapter = 0;
  chaptersCompleted = 0;
  progressCurrent = 0;
  progressTotal = 0;
  lastProgressUpdate = 0;
  statusMsg[0] = '\0';
  liveTranslated = 0;
  liveFailed = 0;
  bookTranslated = 0;
  bookFailed = 0;
  bookAlreadyTranslated = 0;
  runStartMillis = millis();
  runEndMillis = 0;
  boundaryPending = false;
  boundaryAck = false;
  lastRepaintProgress = 0;
  // Seed the repaint clock at run start so the first time-based repaint fires ~20 s in,
  // not immediately at the first batch boundary.
  lastRepaintMillis = millis();

  // Flush the initial "Translating..." screen to the panel BEFORE freeing the
  // framebuffer (blocks until drawn+displayed). Safe here: startTranslation() runs on
  // the main task from a result handler, which does not hold the RenderLock.
  requestUpdateAndWait();

  LOG_DBG("BKT", "State -> TRANSLATING: %d chapters, lang=%s, engine=%d", totalChapters, targetLangCode.c_str(),
          SETTINGS.translationEngine);
  LOG_DBG("MEM", "BKT pre-task (wifi up): free=%u max=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  // Free the 48 KB framebuffer for the first chapter's TLS handshake. Restored and
  // re-released at each subsequent chapter boundary; finally restored in loop()'s
  // completion path (and unconditionally in onExit()).
  releaseFramebuffer();

  // 10 KB stack matches ChapterTranslation: ParagraphTranslator can spike to ~6-8 KB
  // during HTTP + JSON parse on the larger engines. Priority 1 keeps it below the
  // render task.
  xTaskCreate(translationTask, "bookTranslate", 10240, this, 1, &taskHandle);
}

// ─── framebuffer lifecycle (main task only) ────────────────────────────────────

void BookTranslationActivity::releaseFramebuffer() {
  // RenderLock serialises against the render task: freeing the buffer mid-draw would
  // be a use-after-free.
  RenderLock lock;
  if (!renderer.hasFrameBuffer()) return;  // already released
  if (!renderer.releaseFrameBufferForNetwork()) {
    LOG_ERR("BKT", "Framebuffer release failed");
    return;
  }
  LOG_DBG("MEM", "BKT post-release: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void BookTranslationActivity::restoreFramebuffer(bool alreadyLocked) {
  if (renderer.hasFrameBuffer()) return;  // idempotent: nothing to restore

  for (int attempt = 0; attempt < 5; attempt++) {
    bool ok;
    if (alreadyLocked) {
      // onExit() already holds the RenderLock via exitActivity(); taking the
      // non-recursive mutex again on the same task would deadlock.
      ok = renderer.restoreFrameBufferAfterNetwork();
    } else {
      RenderLock lock;
      if (renderer.hasFrameBuffer()) return;  // re-check under the lock
      ok = renderer.restoreFrameBufferAfterNetwork();
    }
    if (ok) {
      LOG_DBG("MEM", "BKT post-restore: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    LOG_ERR("BKT", "Framebuffer realloc failed (attempt %d/5)", attempt + 1);
    delay(100);
  }

  // Practically unreachable: restore runs only after the previous chapter's
  // rewriter/HTTP/expat transients (~35+ KB) have been freed, so a clean 48 KB hole
  // is available. If it still fails the device has no buffer to draw on; restart to
  // recover. Finished chapters are already committed to SD and the reader re-reads
  // them on relaunch.
  LOG_ERR("BKT", "Framebuffer realloc permanently failed; restarting");
  ESP.restart();
}

bool BookTranslationActivity::tryRestoreFramebuffer() {
  if (renderer.hasFrameBuffer()) return true;  // already present
  RenderLock lock;
  if (renderer.hasFrameBuffer()) return true;  // re-check under the lock
  if (renderer.restoreFrameBufferAfterNetwork()) {
    LOG_DBG("MEM", "BKT boundary restore: free=%u max=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap());
    return true;
  }
  // No contiguous 48 KB hole right now (the keep-alive TLS session is still open mid
  // chapter). Skip this cosmetic repaint; the next boundary retries. Never restart here —
  // a mid-book reboot would drop the in-flight chapter's work.
  LOG_DBG("BKT", "Boundary framebuffer restore unavailable; skipping repaint");
  return false;
}

void BookTranslationActivity::translationTask(void* param) {
  auto* self = static_cast<BookTranslationActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

void BookTranslationActivity::runTranslation() {
  // Force public DNS so engine-specific subdomains resolve consistently.
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
#ifndef SIMULATOR
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
#else
  (void)dns1;
  (void)dns2;
#endif
  delay(500);
  LOG_DBG("BKT", "DNS set to 8.8.8.8 / 8.8.4.4");

  // Defensive: the lean Epub is normally loaded in onEnter, but guard the task entry too.
  // This one Epub stays open for the whole run (re-extracted per chapter below).
  if (!ensureEpubLoaded()) {
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    taskFailed = true;
    return;
  }

  const auto& cachePath = epub->getCachePath();

  // The sections/ subdir is normally created by Section::createSectionFile when the
  // user opens a chapter; chapters they have never visited won't have it yet.
  const std::string sectionsDir = cachePath + "/sections";
  Storage.mkdir(sectionsDir.c_str());

  for (int si = 0; si < totalChapters; si++) {
    if (cancelFlag) break;

    currentChapter = si;
    progressCurrent = 0;
    progressTotal = 0;
    // progressCurrent restarts from 0 for this chapter, so reset the batch-repaint
    // baseline too; otherwise the carried-over value from the previous chapter would
    // suppress the first few per-chapter repaints.
    lastRepaintProgress = 0;

    const std::string translatedPath = cachePath + "/sections/" + std::to_string(si) + ".translated.html";

    // Skip if already translated and user chose "Skip Translated". The final file exists
    // only after the atomic ".part" -> rename commit, so its presence means a complete
    // translation. Skipped chapters do no network work, so they need no framebuffer cycle
    // or repaint (which would cost a slow E-ink refresh each) — just advance the overall
    // counter and move on.
    // "Missing only": an already-translated chapter is not skipped blindly any more. It is
    // re-read in fill-missing mode, which keeps its translations and only sends the paragraphs
    // that have none (failed requests, over-long paragraphs...). A complete chapter makes no
    // network request at all, so it does not need the boundary repaint below.
    const bool fillChapter = skipTranslated && Storage.exists(translatedPath.c_str());

    // Chapter boundary: for every chapter except the one the initial startTranslation()
    // screen already shows (chapter 0 with nothing completed), ask the main task to
    // restore the framebuffer, repaint progress, and free it again before this
    // chapter's network work. The worker owns no framebuffer/render state — it only
    // raises boundaryPending and waits — so the restore/redraw/release can never race
    // the render task or onExit()'s teardown. The spin also exits on cancelFlag so
    // onExit() never blocks waiting for us. The freed transients from the previous
    // iteration (rewriter/HalFiles went out of scope) guarantee a clean 48 KB hole.
    if (!fillChapter && (currentChapter != 0 || chaptersCompleted != 0)) {
      boundaryAck = false;
      boundaryPending = true;
      while (!boundaryAck && !cancelFlag) {
        delay(5);
      }
      boundaryPending = false;
    }
    if (cancelFlag) break;

    // Heap backpressure before this chapter's network work: if fragmentation carried over
    // from a prior chapter left the heap below the handshake floor, wait (bounded) for it
    // to recover before opening files and starting the TLS handshake, rather than
    // allocating into a low heap. Best-effort and non-terminal here — the per-batch
    // backpressure inside the rewriter is the finer gate and owns the low-memory abort.
    // Services cancelFlag so onExit() never blocks; nothing is open yet, so a cancel here
    // just breaks with no cleanup.
    heapbp::waitForHeap(HttpDownloader::MIN_FREE_HEAP_FOR_TLS, HttpDownloader::MIN_MAX_ALLOC_FOR_TLS,
                        CHAPTER_START_HEAP_WAIT_MS, &cancelFlag, "BKT", "chapter-start TLS heap");
    if (cancelFlag) break;

    // Extract chapter HTML from the EPUB to a temp scratch file.
    const auto& spineItem = epub->getSpineItem(si);
    const std::string tmpPath = fillChapter ? translatedPath : cachePath + "/.tmp_book_" + std::to_string(si) + ".html";

    HalFile tmpFile;
    if (fillChapter) {
      // Input is the existing bilingual file itself; nothing to extract.
    } else if (!Storage.openFileForWrite("BKT", tmpPath, tmpFile)) {
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to create temp file", si + 1);
      taskFailed = true;
      return;
    }
    if (!fillChapter) {
      if (!epub->readItemContentsToStream(spineItem.href, tmpFile, 1024)) {
        tmpFile.close();
        Storage.remove(tmpPath.c_str());
        snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to extract HTML", si + 1);
        taskFailed = true;
        return;
      }
      tmpFile.close();  // Must close before re-opening for read in the rewriter step.
    }

    // Pre-scan for the progress-bar denominator. countBlocksInFile is a pure SAX pass
    // and is cheap relative to the network-bound translation step.
    progressTotal = TranslationHtmlRewriter::countBlocksInFile(tmpPath);

    LOG_DBG("BKT", "Ch %d/%d: href=%s, blocks=%d", si + 1, totalChapters, spineItem.href.c_str(), (int)progressTotal);

    // Write to a ".part" file and only rename it into the final path after a clean result —
    // the same crash-safe commit protocol as ChapterTranslationActivity. A power loss
    // mid-chapter leaves only a ".part" (ignored); the final file appears only via the
    // rename, so any prior committed translation survives a failed re-translation.
    const std::string partPath = translatedPath + ".part";
    Storage.remove(partPath.c_str());  // clear any stale partial from an interrupted run

    HalFile outFile;
    if (!Storage.openFileForWrite("BKT", partPath, outFile)) {
      if (!fillChapter) Storage.remove(tmpPath.c_str());
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to create output", si + 1);
      taskFailed = true;
      return;
    }

    const char* srcLang = sourceLangCode.c_str();
    TranslationHtmlRewriter rewriter;
    rewriter.setFillMissingMode(fillChapter);
    liveTranslated = 0;
    liveFailed = 0;
    rewriter.setLiveCounters(&liveTranslated, &liveFailed);
    const auto result = rewriter.rewriteFromFile(
        tmpPath, outFile, srcLang, targetLangCode.c_str(), SETTINGS.translationEngine, SETTINGS.translateApiKey,
        &cancelFlag, &progressCurrent, &BookTranslationActivity::batchBoundaryTrampoline, this);
    outFile.close();
    if (!fillChapter) Storage.remove(tmpPath.c_str());
    // Fold this chapter into the book totals (the live counters restart for the next one).
    bookTranslated = bookTranslated + result.paragraphsTranslated;
    bookFailed = bookFailed + result.translateFailures;
    bookAlreadyTranslated = bookAlreadyTranslated + result.alreadyTranslated;
    liveTranslated = 0;
    liveFailed = 0;

    LOG_DBG("BKT", "Ch %d/%d: translated=%d, skipped=%d, failed=%d, errors=%d, cancelled=%d", si + 1, totalChapters,
            result.paragraphsTranslated, result.paragraphsSkipped, result.translateFailures,
            result.abortedOnErrors ? 1 : 0, result.cancelled ? 1 : 0);

    if (cancelFlag || result.cancelled) {
      // Drop the partial output of the in-flight chapter. Finished chapters retain
      // their committed .translated.html on disk.
      Storage.remove(partPath.c_str());
      break;
    }

    if (result.abortedOnErrors) {
      Storage.remove(partPath.c_str());
      if (result.abortedLowMemory) {
        // Specific low-memory abort: render() draws tr(STR_TRANSLATION_LOW_MEMORY)
        // directly (the translated text does not fit statusMsg's 64-byte buffer).
        // Completed chapters remain committed on disk; only this chapter's .part was dropped.
        lowMemoryAbort = true;
      } else if (result.errorDetail[0]) {
        snprintf(statusMsg, sizeof(statusMsg), "Ch %d: %s", si + 1, result.errorDetail);
      } else {
        snprintf(statusMsg, sizeof(statusMsg), "Ch %d: too many API errors", si + 1);
      }
      taskFailed = true;
      return;
    }

    // A chapter that HAD translatable content but translated none of it (every paragraph
    // failed) must not be committed as a bogus ".translated.html" — abort so it can be
    // retried. (A cover / image-only / all-already-translated chapter has zero failures and
    // is a valid passthrough, committed below.)
    if (fillChapter && result.paragraphsTranslated == 0) {
      // Nothing new for an already-translated chapter: keep its file as it is.
      Storage.remove(partPath.c_str());
      chaptersCompleted = chaptersCompleted + 1;
      continue;
    }
    if (result.paragraphsTranslated == 0 && result.translateFailures > 0) {
      // Every request of this chapter failed: do not commit it, but keep going with the rest of
      // the book (its failures are counted and "Retry missing" on the summary picks it up).
      Storage.remove(partPath.c_str());
      LOG_ERR("BKT", "Ch %d: no paragraphs translated; continuing", si + 1);
      continue;
    }

    // Commit atomically: remove any prior final output, then promote ".part" -> final. A
    // chapter with zero translatable content (cover / image-only / all already translated)
    // is a valid passthrough result, so it commits like any other.
    Storage.remove(translatedPath.c_str());
    if (!Storage.rename(partPath.c_str(), translatedPath.c_str())) {
      Storage.remove(partPath.c_str());
      snprintf(statusMsg, sizeof(statusMsg), "Ch %d: failed to finalize", si + 1);
      taskFailed = true;
      return;
    }
    Section::invalidatePageCaches(translatedPath);

    chaptersCompleted = chaptersCompleted + 1;  // ++ on volatile is deprecated in C++20
    LOG_DBG("BKT", "Ch %d/%d: done (%d translated, %d skipped), free=%d", si + 1, totalChapters,
            result.paragraphsTranslated, result.paragraphsSkipped, (int)ESP.getFreeHeap());

    // Let the heap recover between chapters — prevents fragmentation from accumulating
    // across long runs.
    delay(500);
  }

  taskDone = true;
}

// ─── periodic progress repaint (worker-side cadence + UI handshake) ────────────

void BookTranslationActivity::batchBoundaryTrampoline(void* ctx) {
  static_cast<BookTranslationActivity*>(ctx)->serviceBatchBoundary();
}

void BookTranslationActivity::serviceBatchBoundary() {
  // Runs on the worker (bookTranslate) task between batches: this batch's TLS/HTTP
  // transients are freed and progressCurrent is up to date. Bail immediately if we're
  // cancelling so onExit() (which sets cancelFlag then waits for the task to finish) never
  // blocks on a spin.
  if (cancelFlag) return;

  const int current = progressCurrent;
  const int total = progressTotal;
  // Repaint every max(5, total/10) blocks -> ~10-15 repaints across a chapter.
  const int blockThreshold = (total / 10) > 5 ? (total / 10) : 5;
  const unsigned long now = millis();
  const bool progressHit = (current - lastRepaintProgress) >= blockThreshold;
  const bool timeHit = (now - lastRepaintMillis) >= 20000UL;  // millis(); no Date APIs
  if (!progressHit && !timeHit) return;

  // Hand off to the main task: its loop() restores the framebuffer, draws the updated
  // progress synchronously, frees it again, then sets boundaryAck. We own no
  // framebuffer/render state here, so the restore/redraw/release can never race the render
  // task or onExit()'s teardown. The spin also exits on cancelFlag.
  boundaryAck = false;
  boundaryPending = true;
  while (!boundaryAck && !cancelFlag) {
    delay(5);
  }
  boundaryPending = false;

  // Advance the cadence baselines even if a cancel broke the spin — the next boundary
  // (if any) then re-evaluates from here rather than firing again instantly.
  lastRepaintProgress = current;
  lastRepaintMillis = now;
}

// ─── engine name helper ──────────────────────────────────────────────────────

const char* BookTranslationActivity::getEngineName() const {
  switch (SETTINGS.translationEngine) {
    case CrossPointSettings::ENGINE_GOOGLE_FREE:
      return tr(STR_ENGINE_GOOGLE_FREE);
    case CrossPointSettings::ENGINE_DEEPL:
      return tr(STR_ENGINE_DEEPL);
    case CrossPointSettings::ENGINE_DEEPL_PRO:
      return tr(STR_ENGINE_DEEPL_PRO);
    case CrossPointSettings::ENGINE_OPENAI:
      return tr(STR_ENGINE_OPENAI);
    case CrossPointSettings::ENGINE_DEEPSEEK:
      return tr(STR_ENGINE_DEEPSEEK);
    case CrossPointSettings::ENGINE_GEMINI:
      return tr(STR_ENGINE_GEMINI);
    case CrossPointSettings::ENGINE_GOOGLE_V2:
      return tr(STR_ENGINE_GOOGLE_V2);
    case CrossPointSettings::ENGINE_GOOGLE_HTML:
      return tr(STR_ENGINE_GOOGLE_HTML);
    case CrossPointSettings::ENGINE_AZURE:
      return tr(STR_ENGINE_AZURE);
    default:
      return "Unknown";
  }
}

// ─── loop / render ────────────────────────────────────────────────────────────

void BookTranslationActivity::loop() {
  // Touch fallback (X4 Pro): bottom strip = Back, elsewhere = Confirm / pick the row under the finger.
  int tapX = 0;
  int tapY = 0;
  const LinguaTouch::Tap tap = (state == CHOOSE_DISPLAY_MODE)
                                   ? LinguaTouch::Tap::None
                                   : LinguaTouch::readTap(mappedInput, renderer, &tapX, &tapY);
  // CONFIRM_RETRANSLATE: Skip Translated vs Re-translate All two-option menu.
  if (state == CONFIRM_RETRANSLATE) {
    if (tap == LinguaTouch::Tap::Confirm) {
      for (int i = 0; i < optionCount; i++) {
        if (TranslationProgressUi::rectContains(optionRects[i], tapX, tapY)) {
          confirmSelection = i;
          skipTranslated = (confirmSelection == 0);
          launchWifiOrStart();
          return;
        }
      }
      return;
    }
    if (tap == LinguaTouch::Tap::Back) {
      returnToCaller();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      confirmSelection = 1 - confirmSelection;  // toggle 0 / 1
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      skipTranslated = (confirmSelection == 0);
      LOG_DBG("BKT", "Re-translate choice: %s", skipTranslated ? "Skip Translated" : "Re-translate All");
      launchWifiOrStart();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToCaller();
      return;
    }
    return;
  }

  // TRANSLATING: service chapter boundaries, then poll task status.
  if (state == TRANSLATING) {
    // The worker is paused at a chapter boundary: restore the buffer, draw the updated
    // progress synchronously, free it again, then release the worker. All of this runs
    // on the main task, so it never races the render task; and because onExit() runs on
    // this same task it can never overlap this sequence. Claim boundaryPending up front
    // so a second loop() pass cannot re-run the cycle.
    if (boundaryPending) {
      boundaryPending = false;
      // A mid-chapter batch boundary can momentarily lack a contiguous 48 KB hole. Waiting
      // here is pointless: the worker is parked spinning on boundaryAck with the keep-alive
      // TLS session's buffers held, so nothing can free memory while we stall — a wait only
      // freezes the UI (with cancel unresponsive, since this very task services input). Go
      // straight to the non-restarting restore and skip the repaint if there is no hole (the
      // next boundary retries) rather than reboot mid-book. When the restore succeeds we
      // always release the buffer again before acking, so it is never held across the
      // worker's network calls. Chapter boundaries hit this same path but always have a
      // clean hole.
      if (tryRestoreFramebuffer()) {
        requestUpdateAndWait();
        releaseFramebuffer();
      }
      boundaryAck = true;
      return;
    }

    if (taskDone) {
      restoreFramebuffer();  // bring the buffer back BEFORE the result screen draws
      runEndMillis = millis();
      // Summary first (counts, time, retry when paragraphs are missing); Continue then offers
      // the display-mode chooser.
      state = cancelFlag ? CANCELLED : DONE;
      doneSelection = 0;
      requestUpdate();
    } else if (taskFailed) {
      runEndMillis = millis();
      restoreFramebuffer();
      state = FAILED;
      requestUpdate();
    } else {
      // The framebuffer is released during each chapter's network run, so
      // hasFrameBuffer() is false and no repaints are issued — progress advances only
      // at chapter boundaries by design. Gate on the buffer so we resume normal
      // throttled repaints if a release ever failed.
      const unsigned long now = millis();
      if (renderer.hasFrameBuffer() && now - lastProgressUpdate >= 3000) {
        lastProgressUpdate = now;
        requestUpdate();
      }
    }
  }

  // Display-mode chooser: Up/Down move the highlight, Confirm persists the choice (guarded
  // save, mirroring the Lingua submenu) and exits, Back skips and exits. Both exits use the
  // normal return path so the relaunched reader picks up the mode from settings.
  if (state == CHOOSE_DISPLAY_MODE) {
    switch (displayModeChooser.handleInput(mappedInput)) {
      case LinguaModeChooser::Result::Redraw:
        requestUpdate();
        return;
      case LinguaModeChooser::Result::Picked: {
        const uint8_t chosen = static_cast<uint8_t>(LINGUA_SELECTABLE_MODES[displayModeChooser.selected()]);
        if (SETTINGS.translationDisplayMode != chosen) {  // guard SPIFFS write on no-op selections
          SETTINGS.translationDisplayMode = chosen;
          SETTINGS.saveToFile();
          LOG_DBG("BKT", "Display mode set to %d after translation", (int)chosen);
        }
        returnToCaller();
        return;
      }
      case LinguaModeChooser::Result::Cancelled:
        returnToCaller();
        return;
      case LinguaModeChooser::Result::None:
        return;
    }
    return;
  }

  if (state == DONE) {
    if (optionCount > 1 && (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                            mappedInput.wasReleased(MappedInputManager::Button::PageForward))) {
      doneSelection = (doneSelection + 1) % optionCount;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateDoneOption(doneSelection);
      return;
    }
    if (tap == LinguaTouch::Tap::Confirm) {
      for (int i = 0; i < optionCount; i++) {
        if (TranslationProgressUi::rectContains(optionRects[i], tapX, tapY)) {
          activateDoneOption(i);
          return;
        }
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || tap == LinguaTouch::Tap::Back) {
      returnToCaller();
    }
    return;
  }

  // Result screens: any of Confirm/Back leaves. The reader was torn down before this
  // activity launched, so we relaunch it from disk (it re-reads translation state).
  if (state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || tap != LinguaTouch::Tap::None) {
      returnToCaller();
      return;
    }
  }

  // Mid-translation cancel: worker checks cancelFlag between batches and between
  // chapters. Finished chapters are preserved on disk.
  if (state == TRANSLATING &&
      (mappedInput.wasReleased(MappedInputManager::Button::Back) || tap == LinguaTouch::Tap::Back)) {
    cancelFlag = true;
  }
}

void BookTranslationActivity::activateDoneOption(const int index) {
  if (bookFailed > 0 && index == 0) {
    // Retry: one more pass over the whole book in "missing only" mode.
    skipTranslated = true;
    launchWifiOrStart();
    return;
  }
  if (chaptersCompleted > 0) {
    displayModeChooser.begin(static_cast<int>(linguaSelectableIndex(SETTINGS.translationDisplayMode)));
    state = CHOOSE_DISPLAY_MODE;
    requestUpdate();
    return;
  }
  returnToCaller();
}

int BookTranslationActivity::drawInfoBlock(int y) {
  namespace ui = TranslationProgressUi;
  if (bookTitle.empty() && epub) bookTitle = epub->getTitle();
  y = ui::drawLine(renderer, y, bookTitle.c_str(), UI_12_FONT_ID, /*bold=*/true);
  if (!targetLangName.empty()) {
    std::string langLine = sourceLangName + " -> " + targetLangName + "  -  " + getEngineName();
    y = ui::drawLine(renderer, y, langLine.c_str(), UI_10_FONT_ID);
  }
  if (skipTranslated && state != CONFIRM_RETRANSLATE) {
    y = ui::drawLine(renderer, y, tr(STR_TRANSLATION_FILL_MODE), UI_10_FONT_ID);
  }
  return ui::drawSeparator(renderer, y + 4);
}

void BookTranslationActivity::render(RenderLock&&) {
  // Backstop for the loop()-level suppression: while the framebuffer is freed for a
  // chapter's network run there is nothing to draw on (drawing would be a
  // use-after-free). The panel retains its last flushed image until the next boundary.
  if (!renderer.hasFrameBuffer()) return;

  // The chooser owns a different, list-based layout via the UITheme components, so it draws
  // and flushes itself rather than sharing the raw-coordinate result-screen chrome below.
  if (state == CHOOSE_DISPLAY_MODE) {
    renderDisplayModeChooser();
    return;
  }

  namespace ui = TranslationProgressUi;
  renderer.clearScreen();
  int y = ui::drawHeader(renderer, tr(STR_TRANSLATE_BOOK));
  optionCount = 0;
  char line[96];

  if (state == CONFIRM_RETRANSLATE) {
    y = drawInfoBlock(y);
    snprintf(line, sizeof(line), "%d / %d %s", alreadyTranslatedCount, totalChapters,
             tr(STR_CHAPTERS_ALREADY_TRANSLATED));
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    y += 10;
    const char* labels[2] = {tr(STR_TRANSLATE_MISSING_ONLY), tr(STR_RETRANSLATE_ALL)};
    optionCount = 2;
    ui::drawButtons(renderer, y, labels, optionCount, confirmSelection, optionRects);
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  } else if (state == TRANSLATING) {
    y = drawInfoBlock(y);
    const int total = progressTotal;
    const int current = progressCurrent > total && total > 0 ? total : progressCurrent;
    const int chDone = chaptersCompleted;
    const int chIdx = currentChapter + 1;

    // Current chapter.
    snprintf(line, sizeof(line), tr(STR_CHAPTER_X_OF_Y), chIdx, totalChapters);
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID, /*bold=*/true);
    y = ui::drawProgress(renderer, y, current, total);
    if (total > 0) {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_PARAGRAPHS_FORMAT), current, total);
      y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    }
    // Whole book.
    y = ui::drawLine(renderer, y + 8, tr(STR_OVERALL_PROGRESS), UI_10_FONT_ID, /*bold=*/true);
    y = ui::drawProgress(renderer, y, chDone, totalChapters);
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_COUNTS_FORMAT), bookTranslated + liveTranslated,
             bookFailed + liveFailed);
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    const unsigned long elapsed = millis() - runStartMillis;
    // Book ETA from chapters done plus the fraction of the current one.
    const float doneFraction =
        static_cast<float>(chDone) + (total > 0 ? static_cast<float>(current) / static_cast<float>(total) : 0.0f);
    if (doneFraction > 0.05f && totalChapters > 0 && doneFraction < totalChapters) {
      const unsigned long remaining =
          static_cast<unsigned long>(static_cast<float>(elapsed) * (totalChapters - doneFraction) / doneFraction);
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_TIME_FORMAT), ui::formatDuration(elapsed).c_str(),
               ui::formatDuration(remaining).c_str());
    } else {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_ELAPSED_FORMAT), ui::formatDuration(elapsed).c_str());
    }
    ui::drawLine(renderer, y, line, UI_10_FONT_ID);

    const char* cancelHint = mappedInput.hasTouch() ? tr(STR_TAP_BOTTOM_TO_CANCEL) : tr(STR_BACK_TO_CANCEL);
    const auto hints = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
    const int h = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_10_FONT_ID, h - (h * LinguaTouch::BACK_ZONE_PERCENT) / 100 - 30, cancelHint);

  } else if (state == DONE) {
    y = drawInfoBlock(y);
    y = ui::drawLine(renderer, y, tr(STR_BOOK_TRANSLATION_DONE), UI_12_FONT_ID, /*bold=*/true);
    snprintf(line, sizeof(line), tr(STR_CHAPTER_X_OF_Y), (int)chaptersCompleted, totalChapters);
    y = ui::drawLine(renderer, y + 4, line, UI_10_FONT_ID);
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_COUNTS_FORMAT), (int)bookTranslated, (int)bookFailed);
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    if (bookAlreadyTranslated > 0) {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_ALREADY_FORMAT), (int)bookAlreadyTranslated);
      y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    }
    const unsigned long elapsed = (runEndMillis ? runEndMillis : millis()) - runStartMillis;
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_FINISHED_IN_FORMAT), ui::formatDuration(elapsed).c_str());
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    if (bookFailed > 0) y = ui::drawWrapped(renderer, y + 6, tr(STR_TRANSLATION_MISSING_HINT), UI_10_FONT_ID);
    y += 10;
    char retryLabel[48];
    snprintf(retryLabel, sizeof(retryLabel), tr(STR_RETRY_MISSING_FORMAT), (int)bookFailed);
    const char* labels[2] = {retryLabel, tr(STR_CONTINUE)};
    if (bookFailed > 0) {
      optionCount = 2;
      ui::drawButtons(renderer, y, labels, optionCount, doneSelection, optionRects);
    } else {
      optionCount = 1;
      ui::drawButtons(renderer, y, labels + 1, optionCount, 0, optionRects);
    }
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  } else if (state == FAILED || state == CANCELLED) {
    y = drawInfoBlock(y);
    y = ui::drawLine(renderer, y,
                     state == FAILED ? tr(STR_TRANSLATION_FAILED) : tr(STR_BOOK_TRANSLATION_CANCELLED),
                     UI_12_FONT_ID, /*bold=*/true);
    if (state == FAILED && lowMemoryAbort) {
      y = ui::drawWrapped(renderer, y + 6, tr(STR_TRANSLATION_LOW_MEMORY), UI_10_FONT_ID, 4);
    } else if (state == FAILED && statusMsg[0]) {
      y = ui::drawWrapped(renderer, y + 6, statusMsg, UI_10_FONT_ID);
    }
    snprintf(line, sizeof(line), tr(STR_CHAPTER_X_OF_Y), (int)chaptersCompleted, totalChapters);
    y = ui::drawLine(renderer, y + 4, line, UI_10_FONT_ID);
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_COUNTS_FORMAT), bookTranslated + liveTranslated,
             bookFailed + liveFailed);
    ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() * 2 / 3, tr(STR_PRESS_ANY_CONTINUE));
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  }

  renderer.displayBuffer();
}

void BookTranslationActivity::renderDisplayModeChooser() { displayModeChooser.render(renderer, mappedInput); }
