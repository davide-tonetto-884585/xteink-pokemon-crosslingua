#include "modules/recap/ChapterRecapActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "modules/recap/GeminiRecap.h"
#include "network/HttpDownloader.h"

namespace {
constexpr int BODY_FONT_ID = UI_12_FONT_ID;
constexpr int SIDE_MARGIN = 20;
// Generous cap for one paragraph's wrapped lines; a recap paragraph is a few lines.
constexpr int MAX_LINES_PER_PARAGRAPH = 200;

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
}  // namespace

// ─── lifecycle ───────────────────────────────────────────────────────────────

void ChapterRecapActivity::onEnter() {
  Activity::onEnter();
  if (excerpt.text.empty()) {
    state = State::FAILED;
    errorMessage = tr(STR_RECAP_NO_TEXT);
    requestUpdate();
    return;
  }
  launchWifiOrStart();
}

void ChapterRecapActivity::onExit() {
  Activity::onExit();
  // A worker still blocked in its HTTP call keeps its own reference to the job, so it is simply
  // abandoned here; it frees the job when the call returns.
  job.reset();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  // onExit() runs under the ActivityManager's RenderLock; the next activity renders immediately.
  restoreFramebuffer(/*alreadyLocked=*/true);
}

void ChapterRecapActivity::returnToReader() { activityManager.goToReader(epubPath); }

// ─── network ─────────────────────────────────────────────────────────────────

void ChapterRecapActivity::launchWifiOrStart() {
  state = State::WIFI;
  requestUpdate();

  WiFi.mode(WIFI_STA);
#if defined(SIMULATOR)
  // Same as the Lingua activities: the simulator's fake scan confuses the Wi-Fi picker.
  if (WiFi.status() != WL_CONNECTED) WiFi.begin();
#endif
  if (WiFi.status() == WL_CONNECTED) {
    startRequest();
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
                           startRequest();
                         });
}

void ChapterRecapActivity::startRequest() {
  job = std::make_shared<Job>();
  job->apiKey = SETTINGS.recapApiKey;
  if (job->apiKey.empty() && SETTINGS.translationEngine == CrossPointSettings::ENGINE_GEMINI) {
    job->apiKey = SETTINGS.translateApiKey;
  }
  // Copied, not moved: a Retry after a failure needs the excerpt again.
  job->requestText = excerpt.text;
  job->bookTitle = excerpt.bookTitle;
  job->chapterTitle = excerpt.chapterTitle;
  job->languageName = currentLanguageName();
  job->startsMidChapter = excerpt.startsMidChapter;

  state = State::REQUESTING;
  // Flush the "Generating..." screen before the framebuffer goes away: the panel keeps showing it
  // for the whole request.
  requestUpdateAndWait();
  releaseFramebuffer();

  auto* handoff = new std::shared_ptr<Job>(job);
  // 8 KB: the HTTP client and ArduinoJson both allocate on the heap; the stack only holds frames.
  if (xTaskCreate(workerTask, "recapReq", 8192, handoff, 1, nullptr) != pdPASS) {
    delete handoff;
    job->error = tr(STR_RECAP_CONNECTION_FAILED);
    job->done = true;
  }
}

void ChapterRecapActivity::workerTask(void* param) {
  auto* handoff = static_cast<std::shared_ptr<Job>*>(param);
  runJob(**handoff);
  delete handoff;
  vTaskDelete(nullptr);
}

void ChapterRecapActivity::runJob(Job& job) {
  if (job.apiKey.empty()) {
    job.error = tr(STR_RECAP_INVALID_KEY);
    job.done = true;
    return;
  }

  recap::RecapRequest request{job.requestText};
  request.languageName = job.languageName.c_str();
  request.bookTitle = job.bookTitle.c_str();
  request.chapterTitle = job.chapterTitle.c_str();
  request.startsMidChapter = job.startsMidChapter;

#ifndef SIMULATOR
  // Same as the Lingua translation tasks: resolve googleapis.com through public DNS (some routers'
  // resolvers fail on it) and give the new settings a moment before the first connect.
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), IPAddress(8, 8, 8, 8), IPAddress(8, 8, 4, 4));
  delay(500);
#endif

  std::string response;
  ReusableHttpSession heapGate;
  for (size_t modelIndex = 0; modelIndex < recap::GEMINI_MODEL_COUNT; modelIndex++) {
    const char* model = recap::GEMINI_MODELS[modelIndex];
    request.lowThinking = true;
    int transportRetries = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
      // The TLS handshake needs a large contiguous block; wait briefly for the heap to settle.
      heapGate.waitForHeapReady(5000, nullptr);
      std::string body = recap::buildGeminiRequestBody(request);
      LOG_DBG("RECAP", "POST %s (%u bytes, heap %u/%u)", model, static_cast<unsigned>(body.size()),
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      response.clear();
      const bool ok = HttpDownloader::post(recap::geminiEndpoint(model), body, "application/json",
                                           recap::GEMINI_API_KEY_HEADER, job.apiKey.c_str(), response);
      const int code = HttpDownloader::lastHttpCode;
      body.clear();
      body.shrink_to_fit();

      if (ok) {
        std::string text;
        const auto status = recap::parseGeminiResponse(response, text);
        if (status == recap::RecapParseStatus::Ok) {
          job.recap = std::move(text);
          job.model = model;
        } else {
          job.error = status == recap::RecapParseStatus::Blocked ? tr(STR_RECAP_BLOCKED) : tr(STR_RECAP_EMPTY);
          LOG_ERR("RECAP", "%s: unusable response (%.120s)", model, response.c_str());
        }
        job.done = true;
        return;
      }

      LOG_ERR("RECAP", "%s failed: HTTP %d %s (%.160s)", model, code,
              HttpDownloader::lastErrorName ? HttpDownloader::lastErrorName : "", response.c_str());
      // Connection-level failure (DNS, TLS, timeout): try the same model again after a pause.
      if (code <= 0 && transportRetries < 2) {
        transportRetries++;
        delay(1500);
        continue;
      }
      // A model that does not accept the thinking setting answers 400 on an otherwise valid
      // request: retry it once without the setting before giving up on it.
      const bool keyRejected = recap::parseGeminiError(response).find("API key") != std::string::npos;
      if (code == 400 && request.lowThinking && !keyRejected) {
        request.lowThinking = false;
        continue;
      }
      job.error = describeHttpFailure(code, response);
      if (!recap::shouldTryNextModel(code)) {
        job.done = true;
        return;
      }
      break;  // next model
    }
  }
  job.done = true;
}

