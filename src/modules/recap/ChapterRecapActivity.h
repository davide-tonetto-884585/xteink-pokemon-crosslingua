#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

/**
 * Chapter recap: sends the last pages the reader read in the current chapter to Gemini (Google AI
 * Studio) and shows the returned summary, paged, so the user can catch up on what they read.
 *
 * Launched by EpubReaderActivity, which extracts the excerpt text while its section is still loaded
 * and then tears itself down (replaceActivity) to leave this activity the heap a TLS handshake
 * needs - the same hand-off ChapterTranslationActivity uses. Back relaunches the reader at the same
 * position via ActivityManager::goToReader().
 *
 * Flow: Wi-Fi pre-flight (WifiSelectionActivity if not connected) -> REQUESTING (framebuffer
 * released, one worker task walks GEMINI_MODELS) -> SHOWING the paged recap, or FAILED with the
 * reason.
 */
class ChapterRecapActivity final : public Activity {
 public:
  struct Excerpt {
    std::string text;
    std::string bookTitle;
    std::string chapterTitle;
    int pageCount = 0;
    bool startsMidChapter = false;
  };

  explicit ChapterRecapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                Excerpt excerpt)
      : Activity("ChapterRecap", renderer, mappedInput), epubPath(std::move(epubPath)), excerpt(std::move(excerpt)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::REQUESTING || state == State::WIFI; }

 private:
  enum class State : uint8_t { WIFI, REQUESTING, SHOWING, FAILED };

  // Shared with the worker task, so a worker still blocked in an HTTP call can never outlive what it
  // writes to: whoever drops the last reference frees it.
  struct Job {
    std::string apiKey;
    std::string requestText;
    std::string bookTitle;
    std::string chapterTitle;
    std::string languageName;
    bool startsMidChapter = false;
    // Outputs, written by the worker before `done` is published.
    std::string recap;
    std::string error;
    std::string model;
    volatile bool done = false;
  };

  std::string epubPath;
  Excerpt excerpt;
  State state = State::WIFI;
  std::shared_ptr<Job> job;
  std::string errorMessage;

  // Recap laid out for the screen (empty strings are paragraph gaps).
  std::vector<std::string> lines;
  int linesPerPage = 1;
  int currentPage = 0;
  int bodyTop = 0;
  int lineHeight = 0;

  void returnToReader();
  void launchWifiOrStart();
  void startRequest();
  static void workerTask(void* param);
  static void runJob(Job& job);
  void finishRequest();
  void layoutRecap();
  int pageCount() const;

  void releaseFramebuffer();
  void restoreFramebuffer(bool alreadyLocked = false);

  void renderHeader(const char* subtitle);
  void renderCentered(const char* title, const std::string& message, const char* hint);
};
