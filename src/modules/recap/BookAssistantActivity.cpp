#include "modules/recap/BookAssistantActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "modules/recap/GeminiRecap.h"
#include "modules/recap/HtmlTextExtractor.h"
#include "network/HttpDownloader.h"

namespace {
constexpr int BODY_FONT_ID = UI_12_FONT_ID;
constexpr int SIDE_MARGIN = 20;
// Generous cap for one paragraph's wrapped lines; an answer paragraph is a few lines.
constexpr int MAX_LINES_PER_PARAGRAPH = 200;
constexpr size_t MAX_QUESTION_CHARS = 300;

// Context limits. A chapter longer than this is summarized from its first part only; the current
// chapter keeps its last part (the most recent text matters most for a question).
constexpr size_t MAX_CHAPTER_TEXT_BYTES = 200 * 1024;
constexpr size_t MAX_CURRENT_TEXT_BYTES = 120 * 1024;
// Spine items with less text than this (cover, title page, dedication) get an empty summary.
constexpr size_t MIN_STORY_TEXT_BYTES = 400;
// Free-tier requests per minute are limited; space chapter-summary requests out.
constexpr uint32_t MIN_SUMMARY_REQUEST_SPACING_MS = 4000;
// After every model answered 429, wait this long and try the list again (this many times).
constexpr uint32_t QUOTA_WAIT_MS = 30000;
constexpr int SUMMARY_QUOTA_ROUNDS = 3;
constexpr int ANSWER_QUOTA_ROUNDS = 1;
// The progress screen is repainted between chapters at most this often (each repaint costs a
// framebuffer restore/release cycle and an e-ink refresh).
constexpr uint32_t REPAINT_INTERVAL_MS = 12000;
constexpr size_t FILE_CHUNK = 2048;

const char* currentLanguageName() {
  const auto lang = static_cast<size_t>(I18N.getLanguage());
  return lang < static_cast<size_t>(Language::_COUNT) ? LANGUAGE_NAMES[lang] : "English";
}

std::string describeHttpFailure(const int code, const std::string& body) {
  const std::string apiMessage = recap::parseGeminiError(body);
  char buf[112];
  if (code == 400 && apiMessage.find("API key") != std::string::npos) return tr(STR_RECAP_INVALID_KEY);
  if (code == 401 || code == 403) return tr(STR_RECAP_INVALID_KEY);
  if (code == 429) return tr(STR_RECAP_QUOTA);
  if (code <= 0) {
    // The transport error name (DNS/connect, TLS, timeout...) is the only clue on the device.
    if (HttpDownloader::lastErrorName) {
      snprintf(buf, sizeof(buf), "%s (%s)", tr(STR_RECAP_CONNECTION_FAILED), HttpDownloader::lastErrorName);
    } else {
      snprintf(buf, sizeof(buf), "%s (%d)", tr(STR_RECAP_CONNECTION_FAILED), code);
    }
    return buf;
  }
  snprintf(buf, sizeof(buf), "HTTP %d", code);
  std::string out = buf;
  if (!apiMessage.empty()) {
    out += ": ";
    out += apiMessage.substr(0, 160);
  }
  return out;
}

std::string trimmed(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// Print adapter feeding EPUB item bytes into the HTML text extractor.
class ExtractorPrint final : public Print {
 public:
  explicit ExtractorPrint(recap::HtmlTextExtractor& extractor) : extractor_(extractor) {}
  size_t write(uint8_t c) override {
    const char ch = static_cast<char>(c);
    extractor_.feed(&ch, 1);
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    extractor_.feed(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

 private:
  recap::HtmlTextExtractor& extractor_;
};

// Writes JSON-escaped text to a file through a reused buffer.
class EscapedWriter {
 public:
  explicit EscapedWriter(HalFile& file) : file_(file) {}
  bool raw(const std::string& s) { return write(s.data(), s.size(), false); }
  bool text(const std::string& s) { return write(s.data(), s.size(), true); }
  bool text(const char* s, size_t len) { return write(s, len, true); }
  bool ok() const { return ok_; }

 private:
  bool write(const char* s, size_t len, bool escape) {
    if (!ok_ || len == 0) return ok_;
    buffer_.clear();
    if (escape) {
      recap::appendJsonEscaped(buffer_, s, len);
    } else {
      buffer_.assign(s, len);
    }
    ok_ = file_.write(reinterpret_cast<const uint8_t*>(buffer_.data()), buffer_.size()) == buffer_.size();
    return ok_;
  }
  HalFile& file_;
  std::string buffer_;
  bool ok_ = true;
};
}  // namespace

// ─── worker job ──────────────────────────────────────────────────────────────

// Shared with the worker task, so a worker still blocked in an HTTP call can never outlive what it
// writes to: whoever drops the last reference frees it.
struct BookAssistantActivity::Job {
  enum class Phase : uint8_t { Preparing, Requesting };

  Mode mode = Mode::Recap;
  std::string apiKey;
  std::string languageName;
  std::string bookTitle;
  std::string chapterTitle;
  std::string subject;
  int spineIndex = 0;
  bool startsMidChapter = false;
  std::string excerptText;      // Recap
  std::shared_ptr<Epub> epub;   // context modes
  std::string dir;              // <book cache>/assistant

  volatile Phase phase = Phase::Requesting;
  volatile int progressDone = 0;
  volatile int progressTotal = 0;
  volatile bool cancel = false;
  volatile bool repaintRequested = false;

  // Outputs, written by the worker before `finished` is published.
  std::string answer;
  std::string error;
  std::string model;
  bool cancelled = false;
  volatile bool finished = false;

  std::string summaryPath(int spine) const { return dir + "/ch" + std::to_string(spine) + ".sum"; }
  std::string chapterTextPath() const { return dir + "/chapter.txt"; }
  std::string bodyPath() const { return dir + "/body.json"; }
  std::string currentTextPath() const { return dir + "/current.txt"; }

  // Sleeps in small steps; false if the job was cancelled meanwhile.
  bool wait(uint32_t ms) {
    const uint32_t start = millis();
    while (millis() - start < ms) {
      if (cancel) return false;
      delay(100);
    }
    return !cancel;
  }

  // Asks the main task to repaint the progress screen and waits (briefly) until it has.
  void requestRepaint() {
    repaintRequested = true;
    const uint32_t start = millis();
    while (repaintRequested && !cancel && millis() - start < 8000) delay(50);
    repaintRequested = false;
  }

  std::string tocTitle(int spine) const {
    if (!epub) return {};
    const int tocIndex = epub->getTocIndexForSpineIndex(spine);
    return tocIndex >= 0 ? epub->getTocItem(tocIndex).title : std::string();
  }
};

namespace {
using Job = BookAssistantActivity::Job;

// Request body: built in memory (Recap) or into a file on the SD card (context modes). `build` is
// called again with lowThinking=false if a model rejects the thinking setting.
struct BodySource {
  std::string memory;
  std::string filePath;
  std::function<bool(BodySource&, bool lowThinking)> build;
};

enum class CallOutcome : uint8_t { Ok, Failed, Cancelled };

CallOutcome callGemini(Job& job, const char* const* models, const size_t modelCount, BodySource& body,
                       const int quotaRounds, std::string& outText) {
  std::string response;
  ReusableHttpSession heapGate;
  for (int round = 0; round <= quotaRounds; round++) {
    bool allRateLimited = true;
    for (size_t modelIndex = 0; modelIndex < modelCount; modelIndex++) {
      const char* model = models[modelIndex];
      bool lowThinking = true;
      int transportRetries = 0;
      bool nextModel = false;
      for (int attempt = 0; attempt < 4 && !nextModel; attempt++) {
        if (job.cancel) return CallOutcome::Cancelled;
        if (!body.build(body, lowThinking)) {
          job.error = tr(STR_ASSISTANT_NO_CONTEXT);
          return CallOutcome::Failed;
        }
        // The TLS handshake needs a large contiguous block; wait briefly for the heap to settle.
        heapGate.waitForHeapReady(5000, &job.cancel);
        LOG_DBG("RECAP", "POST %s (heap %u/%u)", model, static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        response.clear();
        const std::string url = recap::geminiEndpoint(model);
        const bool ok = body.filePath.empty()
                            ? HttpDownloader::post(url, body.memory, "application/json", recap::GEMINI_API_KEY_HEADER,
                                                   job.apiKey.c_str(), response)
                            : HttpDownloader::postFile(url, body.filePath, "application/json",
                                                       recap::GEMINI_API_KEY_HEADER, job.apiKey.c_str(), response);
        const int code = HttpDownloader::lastHttpCode;
        if (body.filePath.empty()) {
          body.memory.clear();
          body.memory.shrink_to_fit();
        }

        if (ok) {
          const auto status = recap::parseGeminiResponse(response, outText);
          if (status == recap::RecapParseStatus::Ok) {
            job.model = model;
            return CallOutcome::Ok;
          }
          job.error = status == recap::RecapParseStatus::Blocked ? tr(STR_RECAP_BLOCKED) : tr(STR_RECAP_EMPTY);
          LOG_ERR("RECAP", "%s: unusable response (%.120s)", model, response.c_str());
          return CallOutcome::Failed;
        }

        LOG_ERR("RECAP", "%s failed: HTTP %d %s (%.160s)", model, code,
                HttpDownloader::lastErrorName ? HttpDownloader::lastErrorName : "", response.c_str());
        // Connection-level failure (DNS, TLS, timeout): try the same model again after a pause.
        if (code <= 0 && transportRetries < 2) {
          transportRetries++;
          if (!job.wait(1500)) return CallOutcome::Cancelled;
          continue;
        }
        // A model that does not accept the thinking setting answers 400 on an otherwise valid
        // request: retry it once without the setting before giving up on it.
        const bool keyRejected = recap::parseGeminiError(response).find("API key") != std::string::npos;
        if (code == 400 && lowThinking && !keyRejected) {
          lowThinking = false;
          continue;
        }
        job.error = describeHttpFailure(code, response);
        if (code != 429) allRateLimited = false;
        if (!recap::shouldTryNextModel(code)) return CallOutcome::Failed;
        nextModel = true;
      }
    }
    // Every model is over its per-minute quota: wait for the window to roll over, then retry.
    if (!allRateLimited || round == quotaRounds) break;
    LOG_INF("RECAP", "All models rate limited; waiting %u ms", static_cast<unsigned>(QUOTA_WAIT_MS));
    if (!job.wait(QUOTA_WAIT_MS)) return CallOutcome::Cancelled;
  }
  return CallOutcome::Failed;
}

// Extracts one chapter's text from the EPUB to chapter.txt. Returns the bytes written.
size_t extractChapterText(Job& job, const int spine) {
  HalFile out;
  if (!Storage.openFileForWrite("RECAP", job.chapterTextPath(), out)) return 0;
  size_t written = 0;
  recap::HtmlTextExtractor extractor([&out, &written](const char* data, size_t len) {
    if (written >= MAX_CHAPTER_TEXT_BYTES) return;
    len = std::min(len, MAX_CHAPTER_TEXT_BYTES - written);
    out.write(reinterpret_cast<const uint8_t*>(data), len);
    written += len;
  });
  ExtractorPrint adapter(extractor);
  const auto spineItem = job.epub->getSpineItem(spine);
  if (!job.epub->readItemContentsToStream(spineItem.href, adapter, 1024)) {
    LOG_ERR("RECAP", "Could not read spine %d (%s)", spine, spineItem.href.c_str());
  }
  extractor.finish();
  out.close();
  return written;
}

// Streams `path` (from byte `start`) into the writer JSON-escaped. When starting mid-file, the
// partial word at the cut is dropped.
bool appendFileText(EscapedWriter& writer, const std::string& path, const size_t start) {
  HalFile in;
  if (!Storage.openFileForRead("RECAP", path, in)) return false;
  if (start > 0) in.seek(start);
  char buf[FILE_CHUNK];
  bool skipPartialWord = start > 0;
  while (writer.ok()) {
    const int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    size_t offset = 0;
    if (skipPartialWord) {
      while (offset < static_cast<size_t>(n) && buf[offset] != ' ' && buf[offset] != '\n') offset++;
      if (offset < static_cast<size_t>(n)) skipPartialWord = false;
    }
    if (offset < static_cast<size_t>(n)) writer.text(buf + offset, n - offset);
  }
  in.close();
  return writer.ok();
}

bool buildSummaryBody(Job& job, const std::string& chapterTitle, const bool lowThinking) {
  HalFile file;
  if (!Storage.openFileForWrite("RECAP", job.bodyPath(), file)) return false;
  EscapedWriter writer(file);
  writer.raw(recap::streamingBodyPrefix(recap::chapterSummaryInstructions(job.languageName.c_str(),
                                                                          job.bookTitle.c_str(),
                                                                          chapterTitle.c_str()),
                                        lowThinking));
  appendFileText(writer, job.chapterTextPath(), 0);
  writer.raw(recap::streamingBodySuffix());
  file.close();
  return writer.ok();
}

// Makes sure every chapter before the current one has a summary on the SD card.
bool prepareSummaries(Job& job) {
  std::vector<int> missing;
  for (int spine = 0; spine < job.spineIndex; spine++) {
    if (!Storage.exists(job.summaryPath(spine).c_str())) missing.push_back(spine);
  }
  if (missing.empty()) return true;

  job.phase = Job::Phase::Preparing;
  job.progressTotal = static_cast<int>(missing.size());
  job.progressDone = 0;
  job.requestRepaint();
  uint32_t lastRepaint = millis();
  uint32_t lastRequest = 0;

  for (const int spine : missing) {
    if (job.cancel) {
      job.cancelled = true;
      return false;
    }
    if (millis() - lastRepaint >= REPAINT_INTERVAL_MS) {
      job.requestRepaint();
      lastRepaint = millis();
    }

    std::string summary;
    const size_t textBytes = extractChapterText(job, spine);
    if (textBytes >= MIN_STORY_TEXT_BYTES) {
      if (lastRequest != 0 && millis() - lastRequest < MIN_SUMMARY_REQUEST_SPACING_MS &&
          !job.wait(MIN_SUMMARY_REQUEST_SPACING_MS - (millis() - lastRequest))) {
        job.cancelled = true;
        return false;
      }
      const std::string chapterTitle = job.tocTitle(spine);
      BodySource body;
      body.filePath = job.bodyPath();
      body.build = [&job, &chapterTitle](BodySource&, bool lowThinking) {
        return buildSummaryBody(job, chapterTitle, lowThinking);
      };
      const auto outcome = callGemini(job, recap::GEMINI_SUMMARY_MODELS, recap::GEMINI_SUMMARY_MODEL_COUNT, body,
                                      SUMMARY_QUOTA_ROUNDS, summary);
      lastRequest = millis();
      if (outcome == CallOutcome::Cancelled) {
        job.cancelled = true;
        return false;
      }
      if (outcome != CallOutcome::Ok) return false;
      summary = trimmed(summary);
      if (summary.rfind("NONE", 0) == 0 && summary.size() <= 6) summary.clear();
      LOG_INF("RECAP", "Summary for spine %d: %u bytes from %u", spine, static_cast<unsigned>(summary.size()),
              static_cast<unsigned>(textBytes));
    }
    // An empty file marks a chapter with no story text, so it is not requested again.
    HalFile out;
    if (!Storage.openFileForWrite("RECAP", job.summaryPath(spine), out)) {
      job.error = tr(STR_ASSISTANT_NO_CONTEXT);
      return false;
    }
    if (!summary.empty()) out.write(reinterpret_cast<const uint8_t*>(summary.data()), summary.size());
    out.close();
    job.progressDone = job.progressDone + 1;
  }
  Storage.remove(job.chapterTextPath().c_str());
  return true;
}

bool buildContextBody(Job& job, const bool lowThinking) {
  recap::AssistantPrompt prompt;
  prompt.task = job.mode == BookAssistantActivity::Mode::Characters ? recap::AssistantTask::Characters
                : job.mode == BookAssistantActivity::Mode::WhoIs    ? recap::AssistantTask::WhoIs
                                                                     : recap::AssistantTask::Question;
  prompt.languageName = job.languageName.c_str();
  prompt.bookTitle = job.bookTitle.c_str();
  prompt.subject = job.subject.c_str();

  HalFile file;
  if (!Storage.openFileForWrite("RECAP", job.bodyPath(), file)) return false;
  EscapedWriter writer(file);
  writer.raw(recap::streamingBodyPrefix(recap::assistantInstructions(prompt), lowThinking));

  writer.text(std::string(job.spineIndex > 0 ? recap::CONTEXT_SUMMARIES_HEADER : recap::CONTEXT_NO_SUMMARIES));
  for (int spine = 0; spine < job.spineIndex && writer.ok(); spine++) {
    HalFile in;
    if (!Storage.openFileForRead("RECAP", job.summaryPath(spine), in)) continue;
    const size_t size = in.size();
    std::string summary(size, '\0');
    const int n = size > 0 ? in.read(summary.data(), size) : 0;
    in.close();
    if (n <= 0) continue;
    summary.resize(static_cast<size_t>(n));
    writer.text(recap::contextSummaryHeading(spine + 1, job.tocTitle(spine).c_str()));
    writer.text(summary);
    writer.text(std::string("\n\n"));
  }

  size_t currentSize = 0;
  {
    HalFile in;
    if (Storage.openFileForRead("RECAP", job.currentTextPath(), in)) {
      currentSize = in.size();
      in.close();
    }
  }
  const size_t start = currentSize > MAX_CURRENT_TEXT_BYTES ? currentSize - MAX_CURRENT_TEXT_BYTES : 0;
  writer.text(recap::contextCurrentChapterHeading(job.chapterTitle.c_str(), job.startsMidChapter || start > 0));
  if (currentSize > 0) appendFileText(writer, job.currentTextPath(), start);
  writer.text(std::string("\n\n") + recap::assistantRequestLine(prompt));
  writer.raw(recap::streamingBodySuffix());
  file.close();
  return writer.ok();
}

void runJob(Job& job) {
  if (job.apiKey.empty()) {
    job.error = tr(STR_RECAP_INVALID_KEY);
    return;
  }

#ifndef SIMULATOR
  // Same as the Lingua translation tasks: resolve googleapis.com through public DNS (some routers'
  // resolvers fail on it) and give the new settings a moment before the first connect.
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), IPAddress(8, 8, 8, 8), IPAddress(8, 8, 4, 4));
  delay(500);
#endif

  if (job.mode == BookAssistantActivity::Mode::Recap) {
    BodySource body;
    body.build = [&job](BodySource& source, bool lowThinking) {
      recap::RecapRequest request{job.excerptText};
      request.languageName = job.languageName.c_str();
      request.bookTitle = job.bookTitle.c_str();
      request.chapterTitle = job.chapterTitle.c_str();
      request.startsMidChapter = job.startsMidChapter;
      request.lowThinking = lowThinking;
      source.memory = recap::buildGeminiRequestBody(request);
      return true;
    };
    if (callGemini(job, recap::GEMINI_MODELS, recap::GEMINI_MODEL_COUNT, body, ANSWER_QUOTA_ROUNDS, job.answer) ==
        CallOutcome::Cancelled) {
      job.cancelled = true;
    }
    return;
  }

  Storage.mkdir(job.dir.c_str());
  if (!prepareSummaries(job)) return;

  if (job.phase == Job::Phase::Preparing) {
    job.phase = Job::Phase::Requesting;
    job.requestRepaint();
  }
  BodySource body;
  body.filePath = job.bodyPath();
  body.build = [&job](BodySource&, bool lowThinking) { return buildContextBody(job, lowThinking); };
  const auto outcome =
      callGemini(job, recap::GEMINI_MODELS, recap::GEMINI_MODEL_COUNT, body, ANSWER_QUOTA_ROUNDS, job.answer);
  if (outcome == CallOutcome::Cancelled) job.cancelled = true;
  Storage.remove(job.bodyPath().c_str());
}
}  // namespace

// ─── lifecycle ───────────────────────────────────────────────────────────────

bool BookAssistantActivity::ensureEpubLoaded() {
  if (epub) return true;
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS, and don't rebuild the cache if it is missing.
  if (!epub->load(false, true)) {
    LOG_ERR("RECAP", "Failed to load epub: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  return true;
}

void BookAssistantActivity::onEnter() {
  Activity::onEnter();
  if (request.mode == Mode::Recap && request.excerptText.empty()) {
    state = State::FAILED;
    errorMessage = tr(STR_RECAP_NO_TEXT);
    requestUpdate();
    return;
  }
  if (usesBookContext() &&
      (!ensureEpubLoaded() || !Storage.exists(currentChapterTextPath(epub->getCachePath()).c_str()))) {
    state = State::FAILED;
    errorMessage = tr(STR_ASSISTANT_NO_CONTEXT);
    requestUpdate();
    return;
  }
  launchWifiOrStart();
}

void BookAssistantActivity::onExit() {
  Activity::onExit();
  // A worker still blocked in its HTTP call keeps its own reference to the job (and the Epub), so
  // it is simply told to stop and abandoned; it frees the job when the call returns.
  if (job) job->cancel = true;
  job.reset();
  epub.reset();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  // onExit() runs under the ActivityManager's RenderLock; the next activity renders immediately.
  restoreFramebuffer(/*alreadyLocked=*/true);
}

void BookAssistantActivity::returnToReader() { activityManager.goToReader(epubPath); }

// ─── network ─────────────────────────────────────────────────────────────────

void BookAssistantActivity::launchWifiOrStart() {
  state = State::WIFI;
  requestUpdate();

  WiFi.mode(WIFI_STA);
#if defined(SIMULATOR)
  // Same as the Lingua activities: the simulator's fake scan confuses the Wi-Fi picker.
  if (WiFi.status() != WL_CONNECTED) WiFi.begin();
#endif
  if (WiFi.status() == WL_CONNECTED) {
    startJob();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = State::FAILED;
                             errorMessage = tr(STR_WIFI_CONN_FAILED);
                             requestUpdate();
                             return;
                           }
                           startJob();
                         });
}

void BookAssistantActivity::startJob() {
  job = std::make_shared<Job>();
  job->mode = request.mode;
  job->apiKey = SETTINGS.recapApiKey;
  if (job->apiKey.empty() && SETTINGS.translationEngine == CrossPointSettings::ENGINE_GEMINI) {
    job->apiKey = SETTINGS.translateApiKey;
  }
  job->languageName = currentLanguageName();
  job->bookTitle = request.bookTitle;
  job->chapterTitle = request.chapterTitle;
  job->subject = request.subject;
  job->spineIndex = request.spineIndex;
  job->startsMidChapter = request.startsMidChapter;
  // Copied, not moved: a Retry after a failure needs it again.
  if (request.mode == Mode::Recap) job->excerptText = request.excerptText;
  if (usesBookContext()) {
    job->epub = epub;
    job->dir = assistantDir(epub->getCachePath());
  }

  cancelRequested = false;
  progressDone = 0;
  progressTotal = 0;
  state = State::REQUESTING;
  // Flush the waiting screen before the framebuffer goes away: the panel keeps showing it until the
  // worker asks for a progress repaint or finishes.
  requestUpdateAndWait();
  releaseFramebuffer();

  auto* handoff = new std::shared_ptr<Job>(job);
  // 10 KB: chapter extraction inflates the EPUB's zip entries on this task, on top of the HTTP
  // client and ArduinoJson (both mostly heap).
  if (xTaskCreate(workerTask, "assistant", 10240, handoff, 1, nullptr) != pdPASS) {
    delete handoff;
    job->error = tr(STR_RECAP_CONNECTION_FAILED);
    job->finished = true;
  }
}

void BookAssistantActivity::workerTask(void* param) {
  auto* handoff = static_cast<std::shared_ptr<Job>*>(param);
  Job& job = **handoff;
  runJob(job);
  job.finished = true;
  delete handoff;
  vTaskDelete(nullptr);
}

void BookAssistantActivity::serviceRepaint() {
  progressDone = job->progressDone;
  progressTotal = job->progressTotal;
  state = job->phase == Job::Phase::Preparing ? State::PREPARING : State::REQUESTING;
  if (tryRestoreFramebuffer()) {
    requestUpdateAndWait();
    releaseFramebuffer();
  }
  job->repaintRequested = false;
}

void BookAssistantActivity::finishJob() {
  restoreFramebuffer();
  const bool cancelled = job->cancelled || cancelRequested;
  if (!cancelled && !job->answer.empty()) {
    LOG_INF("RECAP", "Answer from %s: %u bytes", job->model.c_str(), static_cast<unsigned>(job->answer.size()));
    layoutAnswer(job->answer);
    currentPage = 0;
    state = State::SHOWING;
  } else {
    errorMessage = !job->error.empty() ? job->error : std::string(tr(STR_RECAP_EMPTY));
    state = State::FAILED;
  }
  job.reset();
  if (cancelled) {
    returnToReader();
    return;
  }
  requestUpdate();
}

void BookAssistantActivity::askNewQuestion() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string(tr(STR_ASSISTANT_ASK)), std::string(),
                                              MAX_QUESTION_CHARS, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const std::string question = trimmed(std::get<KeyboardResult>(result.data).text);
        if (question.empty()) {
          requestUpdate();
          return;
        }
        request.mode = Mode::Question;
        request.subject = question;
        launchWifiOrStart();
      });
}