void ChapterRecapActivity::finishRequest() {
  restoreFramebuffer();
  if (job && !job->recap.empty()) {
    LOG_INF("RECAP", "Recap from %s: %u bytes", job->model.c_str(), static_cast<unsigned>(job->recap.size()));
    layoutRecap();
    currentPage = 0;
    state = State::SHOWING;
  } else {
    errorMessage = job && !job->error.empty() ? job->error : std::string(tr(STR_RECAP_EMPTY));
    state = State::FAILED;
  }
  job.reset();
  // The radio is no longer needed; free its heap for the reader that comes next.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  requestUpdate();
}

// ─── framebuffer lifecycle (see ChapterTranslationActivity) ─────────────────

void ChapterRecapActivity::releaseFramebuffer() {
  RenderLock lock;
  if (!renderer.hasFrameBuffer()) return;
  if (!renderer.releaseFrameBufferForNetwork()) LOG_ERR("RECAP", "Framebuffer release failed");
}

void ChapterRecapActivity::restoreFramebuffer(const bool alreadyLocked) {
  if (renderer.hasFrameBuffer()) return;
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
  LOG_ERR("RECAP", "Framebuffer realloc permanently failed; restarting");
  ESP.restart();
}

// ─── layout & input ──────────────────────────────────────────────────────────

void ChapterRecapActivity::layoutRecap() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  lineHeight = renderer.getLineHeight(BODY_FONT_ID);
  bodyTop = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
  const int bodyBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  linesPerPage = std::max(1, (bodyBottom - bodyTop) / std::max(1, lineHeight));

  lines.clear();
  const std::string& text = job->recap;
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

int ChapterRecapActivity::pageCount() const {
  return std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
}

void ChapterRecapActivity::loop() {
  using Button = MappedInputManager::Button;

  if (state == State::REQUESTING) {
    // The request cannot be interrupted mid-flight (the HTTP client blocks); input waits for it.
    if (job && job->done) finishRequest();
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

  // SHOWING: page through the recap.
  int delta = 0;
  if (mappedInput.wasReleased(Button::PageForward) || mappedInput.wasReleased(Button::Right) ||
      mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::Confirm)) {
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
      if (mappedInput.wasScreenTapped(x, y)) delta = x < renderer.getScreenWidth() / 3 ? -1 : 1;
    }
  }
  if (delta == 0) return;

  const int next = currentPage + delta;
  if (next >= pageCount() && delta > 0 && mappedInput.wasReleased(Button::Confirm)) {
    returnToReader();  // Confirm on the last page closes the recap.
    return;
  }
  if (next < 0 || next >= pageCount()) return;
  currentPage = next;
  requestUpdate();
}

// ─── rendering ───────────────────────────────────────────────────────────────

void ChapterRecapActivity::renderHeader(const char* subtitle) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header{0, metrics.topPadding, renderer.getScreenWidth(),
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_RECAP), false, 0, subtitle);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_RECAP), subtitle);
  }
}

void ChapterRecapActivity::renderCentered(const char* title, const std::string& message, const char* hint) {
  renderHeader(excerpt.chapterTitle.empty() ? nullptr : excerpt.chapterTitle.c_str());
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  int y = renderer.getScreenHeight() / 3;
  renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) * 2;
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, message.c_str(), width, 6)) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += lh;
  }
  if (hint) {
    y += lh;
    renderer.drawCenteredText(UI_10_FONT_ID, y, hint);
  }
}

void ChapterRecapActivity::render(RenderLock&&) {
  // While the framebuffer is lent to the network stack there is nothing to draw on.
  if (!renderer.hasFrameBuffer()) return;
  renderer.clearScreen();
  char buf[96];

  switch (state) {
    case State::WIFI:
      renderCentered(tr(STR_RECAP), tr(STR_CONNECTING), nullptr);
      break;

    case State::REQUESTING:
      snprintf(buf, sizeof(buf), tr(STR_RECAP_PAGES_FORMAT), excerpt.pageCount);
      renderCentered(tr(STR_RECAP_GENERATING), buf, tr(STR_RECAP_PLEASE_WAIT));
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
      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                                lastPage ? tr(STR_DONE) : tr(STR_NEXT_PAGE), tr(STR_DIR_UP),
                                                tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }
  renderer.displayBuffer();
}
