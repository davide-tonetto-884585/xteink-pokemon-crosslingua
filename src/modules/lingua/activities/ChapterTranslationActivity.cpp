#include "modules/lingua/activities/ChapterTranslationActivity.h"

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
#include "modules/lingua/ui/LinguaTouch.h"
#include "modules/lingua/ui/TranslationProgressUi.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "modules/lingua/LinguaModeCatalog.h"
#include "modules/lingua/activities/LanguagePickerActivity.h"

// Sentinel value LanguagePickerActivity returns for the synthetic "Auto-detect" entry.
// Matches CrossPointSettings::sourceTranslationLanguage's 0xFF sentinel.
static constexpr uint8_t AUTO_DETECT_SENTINEL = 0xFF;

// ─── epub (re)loading ───────────────────────────────────────────────────────

bool ChapterTranslationActivity::ensureEpubLoaded() {
  if (epub) return true;
  LOG_DBG("CHT", "Loading lean epub (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS, and don't rebuild the cache if it is missing.
  if (!epub->load(false, true)) {
    LOG_ERR("CHT", "Failed to load epub: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  LOG_DBG("CHT", "Lean epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  return true;
}

void ChapterTranslationActivity::returnToCaller() {
  if (returnTarget == TranslationReturnTarget::FILE_BROWSER) {
    // Started from the file browser (no reader was open) — return to the book's folder.
    activityManager.goToFileBrowser(FsHelpers::extractFolderPath(epubPath));
    return;
  }
  activityManager.goToReader(epubPath);
}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void ChapterTranslationActivity::onEnter() {
  Activity::onEnter();

  if (!ensureEpubLoaded()) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    requestUpdate();
    return;
  }

  resolveChapterInfo();

  // If the chapter is already translated, offer "missing only" (default) or "everything".
  if (alreadyTranslated) {
    state = CONFIRM_RETRANSLATE;
    optionSelection = 0;
    requestUpdate();
    return;
  }

  launchSourcePicker();
}

void ChapterTranslationActivity::resolveChapterInfo() {
  if (!epub) return;
  bookTitle = epub->getTitle();
  char buf[48];
  snprintf(buf, sizeof(buf), tr(STR_CHAPTER_X_OF_Y), spineIndex + 1, epub->getSpineItemsCount());
  chapterInfo = buf;
  const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (tocIndex >= 0) {
    const auto toc = epub->getTocItem(tocIndex);
    if (!toc.title.empty()) {
      chapterInfo += " - ";
      chapterInfo += toc.title;
    }
  }
}

void ChapterTranslationActivity::activateOption(const int index) {
  if (state == CONFIRM_RETRANSLATE) {
    fillMissingMode = index == 0;
    if (fillMissingMode) {
      // Same languages as the existing translation: reuse the stored choice when there is one.
      const uint8_t target = SETTINGS.translationLanguage;
      if (target < LanguagePickerActivity::NUM_LANGUAGES) {
        const uint8_t source = SETTINGS.sourceTranslationLanguage;
        if (source < LanguagePickerActivity::NUM_LANGUAGES) {
          sourceLangCode = LanguagePickerActivity::LANGUAGES[source].code;
          sourceLangName = LanguagePickerActivity::LANGUAGES[source].name;
        } else {
          sourceLangCode = "auto";
          sourceLangName = tr(STR_AUTO_DETECT);
        }
        targetLangCode = LanguagePickerActivity::LANGUAGES[target].code;
        targetLangName = LanguagePickerActivity::LANGUAGES[target].name;
        launchWifiOrStart();
        return;
      }
    }
    launchSourcePicker();
    return;
  }
  if (state == DONE) {
    if (missingCount() > 0 && index == 0) {
      // Retry: run again over the bilingual HTML just written, translating only what is missing.
      fillMissingMode = true;
      launchWifiOrStart();
      return;
    }
    if (lastResult.paragraphsTranslated > 0 || lastResult.alreadyTranslated > 0) {
      // Offer the display-mode chooser so a bilingual mode can be enabled straight away.
      displayModeChooser.begin(static_cast<int>(linguaSelectableIndex(SETTINGS.translationDisplayMode)));
      state = CHOOSE_DISPLAY_MODE;
      requestUpdate();
      return;
    }
    returnToCaller();
  }
}

void ChapterTranslationActivity::onExit() {
  Activity::onExit();

  // Signal the worker to bail at the next batch boundary and wait briefly for it.
  // Setting cancelFlag also breaks any in-progress repaint spin in serviceBatchBoundary,
  // so the worker never stays parked waiting for a boundaryAck that loop() will no longer
  // deliver (loop() does not run while we are here on the same main task). The 5-second
  // cap is enough for a partially-translated chapter to abort cleanly; longer waits would
  // block UI navigation.
  cancelFlag = true;
  if (taskHandle) {
    for (int i = 0; i < 50 && !taskDone && !taskFailed; i++) {
      delay(100);
    }
    taskHandle = nullptr;
  }

  // Drop the WiFi link to free heap before the next activity. Mirrors fork behavior.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Always hand the framebuffer back before leaving: returnToCaller() relaunches an
  // activity that renders immediately. onExit() runs while ActivityManager holds the
  // RenderLock, so restore without taking it again. Idempotent — a no-op if we never
  // released or already restored in the completion path. Done after the WiFi teardown
  // so the realloc has the most free heap available.
  restoreFramebuffer(/*alreadyLocked=*/true);
}

// ─── language pickers ─────────────────────────────────────────────────────────

void ChapterTranslationActivity::launchSourcePicker() {
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

void ChapterTranslationActivity::launchTargetPicker() {
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

void ChapterTranslationActivity::onSourceLangSelected(uint8_t resultIndex) {
  if (resultIndex == AUTO_DETECT_SENTINEL) {
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
    SETTINGS.sourceTranslationLanguage = AUTO_DETECT_SENTINEL;
  } else if (resultIndex < LanguagePickerActivity::NUM_LANGUAGES) {
    sourceLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
    sourceLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
    SETTINGS.sourceTranslationLanguage = resultIndex;
  } else {
    // Defensive: out-of-range index from picker. Fall back to auto-detect.
    sourceLangCode = "auto";
    sourceLangName = tr(STR_AUTO_DETECT);
  }
  SETTINGS.saveToFile();
  LOG_DBG("CHT", "Source language: %s (%s)", sourceLangName.c_str(), sourceLangCode.c_str());
  launchTargetPicker();
}

void ChapterTranslationActivity::onTargetLangSelected(uint8_t resultIndex) {
  if (resultIndex >= LanguagePickerActivity::NUM_LANGUAGES) {
    // Defensive: out-of-range. Cancel rather than translate to an unknown code.
    LOG_ERR("CHT", "Target language out of range: %d", resultIndex);
    returnToCaller();
    return;
  }
  targetLangCode = LanguagePickerActivity::LANGUAGES[resultIndex].code;
  targetLangName = LanguagePickerActivity::LANGUAGES[resultIndex].name;
  SETTINGS.translationLanguage = resultIndex;
  SETTINGS.saveToFile();
  LOG_DBG("CHT", "Target language: %s (%s)", targetLangName.c_str(), targetLangCode.c_str());

  launchWifiOrStart();
}

// ─── WiFi gate ────────────────────────────────────────────────────────────────

void ChapterTranslationActivity::launchWifiOrStart() {
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

void ChapterTranslationActivity::onWifiConnected(bool success) {
  if (!success) {
    state = FAILED;
    snprintf(statusMsg, sizeof(statusMsg), "WiFi connection failed");
    requestUpdate();
    return;
  }
  startTranslation();
}

// ─── translation task ─────────────────────────────────────────────────────────

void ChapterTranslationActivity::startTranslation() {
  state = TRANSLATING;
  cancelFlag = false;
  taskDone = false;
  taskFailed = false;
  lowMemoryAbort = false;
  lastResult = {};
  progressCurrent = 0;
  progressTotal = 0;
  liveTranslated = 0;
  liveFailed = 0;
  runStartMillis = millis();
  runEndMillis = 0;
  lastProgressUpdate = 0;
  boundaryPending = false;
  boundaryAck = false;
  lastRepaintProgress = 0;
  // Seed the 20 s repaint clock at run start so the first time-based repaint fires ~20 s
  // in, not immediately at the first boundary.
  lastRepaintMillis = millis();

  // Flush the "Translating..." status screen to the panel BEFORE freeing the
  // framebuffer. requestUpdateAndWait() blocks until the render task has drawn and
  // displayed it; E-ink then retains that image with no buffer for the whole run.
  // Safe here: startTranslation() runs on the main task from a result handler, which
  // does not hold the RenderLock.
  requestUpdateAndWait();

  LOG_DBG("CHT", "State -> TRANSLATING, lang=%s, engine=%d", targetLangCode.c_str(), SETTINGS.translationEngine);

  // Free the 48 KB heap framebuffer so the TLS handshake has the contiguous headroom
  // it needs. Released for the entire chapter run; restored in the loop() completion
  // path (and unconditionally in onExit()) before anything draws again.
  releaseFramebuffer();

  // 10 KB stack: ParagraphTranslator can spike to ~6-8 KB during HTTP + JSON parse on
  // the larger engines (Gemini, OpenAI). Priority 1 keeps it below the render task.
  xTaskCreate(translationTask, "chTranslate", 10240, this, 1, &taskHandle);
}

// ─── framebuffer lifecycle ─────────────────────────────────────────────────────

void ChapterTranslationActivity::releaseFramebuffer() {
  // RenderLock serialises against the render task: freeing the buffer while render()
  // is mid-draw would be a use-after-free. Both this and render() run on separate
  // tasks, so the lock is the guard even though render() also null-checks up front.
  RenderLock lock;
  if (!renderer.hasFrameBuffer()) return;  // already released
  if (!renderer.releaseFrameBufferForNetwork()) {
    LOG_ERR("CHT", "Framebuffer release failed");
    return;
  }
  LOG_DBG("MEM", "CT post-release: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void ChapterTranslationActivity::restoreFramebuffer(bool alreadyLocked) {
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
      LOG_DBG("MEM", "CT post-restore: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return;
    }
    LOG_ERR("CHT", "Framebuffer realloc failed (attempt %d/5)", attempt + 1);
    delay(100);
  }

  // Practically unreachable: restore runs only after the rewriter/HTTP/expat
  // transients (~35+ KB) have been freed, so a clean 48 KB hole is available. If it
  // still fails the device has no buffer to draw on; restart to recover. The
  // translated HTML is already committed to SD and the reader re-reads it on relaunch.
  LOG_ERR("CHT", "Framebuffer realloc permanently failed; restarting");
  ESP.restart();
}

bool ChapterTranslationActivity::tryRestoreFramebuffer() {
  if (renderer.hasFrameBuffer()) return true;  // already present
  RenderLock lock;
  if (renderer.hasFrameBuffer()) return true;  // re-check under the lock
  if (renderer.restoreFrameBufferAfterNetwork()) {
    LOG_DBG("MEM", "CT boundary restore: free=%u max=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return true;
  }
  // No contiguous 48 KB hole right now (the keep-alive TLS session is still open mid
  // chapter). Skip this cosmetic repaint; the next boundary retries. Never restart here —
  // a mid-chapter reboot would drop the in-flight chapter's work.
  LOG_DBG("CHT", "Boundary framebuffer restore unavailable; skipping repaint");
  return false;
}

void ChapterTranslationActivity::translationTask(void* param) {
  auto* self = static_cast<ChapterTranslationActivity*>(param);
  self->runTranslation();
  vTaskDelete(nullptr);
}

void ChapterTranslationActivity::runTranslation() {
  // ESP32 DHCP can hand back a DNS that doesn't resolve every Google subdomain
  // we hit (translate.google.com vs translate.googleapis.com). Force public DNS.
  IPAddress dns1(8, 8, 8, 8);
  IPAddress dns2(8, 8, 4, 4);
#ifndef SIMULATOR
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
#else
  (void)dns1;
  (void)dns2;
#endif
  delay(500);  // Let the lwIP stack pick up the new resolver list.
  LOG_DBG("CHT", "DNS set to 8.8.8.8 / 8.8.4.4");

  // Defensive: the lean Epub is normally loaded in onEnter, but guard the task entry too.
  if (!ensureEpubLoaded()) {
    snprintf(statusMsg, sizeof(statusMsg), "Failed to load book");
    taskFailed = true;
    return;
  }

  // Step 1: extract this chapter's HTML out of the EPUB zip into a scratch file. In fill-missing
  // mode the input is the existing bilingual HTML itself (read-only; output goes to ".part").
  const auto& spineItem = epub->getSpineItem(spineIndex);
  const auto tmpPath = fillMissingMode
                           ? translatedHtmlPath
                           : epub->getCachePath() + "/.tmp_translate_" + std::to_string(spineIndex) + ".html";

  HalFile tmpFile;
  if (fillMissingMode) {
    // Nothing to extract.
  } else if (!Storage.openFileForWrite("CHT", tmpPath, tmpFile)) {
    snprintf(statusMsg, sizeof(statusMsg), "Failed to create temp file");
    taskFailed = true;
    return;
  }
  if (!fillMissingMode) {
    if (!epub->readItemContentsToStream(spineItem.href, tmpFile, 1024)) {
      tmpFile.close();
      Storage.remove(tmpPath.c_str());
      snprintf(statusMsg, sizeof(statusMsg), "Failed to extract chapter");
      taskFailed = true;
      return;
    }
    tmpFile.close();  // Must close before re-opening the same path for read in step 2.
  }

  // Step 2: pre-scan for the progress-bar denominator. countBlocksInFile is a
  // pure SAX pass with no translation work and is cheap relative to step 3.
  progressTotal = TranslationHtmlRewriter::countBlocksInFile(tmpPath);
  LOG_DBG("CHT", "Translation started, total=%d blocks", (int)progressTotal);

  // Step 3: open the destination path and run the rewriter.
  // Section's createSectionFile() expects the `sections/` subdir to exist; the
  // path comes from Section::getTranslatedHtmlPath() so the dir may not have
  // been created yet if this chapter has never been rendered.
  const auto sectionsDir = epub->getCachePath() + "/sections";
  Storage.mkdir(sectionsDir.c_str());

  // Write to a ".part" file, then atomically rename it into place only on success. This
  // guarantees a power loss mid-translation never leaves a truncated file at the final path:
  // the final file appears solely via the post-completion rename, so hasTranslatedHtml()
  // (which trusts the final file's existence) can never observe a partial.
  const std::string partPath = translatedHtmlPath + ".part";
  Storage.remove(partPath.c_str());  // clear any stale partial from an interrupted run

  HalFile outFile;
  if (!Storage.openFileForWrite("CHT", partPath, outFile)) {
    if (!fillMissingMode) Storage.remove(tmpPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to create output file");
    taskFailed = true;
    return;
  }

  const char* srcLang = sourceLangCode.c_str();
  LOG_DBG("CHT", "Using source=%s, target=%s", srcLang, targetLangCode.c_str());

  TranslationHtmlRewriter rewriter;
  rewriter.setFillMissingMode(fillMissingMode);
  rewriter.setLiveCounters(&liveTranslated, &liveFailed);
  lastResult = rewriter.rewriteFromFile(tmpPath, outFile, srcLang, targetLangCode.c_str(), SETTINGS.translationEngine,
                                        SETTINGS.translateApiKey, &cancelFlag, &progressCurrent,
                                        &ChapterTranslationActivity::batchBoundaryTrampoline, this);
  outFile.close();  // Flush and release before the rename/delete dance below.

  if (!fillMissingMode) Storage.remove(tmpPath.c_str());

  if (cancelFlag || lastResult.cancelled) {
    // Partial output is unusable — section loader would render half-translated
    // content as if it were complete. Discard the ".part" and mark cancelled; any
    // prior committed translation at the final path is left untouched.
    Storage.remove(partPath.c_str());
    taskDone = true;
    return;
  }

  if (lastResult.abortedOnErrors) {
    Storage.remove(partPath.c_str());
    if (lastResult.abortedLowMemory) {
      // Specific low-memory abort: render() draws tr(STR_TRANSLATION_LOW_MEMORY)
      // directly (the translated text does not fit statusMsg's 64-byte buffer).
      lowMemoryAbort = true;
    } else if (lastResult.errorDetail[0]) {
      snprintf(statusMsg, sizeof(statusMsg), "%s", lastResult.errorDetail);
    } else {
      snprintf(statusMsg, sizeof(statusMsg), "Translation failed: too many errors");
    }
    taskFailed = true;
    return;
  }

  // Chapter-success semantics: zero translated paragraphs is only a FAILURE when the
  // chapter actually had translatable content that failed. A cover / image-only /
  // fully-already-translated chapter legitimately translates nothing — that passthrough
  // output is a valid result and is committed like any other.
  if (fillMissingMode && lastResult.paragraphsTranslated == 0) {
    // Nothing new: keep the existing bilingual file untouched and report on the summary screen
    // (which offers another retry when paragraphs are still missing).
    Storage.remove(partPath.c_str());
    taskDone = true;
    return;
  }
  if (lastResult.paragraphsTranslated == 0 && (lastResult.translateFailures > 0 || lastResult.abortedOnErrors)) {
    Storage.remove(partPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "No paragraphs translated");
    taskFailed = true;
    return;
  }

  // Commit atomically: remove any prior final output, then promote ".part" -> final. The
  // rename is the commit point — a completed translation is simply "the final file exists";
  // a crash before the rename leaves only the ".part", which is ignored on the next run.
  Storage.remove(translatedHtmlPath.c_str());
  if (!Storage.rename(partPath.c_str(), translatedHtmlPath.c_str())) {
    Storage.remove(partPath.c_str());
    snprintf(statusMsg, sizeof(statusMsg), "Failed to finalize output");
    taskFailed = true;
    return;
  }
  // The pages laid out from the previous translation are stale (e.g. after "missing only").
  Section::invalidatePageCaches(translatedHtmlPath);

  LOG_DBG("CHT", "Translation done: %d translated, %d skipped, %d failed, %d already translated%s",
          lastResult.paragraphsTranslated, lastResult.paragraphsSkipped, lastResult.translateFailures,
          lastResult.alreadyTranslated, fillMissingMode ? " (missing only)" : "");
  taskDone = true;
}

// ─── periodic progress repaint (worker-side cadence + UI handshake) ────────────

void ChapterTranslationActivity::batchBoundaryTrampoline(void* ctx) {
  static_cast<ChapterTranslationActivity*>(ctx)->serviceBatchBoundary();
}

void ChapterTranslationActivity::serviceBatchBoundary() {
  // Runs on the worker (chTranslate) task between batches: transients are freed and
  // progressCurrent is up to date. Bail immediately if we're cancelling so onExit()
  // (which sets cancelFlag then waits for the task to finish) never blocks on a spin.
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
  // framebuffer/render state here, so the restore/redraw/release can never race the
  // render task or onExit()'s teardown. The spin also exits on cancelFlag.
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

const char* ChapterTranslationActivity::getEngineName() const {
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

void ChapterTranslationActivity::loop() {
  // Touch fallback (X4 Pro): bottom strip = Back, elsewhere = Confirm. Read once per pass.
  int tapX = 0;
  int tapY = 0;
  const LinguaTouch::Tap tap = (state == CHOOSE_DISPLAY_MODE)
                                   ? LinguaTouch::Tap::None
                                   : LinguaTouch::readTap(mappedInput, renderer, &tapX, &tapY);
  // Option screens (CONFIRM_RETRANSLATE, DONE summary): Up/Down/Left/Right move the selection,
  // Confirm or a tap on an option activates it, Back / the bottom strip leaves.
  if (state == CONFIRM_RETRANSLATE || state == DONE) {
    if (optionCount > 1 && (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                            mappedInput.wasReleased(MappedInputManager::Button::PageForward))) {
      optionSelection = (optionSelection + 1) % optionCount;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateOption(optionSelection);
      return;
    }
    if (tap == LinguaTouch::Tap::Confirm) {
      for (int i = 0; i < optionCount; i++) {
        if (TranslationProgressUi::rectContains(optionRects[i], tapX, tapY)) {
          activateOption(i);
          return;
        }
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || tap == LinguaTouch::Tap::Back) {
      returnToCaller();
      return;
    }
    return;
  }

  if (state == TRANSLATING) {
    // The worker is paused at a batch boundary asking for a progress repaint: restore
    // the buffer, draw the updated progress synchronously, free it again, then release
    // the worker. All on the main task, so it never races the render task; and since
    // onExit() runs on this same task it can never overlap this sequence. Claim
    // boundaryPending up front so a second loop() pass cannot re-run the cycle.
    if (boundaryPending) {
      boundaryPending = false;
      // A mid-chapter batch boundary can momentarily lack a contiguous 48 KB hole (the
      // keep-alive TLS session is still open). Waiting here is pointless: the worker is
      // parked spinning on boundaryAck with the TLS buffers held, so nothing can free
      // memory while we stall — a wait only freezes the UI with cancel unresponsive. Use
      // the NON-restarting restore directly: if the buffer can't be reclaimed, skip this
      // cosmetic repaint (the next boundary retries) rather than rebooting mid-chapter.
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
      if (cancelFlag || lastResult.cancelled) {
        state = CANCELLED;
      } else {
        // Summary first (counts, time, and a retry when paragraphs are missing); Continue then
        // offers the display-mode chooser when something was translated.
        state = DONE;
        optionSelection = 0;
      }
      requestUpdate();
    } else if (taskFailed) {
      restoreFramebuffer();
      runEndMillis = millis();
      state = FAILED;
      requestUpdate();
    } else {
      // The framebuffer is released for the whole run, so hasFrameBuffer() is false
      // and no progress repaints are issued — the network run stays silent by design
      // (a request here would be a no-op render anyway). Gate on the buffer so we
      // resume normal throttled repaints if release ever failed.
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
          LOG_DBG("CHT", "Display mode set to %d after translation", (int)chosen);
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

  // Result screens: any of Confirm/Back leaves. The reader was torn down before this
  // activity launched, so we relaunch it from disk (it re-reads translation state).
  if (state == FAILED || state == CANCELLED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || tap != LinguaTouch::Tap::None) {
      returnToCaller();
      return;
    }
  }

  // Mid-translation cancel: the worker checks cancelFlag between batches.
  if (state == TRANSLATING &&
      (mappedInput.wasReleased(MappedInputManager::Button::Back) || tap == LinguaTouch::Tap::Back)) {
    cancelFlag = true;
  }
}

int ChapterTranslationActivity::drawInfoBlock(int y) {
  namespace ui = TranslationProgressUi;
  y = ui::drawLine(renderer, y, bookTitle.c_str(), UI_12_FONT_ID, /*bold=*/true);
  y = ui::drawLine(renderer, y, chapterInfo.c_str(), UI_10_FONT_ID);
  if (!targetLangName.empty()) {
    // "Source -> Target" uses ASCII to avoid font-coverage gaps; language names are English.
    std::string langLine = sourceLangName + " -> " + targetLangName + "  -  " + getEngineName();
    y = ui::drawLine(renderer, y, langLine.c_str(), UI_10_FONT_ID);
  }
  if (fillMissingMode) y = ui::drawLine(renderer, y, tr(STR_TRANSLATION_FILL_MODE), UI_10_FONT_ID);
  return ui::drawSeparator(renderer, y + 4);
}

void ChapterTranslationActivity::render(RenderLock&&) {
  namespace ui = TranslationProgressUi;
  // Backstop for the loop()-level suppression: while the framebuffer is freed for the
  // network run there is nothing to draw on (drawing would be a use-after-free). The
  // panel retains the last flushed "Translating..." image until restore.
  if (!renderer.hasFrameBuffer()) return;

  // The chooser owns a different, list-based layout via the UITheme components, so it draws
  // and flushes itself rather than sharing the result-screen chrome below.
  if (state == CHOOSE_DISPLAY_MODE) {
    renderDisplayModeChooser();
    return;
  }

  renderer.clearScreen();
  int y = ui::drawHeader(renderer, tr(STR_TRANSLATE_CHAPTER));
  optionCount = 0;
  char line[96];

  if (state == CONFIRM_RETRANSLATE) {
    y = drawInfoBlock(y);
    y = ui::drawLine(renderer, y, tr(STR_CHAPTER_ALREADY_TRANSLATED), UI_12_FONT_ID, /*bold=*/true);
    y += 10;
    const char* labels[2] = {tr(STR_TRANSLATE_MISSING_ONLY), tr(STR_RETRANSLATE_ALL)};
    optionCount = 2;
    ui::drawButtons(renderer, y, labels, optionCount, optionSelection, optionRects);
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  } else if (state == TRANSLATING) {
    y = drawInfoBlock(y);
    // Snapshot the worker-written counters so one frame is self-consistent.
    const int total = progressTotal;
    const int current = progressCurrent > total && total > 0 ? total : progressCurrent;
    const int translated = liveTranslated;
    const int failed = liveFailed;
    y = ui::drawLine(renderer, y, tr(STR_TRANSLATING_CHAPTER), UI_10_FONT_ID);
    y = ui::drawProgress(renderer, y + 6, current, total);
    if (total > 0) {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_PARAGRAPHS_FORMAT), current, total);
      y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    }
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_COUNTS_FORMAT), translated, failed);
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    const unsigned long elapsed = millis() - runStartMillis;
    if (current > 0 && total > current) {
      const unsigned long remaining =
          static_cast<unsigned long>((static_cast<uint64_t>(elapsed) * (total - current)) / current);
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
    y = ui::drawLine(renderer, y, tr(STR_TRANSLATION_DONE), UI_12_FONT_ID, /*bold=*/true);
    y += 4;
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_COUNTS_FORMAT), lastResult.paragraphsTranslated,
             lastResult.translateFailures);
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    if (lastResult.alreadyTranslated > 0) {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_ALREADY_FORMAT), lastResult.alreadyTranslated);
      y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    }
    // paragraphsSkipped also counts the failures; the rest stayed original on purpose (the
    // engine returned them unchanged, e.g. names or numbers).
    const int unchanged = lastResult.paragraphsSkipped - lastResult.translateFailures;
    if (unchanged > 0) {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_UNCHANGED_FORMAT), unchanged);
      y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    }
    const unsigned long elapsed = (runEndMillis ? runEndMillis : millis()) - runStartMillis;
    snprintf(line, sizeof(line), tr(STR_TRANSLATION_FINISHED_IN_FORMAT), ui::formatDuration(elapsed).c_str());
    y = ui::drawLine(renderer, y, line, UI_10_FONT_ID);
    if (missingCount() > 0) y = ui::drawWrapped(renderer, y + 6, tr(STR_TRANSLATION_MISSING_HINT), UI_10_FONT_ID);
    y += 10;

    char retryLabel[48];
    snprintf(retryLabel, sizeof(retryLabel), tr(STR_RETRY_MISSING_FORMAT), missingCount());
    const char* labels[2] = {retryLabel, tr(STR_CONTINUE)};
    if (missingCount() > 0) {
      optionCount = 2;
      ui::drawButtons(renderer, y, labels, optionCount, optionSelection, optionRects);
    } else {
      optionCount = 1;
      ui::drawButtons(renderer, y, labels + 1, optionCount, 0, optionRects);
    }
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  } else if (state == FAILED) {
    y = drawInfoBlock(y);
    y = ui::drawLine(renderer, y, tr(STR_TRANSLATION_FAILED), UI_12_FONT_ID, /*bold=*/true);
    if (lowMemoryAbort) {
      // Long translated message -> wrapped (statusMsg's 64-byte buffer can't hold it).
      y = ui::drawWrapped(renderer, y + 6, tr(STR_TRANSLATION_LOW_MEMORY), UI_10_FONT_ID, 4);
    } else if (statusMsg[0]) {
      y = ui::drawWrapped(renderer, y + 6, statusMsg, UI_10_FONT_ID);
    }
    if (runStartMillis) {
      snprintf(line, sizeof(line), tr(STR_TRANSLATION_COUNTS_FORMAT), (int)liveTranslated, (int)liveFailed);
      ui::drawLine(renderer, y + 6, line, UI_10_FONT_ID);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() * 2 / 3, tr(STR_PRESS_ANY_CONTINUE));
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  } else if (state == CANCELLED) {
    y = drawInfoBlock(y);
    ui::drawLine(renderer, y, tr(STR_TRANSLATION_CANCELLED), UI_12_FONT_ID, /*bold=*/true);
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() * 2 / 3, tr(STR_PRESS_ANY_CONTINUE));
    const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  }

  renderer.displayBuffer();
}

void ChapterTranslationActivity::renderDisplayModeChooser() { displayModeChooser.render(renderer, mappedInput); }