// ─── framebuffer lifecycle (see ChapterTranslationActivity) ─────────────────

void BookAssistantActivity::releaseFramebuffer() {
  RenderLock lock;
  if (!renderer.hasFrameBuffer()) return;
  if (!renderer.releaseFrameBufferForNetwork()) LOG_ERR("RECAP", "Framebuffer release failed");
}

bool BookAssistantActivity::tryRestoreFramebuffer() {
  if (renderer.hasFrameBuffer()) return true;
  RenderLock lock;
  if (renderer.hasFrameBuffer()) return true;
  return renderer.restoreFrameBufferAfterNetwork();
}

void BookAssistantActivity::restoreFramebuffer(const bool alreadyLocked) {
  if (renderer.hasFrameBuffer()) return;
  // Wi-Fi holds ~55 KB; on an X3 the 48 KB block cannot come back while it is up. Another
  // question reconnects through launchWifiOrStart().
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }
  for (int attempt = 0; attempt < 5; attempt++) {
    bool ok;
    if (alreadyLocked) {
      ok = renderer.restoreFrameBufferAfterNetwork();
    } else {
      RenderLock lock;
      if (renderer.hasFrameBuffer()) return;
      ok = renderer.restoreFrameBufferAfterNetwork();
    }
    if (ok) return;
    LOG_ERR("RECAP", "Framebuffer realloc failed (attempt %d/5)", attempt + 1);
    delay(100);
  }
  // Restart without the boot splash, straight back to the book (the assistant opens from it).
  LOG_ERR("RECAP", "Framebuffer realloc permanently failed; silent restart");
  silentRestartToReader();
}

