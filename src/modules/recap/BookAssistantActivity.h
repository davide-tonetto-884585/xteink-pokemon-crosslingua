#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

class Epub;

/**
 * Book assistant: asks Gemini (Google AI Studio) about the book the reader is reading and shows the
 * answer, paged.
 *
 * Modes:
 *  - Recap: summary of the last pages read in the current chapter. The excerpt comes in memory.
 *  - Characters / Question / WhoIs: the main characters so far, a free-form question, or "who is X"
 *    for a term selected on the page. Their context is the whole book up to the reader's position,
 *    sent the hybrid way: a summary of every earlier chapter (generated once with Gemini and cached
 *    on the SD card under <book cache>/assistant/) plus the full text of the current chapter up to
 *    the reader's page (written to <book cache>/assistant/current.txt by the reader). The request
 *    body is assembled on the SD card and streamed (HttpDownloader::postFile), so it never has to
 *    fit in RAM.
 *
 * Launched by EpubReaderActivity, which tears itself down (replaceActivity) to leave the TLS
 * handshake the heap it needs; Back relaunches the reader at the same position. Flow: Wi-Fi
 * pre-flight -> PREPARING (missing chapter summaries, with a progress screen repainted between
 * chapters) -> REQUESTING -> SHOWING, or FAILED with the reason. Back during PREPARING cancels at
 * the next chapter boundary; summaries already written are kept for next time.
 */
class BookAssistantActivity final : public Activity {
 public:
  enum class Mode : uint8_t { Recap, Characters, Question, WhoIs };

  struct Request {
    Mode mode = Mode::Recap;
    std::string bookTitle;
    std::string chapterTitle;
    int spineIndex = 0;
    // Recap: the excerpt itself and how many pages it spans.
    std::string excerptText;
    int excerptPages = 0;
    // Recap: the excerpt starts after the chapter's first page. Other modes: current.txt does.
    bool startsMidChapter = false;
    // Question text, or the term selected for WhoIs.
    std::string subject;
  };

  // Where the reader writes the current chapter's text for the context modes.
  static std::string assistantDir(const std::string& bookCachePath) { return bookCachePath + "/assistant"; }
  static std::string currentChapterTextPath(const std::string& bookCachePath) {
    return assistantDir(bookCachePath) + "/current.txt";
  }

  explicit BookAssistantActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                 Request request)
      : Activity("BookAssistant", renderer, mappedInput),
        epubPath(std::move(epubPath)),
        request(std::move(request)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state != State::SHOWING && state != State::FAILED; }

  // Worker-task state; defined in the .cpp.
  struct Job;

 private:
  enum class State : uint8_t { WIFI, PREPARING, REQUESTING, SHOWING, FAILED };

  std::string epubPath;
  Request request;
  std::shared_ptr<Epub> epub;  // lean, metadata-only; context modes only
  State state = State::WIFI;
  std::shared_ptr<Job> job;
  std::string errorMessage;
  bool cancelRequested = false;
  // Progress snapshot drawn by render() during PREPARING.
  int progressDone = 0;
  int progressTotal = 0;

  // Answer laid out for the screen (empty strings are paragraph gaps).
  std::vector<std::string> lines;
  int linesPerPage = 1;
  int currentPage = 0;
  int bodyTop = 0;
  int lineHeight = 0;

  bool usesBookContext() const { return request.mode != Mode::Recap; }
  bool ensureEpubLoaded();
  void returnToReader();
  void launchWifiOrStart();
  void startJob();
  static void workerTask(void* param);
  void serviceRepaint();
  void finishJob();
  void askNewQuestion();
  void layoutAnswer(const std::string& answer);
  int pageCount() const;
  const char* title() const;

  void releaseFramebuffer();
  bool tryRestoreFramebuffer();
  void restoreFramebuffer(bool alreadyLocked = false);

  void renderHeader(const char* subtitle);
  void renderCentered(const char* heading, const std::string& message, const char* hint);
};