// ─── layout & input ──────────────────────────────────────────────────────────

void BookAssistantActivity::layoutAnswer(const std::string& answer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  lineHeight = renderer.getLineHeight(BODY_FONT_ID);
  bodyTop = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
  const int bodyBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  linesPerPage = std::max(1, (bodyBottom - bodyTop) / std::max(1, lineHeight));

  // A free-form question is repeated above its answer.
  std::string text = answer;
  if (request.mode == Mode::Question) text = "\xc2\xbb " + request.subject + "\n" + answer;

  lines.clear();
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string paragraph = text.substr(start, end - start);
    if (paragraph.find_first_not_of(" \t\r") != std::string::npos) {
      // One blank line between paragraphs, but never at the top of a page.
      if (!lines.empty() && static_cast<int>(lines.size()) % linesPerPage != 0) lines.emplace_back();
      for (auto& line : renderer.wrappedText(BODY_FONT_ID, paragraph.c_str(), width, MAX_LINES_PER_PARAGRAPH)) {
        lines.push_back(std::move(line));
      }
    }
    start = end + 1;
  }
}

int BookAssistantActivity::pageCount() const {
  return std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
}

void BookAssistantActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == State::REQUESTING || state == State::PREPARING) {
    if (!job) return;
    if (job->finished) {
      finishJob();
      return;
    }
    if (job->repaintRequested) {
      serviceRepaint();
      return;
    }
    // Back (or a tap on the bottom strip) cancels at the next safe point: between chapters, or after
    // the request in flight.
    int x = 0;
    int y = 0;
    const bool bottomTap = mappedInput.wasScreenTapped(x, y) && y >= renderer.getScreenHeight() * 85 / 100;
    if (!cancelRequested && (mappedInput.wasReleased(Button::Back) || bottomTap)) {
      cancelRequested = true;
      job->cancel = true;
    }
    return;
  }
  if (state == State::WIFI) return;

  const bool headerBack = TouchHeaderBackButton::wasTapped(mappedInput, renderer);
  if (headerBack || mappedInput.wasReleased(Button::Back)) {
    returnToReader();
    return;
  }

  if (state == State::FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      launchWifiOrStart();  // Retry
    }
    return;
  }

  // SHOWING: page through the answer.
  const bool confirm = mappedInput.wasReleased(Button::Confirm);
  int delta = 0;
  if (mappedInput.wasReleased(Button::PageForward) || mappedInput.wasReleased(Button::Right) ||
      mappedInput.wasReleased(Button::Down) || confirm) {
    delta = 1;
  } else if (mappedInput.wasReleased(Button::PageBack) || mappedInput.wasReleased(Button::Left) ||
             mappedInput.wasReleased(Button::Up)) {
    delta = -1;
  } else {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
      delta = 1;
    } else if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
      delta = -1;
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        // Touch: the hint strip's confirm area on the last page asks a new question.
        const auto& metrics = UITheme::getInstance().getMetrics();
        const bool inHints = y >= renderer.getScreenHeight() - metrics.buttonHintsHeight;
        if (inHints && currentPage + 1 >= pageCount()) {
          if (usesBookContext()) {
            askNewQuestion();
          } else {
            returnToReader();
          }
          return;
        }
        delta = x < renderer.getScreenWidth() / 3 ? -1 : 1;
      }
    }
  }
  if (delta == 0) return;

  const int next = currentPage + delta;
  if (next >= pageCount() && delta > 0 && confirm) {
    // Confirm on the last page: another question (context modes) or close (Recap).
    if (usesBookContext()) {
      askNewQuestion();
    } else {
      returnToReader();
    }
    return;
  }
  if (next < 0 || next >= pageCount()) return;
  currentPage = next;
  requestUpdate();
}

// ─── rendering ───────────────────────────────────────────────────────────────

const char* BookAssistantActivity::title() const {
  switch (request.mode) {
    case Mode::Recap:
      return tr(STR_RECAP);
    case Mode::Characters:
      return tr(STR_ASSISTANT_CHARACTERS);
    case Mode::Question:
      return tr(STR_ASSISTANT_ANSWER);
    case Mode::WhoIs:
      return request.subject.c_str();
  }
  return tr(STR_BOOK_ASSISTANT);
}

void BookAssistantActivity::renderHeader(const char* subtitle) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header{0, metrics.topPadding, renderer.getScreenWidth(),
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title(), false, 0, subtitle);
  } else {
    GUI.drawHeader(renderer, header, title(), subtitle);
  }
}

void BookAssistantActivity::renderCentered(const char* heading, const std::string& message, const char* hint) {
  renderHeader(request.chapterTitle.empty() ? nullptr : request.chapterTitle.c_str());
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  int y = renderer.getScreenHeight() / 3;
  renderer.drawCenteredText(UI_12_FONT_ID, y, heading, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) * 2;
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, message.c_str(), width, 6)) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += lh;
  }
  if (hint) {
    y += lh;
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, hint, width, 3)) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += lh;
    }
  }
}

void BookAssistantActivity::render(RenderLock&&) {
  // While the framebuffer is lent to the network stack there is nothing to draw on.
  if (!renderer.hasFrameBuffer()) return;
  renderer.clearScreen();
  char buf[96];

  switch (state) {
    case State::WIFI:
      renderCentered(title(), tr(STR_CONNECTING), nullptr);
      break;

    case State::PREPARING: {
      snprintf(buf, sizeof(buf), tr(STR_ASSISTANT_PREPARING_FORMAT), std::min(progressDone + 1, progressTotal),
               progressTotal);
      renderCentered(tr(STR_ASSISTANT_PREPARING), buf, tr(STR_ASSISTANT_PREPARING_HINT));
      renderer.drawCenteredText(UI_10_FONT_ID,
                                renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight -
                                    renderer.getLineHeight(UI_10_FONT_ID) * 2,
                                tr(STR_BACK_TO_CANCEL));
      break;
    }

    case State::REQUESTING:
      if (request.mode == Mode::Recap) {
        snprintf(buf, sizeof(buf), tr(STR_RECAP_PAGES_FORMAT), request.excerptPages);
        renderCentered(tr(STR_RECAP_GENERATING), buf, tr(STR_RECAP_PLEASE_WAIT));
      } else {
        renderCentered(tr(STR_ASSISTANT_THINKING), request.mode == Mode::Question ? request.subject : "",
                       tr(STR_RECAP_PLEASE_WAIT));
      }
      break;

    case State::FAILED: {
      renderCentered(tr(STR_RECAP_FAILED), errorMessage, nullptr);
      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RETRY), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case State::SHOWING: {
      snprintf(buf, sizeof(buf), "%d/%d", currentPage + 1, pageCount());
      renderHeader(buf);
      const int first = currentPage * linesPerPage;
      const int last = std::min(static_cast<int>(lines.size()), first + linesPerPage);
      int y = bodyTop;
      for (int i = first; i < last; i++) {
        if (!lines[i].empty()) renderer.drawText(BODY_FONT_ID, SIDE_MARGIN, y, lines[i].c_str());
        y += lineHeight;
      }
      const bool lastPage = currentPage + 1 >= pageCount();
      const char* confirmLabel = !lastPage           ? tr(STR_NEXT_PAGE)
                                 : usesBookContext() ? tr(STR_ASSISTANT_NEW_QUESTION)
                                                     : tr(STR_DONE);
      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel,
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }
  renderer.displayBuffer();
}
