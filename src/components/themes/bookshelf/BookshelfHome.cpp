#include "BookshelfHome.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "BookshelfWallpaper.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "fontIds.h"

#if defined(CROSSINK_ENABLE_POKEMON)
#include "components/pokemon/PokemonArtPath.h"
#endif

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <esp_system.h>
#endif

namespace {

constexpr const char* CACHE_ROOT = "/.crosspoint";
constexpr int MARGIN = 16;
constexpr int PLANK_HEIGHT = 8;
constexpr int SHELF_TOP_GAP = 10;
// Widest slot the bookend Pokemon gets; a wider one is scaled down to fit.
constexpr int BOOKEND_MAX_WIDTH = 150;
constexpr int BOOKEND_PAD = 3;  // room for its halo
constexpr size_t MAX_READ_BOOKS = 30;
constexpr size_t UNREAD_PICKS = 30;
constexpr int SCAN_MAX_DEPTH = 3;
// Directory entries visited in total, and in any one folder: a folder of
// thousands of images must not use up the whole budget before the books.
constexpr int SCAN_MAX_ENTRIES = 2500;
constexpr int SCAN_MAX_ENTRIES_PER_FOLDER = 400;

// Folders that hold the firmware's own assets, never books.
bool isAssetFolder(const char* name) {
  for (const char* asset : {"System Volume Information", "pokemon", "sleep", "fonts", "screensaver", "screensavers"}) {
    if (strcasecmp(name, asset) == 0) return true;
  }
  return false;
}

uint32_t randomSeed() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return esp_random();
#else
  return 0x9E3779B9U;
#endif
}

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool isBookFile(const char* name) {
  const std::string_view view(name);
  return FsHelpers::hasEpubExtension(view) || FsHelpers::hasXtcExtension(view) || FsHelpers::hasTxtExtension(view);
}

bool hasStartedReading(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return false;
  const std::string cache = Epub::cachePathForFilePath(path, CACHE_ROOT);
  return Storage.exists((cache + "/progress.bin").c_str()) || Storage.exists((cache + "/progress.bin.bak").c_str());
}

// Reservoir sampling over the SD tree: keeps a uniform random pick of at most
// `picks.size()` book files in constant memory however big the library is.
struct UnreadScan {
  std::vector<std::string> picks;
  size_t capacity = 0;
  size_t seen = 0;
  int visited = 0;
  uint32_t random = 0;
  const std::vector<std::string>* exclude = nullptr;

  void offer(const std::string& path) {
    if (std::find(exclude->begin(), exclude->end(), path) != exclude->end()) return;
    ++seen;
    if (picks.size() < capacity) {
      picks.push_back(path);
    } else {
      const size_t slot = nextRandom(random) % seen;
      if (slot < capacity) picks[slot] = path;
    }
  }

  void walk(const std::string& folder, const int depth) {
    HalFile dir = Storage.open(folder.c_str());
    if (!dir || !dir.isDirectory()) return;
    char name[256];
    int visitedHere = 0;
    for (HalFile entry = dir.openNextFile();
         entry && visited < SCAN_MAX_ENTRIES && visitedHere < SCAN_MAX_ENTRIES_PER_FOLDER;
         entry = dir.openNextFile()) {
      ++visited;
      ++visitedHere;
      name[0] = '\0';
      entry.getName(name, sizeof(name));
      const bool isDirectory = entry.isDirectory();
      entry.close();
      // Hidden and system folders (.crosspoint, System Volume Information...).
      if (name[0] == '\0' || name[0] == '.' || (isDirectory && isAssetFolder(name))) continue;
      const std::string path = folder == "/" ? "/" + std::string(name) : folder + "/" + name;
      if (isDirectory) {
        if (depth < SCAN_MAX_DEPTH) walk(path, depth + 1);
      } else if (isBookFile(name)) {
        offer(path);
      }
    }
    dir.close();
  }
};

std::vector<std::string>& sessionUnreadPicks() {
  static std::vector<std::string> picks;
  return picks;
}

bool& sessionUnreadScanned() {
  static bool scanned = false;
  return scanned;
}

void drawWrappedLines(const GfxRenderer& renderer, int fontId, int x, int& y, int width, const char* text, int maxLines,
                      EpdFontFamily::Style style) {
  const auto lines = renderer.wrappedText(fontId, text, width, maxLines, style);
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    renderer.drawText(fontId, x, y, line.c_str(), true, style);
    y += lineHeight;
  }
}

bool drawThumb(const GfxRenderer& renderer, const std::string& path, Rect rect) {
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  FsFile file;
  if (!Storage.openFileForRead("SHELF", path, file)) return false;
  Bitmap bitmap(file);
  bool drawn = false;
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    drawn = renderer.drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height);
  }
  file.close();
  return drawn;
}

}  // namespace

namespace {
// A face-on book looks best a few books in, not jammed against the shelf end.
constexpr size_t FRONT_POSITION = 4;

void moveFrontBookInward(std::vector<BookshelfHome::Book>& books) {
  for (size_t i = 0; i < books.size(); ++i) {
    if (books[i].frontThumbPath.empty()) continue;
    const size_t target = std::min(FRONT_POSITION, books.size() - 1);
    if (i == target) return;
    BookshelfHome::Book front = std::move(books[i]);
    books.erase(books.begin() + static_cast<std::ptrdiff_t>(i));
    books.insert(books.begin() + static_cast<std::ptrdiff_t>(target), std::move(front));
    return;
  }
}
}  // namespace

void BookshelfHome::loadReadShelf(const std::string& currentPath,
                                  const std::vector<std::pair<std::string, std::string>>& frontThumbs) {
  read_.clear();
  for (const RecentBook& recent : RECENT_BOOKS.getBooks()) {
    if (read_.size() >= MAX_READ_BOOKS) break;
    if (recent.path == currentPath || !Storage.exists(recent.path.c_str())) continue;
    Book book;
    book.path = recent.path;
    book.title = recent.title.empty() ? bookshelf::titleFromFileName(recent.path.c_str()) : recent.title;
    book.author = recent.author;
    book.key = bookshelf::keyForPath(recent.path.c_str());
    book.started = true;
    if (FsHelpers::hasEpubExtension(recent.path)) {
      book.completed = BookReadingStats::load(Epub::cachePathForFilePath(recent.path, CACHE_ROOT)).isCompleted;
    }
    for (const auto& [path, thumb] : frontThumbs) {
      if (path == recent.path) book.frontThumbPath = thumb;
    }
    read_.push_back(std::move(book));
  }
  moveFrontBookInward(read_);
}

void BookshelfHome::loadUnreadShelf(const std::string& currentPath) {
  if (!sessionUnreadScanned()) {
    std::vector<std::string> exclude;
    for (const RecentBook& recent : RECENT_BOOKS.getBooks()) exclude.push_back(recent.path);
    UnreadScan scan;
    // Over-sample: a few picks may turn out to be already started.
    scan.capacity = UNREAD_PICKS + 10;
    scan.random = randomSeed() | 1U;
    scan.exclude = &exclude;
    scan.walk("/", 0);
    auto& picks = sessionUnreadPicks();
    picks.clear();
    for (const std::string& path : scan.picks) {
      if (picks.size() >= UNREAD_PICKS) break;
      if (!hasStartedReading(path)) picks.push_back(path);
    }
    sessionUnreadScanned() = true;
    LOG_INF("SHELF", "Unread scan: visited=%d books=%u picked=%u", scan.visited, static_cast<unsigned>(scan.seen),
            static_cast<unsigned>(picks.size()));
  }
  unread_.clear();
  for (const std::string& path : sessionUnreadPicks()) {
    if (path == currentPath) continue;
    Book book;
    book.path = path;
    book.title = bookshelf::titleFromFileName(path.c_str());
    book.key = bookshelf::keyForPath(path.c_str());
    unread_.push_back(std::move(book));
  }
  unreadLoaded_ = true;
}


std::vector<std::string> BookshelfHome::unreadFrontCandidates() const {
  // Only among books that will actually fit on the shelf.
  std::vector<std::string> candidates;
  for (size_t i = 0; i < unread_.size() && i < 14 && candidates.size() < 3; ++i) {
    if (FsHelpers::hasEpubExtension(unread_[i].path)) candidates.push_back(unread_[i].path);
  }
  return candidates;
}

void BookshelfHome::setUnreadFront(const std::string& path, const std::string& thumbPath, const std::string& title,
                                   const std::string& author) {
  for (Book& book : unread_) {
    if (book.path != path) continue;
    book.frontThumbPath = thumbPath;
    if (!title.empty()) book.title = title;
    if (!author.empty()) book.author = author;
    break;
  }
  moveFrontBookInward(unread_);
}

const std::string* BookshelfHome::pathForSlot(const int slot) const {
  if (slot < 0 || static_cast<size_t>(slot) >= slotCount_ || slots_[slot].book == nullptr) return nullptr;
  return &slots_[slot].book->path;
}

int BookshelfHome::hitTest(const int x, const int y, bool& headerHit) const {
  headerHit = x >= headerRect_.x && x < headerRect_.x + headerRect_.width && y >= headerRect_.y &&
              y < headerRect_.y + headerRect_.height;
  for (size_t i = 0; i < slotCount_; ++i) {
    const Rect& r = slots_[i].rect;
    // A little slack around thin spines so they are easy to tap.
    if (x >= r.x - 2 && x < r.x + r.width + 2 && y >= r.y - 6 && y < r.y + r.height + 4) return static_cast<int>(i);
  }
  return -1;
}

void BookshelfHome::render(const GfxRenderer& renderer, const Rect area, const CurrentBook& current) {
  slotCount_ = 0;
  drawWallpaper(renderer, area);
  const int headerHeight = this->headerHeight(renderer, area, current);
  headerRect_ = Rect{area.x, area.y, area.width, headerHeight};
  drawHeader(renderer, headerRect_, current);

  const int shelvesTop = area.y + headerHeight + 6;
  const int shelfHeight = (area.y + area.height - shelvesTop) / 2;
  if (shelfHeight <= PLANK_HEIGHT + SHELF_TOP_GAP + 20) return;
  // The party's first Pokemon ends the top shelf, the second opens the bottom one.
  drawShelf(renderer, Rect{area.x, shelvesTop, area.width, shelfHeight}, read_, bookends_[0], false);
  drawShelf(renderer, Rect{area.x, shelvesTop + shelfHeight, area.width, shelfHeight}, unread_, bookends_[1], true);
}

// The damask wallpaper, tiled over the whole Home below the status bar. Drawn
// pixel by pixel from the 1-bit tile so it is right in every orientation;
// everything with text on it (the header panel, spines, the button bar) is
// then drawn on top on its own white or solid background.
void BookshelfHome::drawWallpaper(const GfxRenderer& renderer, const Rect area) const {
  constexpr int rowBytes = (bookshelf::WALLPAPER_TILE_W + 7) / 8;
  for (int y = area.y; y < area.y + area.height; ++y) {
    const int ty = (y - area.y) % bookshelf::WALLPAPER_TILE_H;
    const uint8_t* row = bookshelf::WALLPAPER_TILE + ty * rowBytes;
    for (int x = area.x; x < area.x + area.width; ++x) {
      const int tx = (x - area.x) % bookshelf::WALLPAPER_TILE_W;
      if ((row[tx >> 3] & (0x80 >> (tx & 7))) != 0) renderer.drawPixel(x, y, true);
    }
  }
}

namespace {
constexpr int HEADER_PAD = 10;

// ---- Reading-stat helpers, the same rules as CrossInk's Dashboard home ----

void formatCompactDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds + 30U) / 60U;
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60U;
  const uint32_t rest = minutes % 60U;
  if (rest == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(rest));
  }
}

// The reader's own live estimate, else extrapolated from progress so far.
bool estimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || stats.totalReadingSeconds < 120) return false;
  const float progress = progressPercent / 100.0f;
  const float estimate = static_cast<float>(stats.totalReadingSeconds) * (1.0f - progress) / progress;
  seconds = estimate > 0.0f ? static_cast<uint32_t>(estimate + 0.5f) : 0;
  return seconds > 0;
}

// Finish date at the pace read so far (reading time per calendar day).
bool estimateFinishDate(const BookReadingStats& stats, const ReadingStatsDateTime& today, const uint32_t secondsLeft,
                        ReadingStatsDate& out) {
  out = {};
  if (!today.date.isValid() || !stats.startDate.isValid() || secondsLeft == 0 || stats.totalReadingSeconds == 0) {
    return false;
  }
  const uint16_t days = std::max<uint16_t>(1, readingSpanDaysElapsed(stats.startDate, today.date));
  const uint64_t calendarSeconds = (static_cast<uint64_t>(secondsLeft) * days * 86400ULL +
                                    static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
                                   static_cast<uint64_t>(stats.totalReadingSeconds);
  if (calendarSeconds == 0) return false;
  ReadingStatsDateTime finish = today;
  addSecondsToReadingStatsDateTime(finish, static_cast<uint32_t>(std::min<uint64_t>(calendarSeconds, UINT32_MAX)));
  out = finish.date;
  return out.isValid();
}

std::string displayTitle(const RecentBook& book) {
  return book.title.empty() ? bookshelf::titleFromFileName(book.path.c_str()) : book.title;
}
}  // namespace

int BookshelfHome::headerHeight(const GfxRenderer& renderer, const Rect area, const CurrentBook& current) const {
  (void)renderer;
  (void)area;
  if (current.book == nullptr) return 60;
  return HEADER_PAD + HEADER_COVER_H + HEADER_PAD + 4;
}

void BookshelfHome::drawHeader(const GfxRenderer& renderer, const Rect area, const CurrentBook& current) const {
  // A white panel, so the title and stats read cleanly over the wallpaper.
  const Rect panel{area.x + MARGIN / 2, area.y + 2, area.width - MARGIN, area.height - 4};
  renderer.fillRoundedRect(panel.x, panel.y, panel.width, panel.height, 8, Color::White);
  renderer.drawRoundedRect(panel.x, panel.y, panel.width, panel.height, 1, 8, true);
  if (current.book == nullptr) {
    renderer.drawCenteredText(UI_12_FONT_ID, area.y + area.height / 2 - 12, tr(STR_BOOKSHELF_EMPTY), true,
                              EpdFontFamily::BOLD);
    return;
  }

  // Cover on the left.
  const Rect cover{area.x + MARGIN, area.y + HEADER_PAD + 2, HEADER_COVER_W, HEADER_COVER_H};
  const std::string title = displayTitle(*current.book);
  if (!drawThumb(renderer, current.coverThumbPath, cover)) {
    renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
    int y = cover.y + 30;
    drawWrappedLines(renderer, SMALL_FONT_ID, cover.x + 8, y, cover.width - 16, title.c_str(), 5,
                     EpdFontFamily::BOLD);
  }
  renderer.drawRect(cover.x, cover.y, cover.width, cover.height, 2, true);
  renderer.fillRect(cover.x + 4, cover.y + cover.height, cover.width, 3, true);
  renderer.fillRect(cover.x + cover.width, cover.y + 4, 3, cover.height, true);

  // Title and author beside it, then the stats in columns underneath.
  const int textX = cover.x + cover.width + MARGIN;
  const int textW = area.x + area.width - MARGIN - textX;
  int y = cover.y - 2;
  drawWrappedLines(renderer, UI_12_FONT_ID, textX, y, textW, title.c_str(), 2, EpdFontFamily::BOLD);
  if (!current.book->author.empty()) {
    drawWrappedLines(renderer, UI_10_FONT_ID, textX, y, textW, current.book->author.c_str(), 1,
                     EpdFontFamily::ITALIC);
  }
  y += 6;
  renderer.drawLine(textX, y, textX + textW, y, true);
  drawStats(renderer, textX, y + 5, textW, cover.y + cover.height + HEADER_PAD, current);
}

// CrossInk's Dashboard stats, as a compact grid: a bold value over a small
// label in each cell, two (or three, when space is short) cells per row.
void BookshelfHome::drawStats(const GfxRenderer& renderer, const int x, const int top, const int width,
                              const int bottom, const CurrentBook& current) const {
  const BookReadingStats emptyStats{};
  const BookReadingStats& stats = current.stats != nullptr ? *current.stats : emptyStats;
  const bool rtc = halClock.isAvailable();
  ReadingStatsDateTime today;
  const bool hasToday = rtc && getCurrentLocalReadingStatsDateTime(today);

  struct Cell {
    char value[32];
    char label[48];
  };
  std::array<Cell, 8> cells{};
  size_t count = 0;
  auto next = [&](const char* label) -> Cell& {
    Cell& cell = cells[count++];
    snprintf(cell.label, sizeof(cell.label), "%s", label);
    snprintf(cell.value, sizeof(cell.value), "-");
    return cell;
  };

  BookReadingStats::formatDuration(stats.totalReadingSeconds, next(tr(STR_STATS_TIME_LBL)).value, sizeof(Cell::value));
  uint32_t secondsLeft = 0;
  const bool hasEstimate = estimatedTimeLeft(stats, current.progressPercent, secondsLeft);
  Cell& left = next(tr(STR_TIME_LEFT_SHORT));
  if (hasEstimate && !stats.isCompleted) formatCompactDuration(secondsLeft, left.value, sizeof(left.value));
  Cell& progress = next(tr(STR_STATS_PROGRESS_LBL));
  if (current.progressPercent >= 0.0f) {
    snprintf(progress.value, sizeof(progress.value), "%d%%", static_cast<int>(current.progressPercent + 0.5f));
  }
  Cell& pace = next(tr(STR_STATS_PAGES_PER_MIN));
  if (stats.totalReadingSeconds > 60) {
    const uint32_t tenths = (stats.totalPagesTurned * 600U + stats.totalReadingSeconds / 2U) / stats.totalReadingSeconds;
    snprintf(pace.value, sizeof(pace.value), "%u.%u", static_cast<unsigned>(tenths / 10U),
             static_cast<unsigned>(tenths % 10U));
  }

  if (rtc) {
    const ReadingStatsDate endDate = stats.isCompleted && stats.finishedDate.isValid()
                                         ? stats.finishedDate
                                         : (hasToday ? today.date : ReadingStatsDate{});
    const bool hasDaySpan = stats.startDate.isValid() && endDate.isValid();
    const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(stats.startDate, endDate) : 0;
    Cell& daily = next(tr(STR_STATS_DAILY_AVG_LBL));
    if (hasDaySpan) {
      BookReadingStats::formatDuration(stats.totalReadingSeconds / std::max<uint16_t>(1, daysReading), daily.value,
                                       sizeof(daily.value));
    }
    Cell& started = next(tr(STR_STATS_STARTED));
    if (stats.startDate.isValid()) formatReadingStatsShortDate(stats.startDate, started.value, sizeof(started.value));
    Cell& finish = next(stats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE));
    ReadingStatsDate finishDate;
    if (stats.isCompleted) {
      finishDate = stats.finishedDate;
    } else if (hasToday && hasEstimate && !estimateFinishDate(stats, today, secondsLeft, finishDate)) {
      ReadingStatsDateTime estimated = today;
      addSecondsToReadingStatsDateTime(estimated, secondsLeft);
      finishDate = estimated.date;
    }
    if (finishDate.isValid()) formatReadingStatsShortDate(finishDate, finish.value, sizeof(finish.value));
    snprintf(next(tr(STR_STATS_SESSIONS_LBL)).value, sizeof(Cell::value), "%u",
             static_cast<unsigned>(stats.sessionCount));
  } else {
    // No clock (X4): no dates, so sessions and the average session instead.
    snprintf(next(tr(STR_STATS_SESSIONS_LBL)).value, sizeof(Cell::value), "%u",
             static_cast<unsigned>(stats.sessionCount));
    Cell& avg = next(tr(STR_STATS_AVG_SESSION_LBL));
    BookReadingStats::formatDuration(stats.sessionCount > 0 ? stats.totalReadingSeconds / stats.sessionCount : 0,
                                     avg.value, sizeof(avg.value));
  }

  const int valueH = renderer.getLineHeight(SMALL_FONT_ID);
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID) - 2;
  const int cellH = valueH + labelH + 2;
  const int available = bottom - top;
  int columns = 2;
  int rows = (static_cast<int>(count) + columns - 1) / columns;
  if (rows * cellH > available) {
    columns = 3;
    rows = (static_cast<int>(count) + columns - 1) / columns;
  }
  // Drop the last cells rather than overflow the panel on a very small screen.
  const int fitRows = std::max(1, std::min(rows, available / cellH));
  const int shown = std::min(static_cast<int>(count), fitRows * columns);
  const int columnW = width / columns;
  for (int i = 0; i < shown; ++i) {
    const int cx = x + (i % columns) * columnW;
    const int cy = top + (i / columns) * cellH;
    const std::string value = renderer.truncatedText(SMALL_FONT_ID, cells[i].value, columnW - 6, EpdFontFamily::BOLD);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, cells[i].label, columnW - 6);
    renderer.drawText(SMALL_FONT_ID, cx, cy, value.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, cx, cy + valueH - 1, label.c_str());
  }
}

void BookshelfHome::drawShelf(const GfxRenderer& renderer, const Rect area, const std::vector<Book>& books,
                              const Bookend& bookend, const bool bookendFirst) {
  // Books stand on the board's top surface.
  const int boardTop = area.y + area.height - PLANK_HEIGHT - 8;
  const int standY = boardTop + 4;
  bookshelf::ShelfGeometry geometry;
  geometry.width = area.width - 2 * MARGIN;
  geometry.maxHeight = standY - area.y - SHELF_TOP_GAP;
  geometry.frontWidth = FRONT_COVER_W;
  geometry.bookendWidth = bookend.width > 0 ? std::min(bookend.width, BOOKEND_MAX_WIDTH) : 0;
  geometry.bookendFirst = bookendFirst;

  std::array<bookshelf::ShelfItem, bookshelf::MAX_PLACEMENTS> items{};
  const size_t count = std::min(books.size(), items.size());
  for (size_t i = 0; i < count; ++i) {
    items[i].key = books[i].key;
    items[i].titleLength = static_cast<uint16_t>(books[i].title.size());
    items[i].front = !books[i].frontThumbPath.empty();
  }
  std::array<bookshelf::Placement, bookshelf::MAX_PLACEMENTS> placements{};
  const size_t placed = bookshelf::layoutShelf(items.data(), count, geometry, placements.data(), placements.size());

  // The board first: a pale top surface seen slightly from above, a solid
  // front edge and a soft shadow under it. Books are drawn over its back half.
  const int boardX = area.x + MARGIN - 8;
  const int boardW = area.width - 2 * MARGIN + 16;
  renderer.fillRect(boardX, boardTop, boardW, 8, false);
  renderer.fillRectDither(boardX, boardTop, boardW, 8, Color::LightGray);
  renderer.drawLine(boardX, boardTop, boardX + boardW - 1, boardTop, true);
  renderer.fillRect(boardX, boardTop + 8, boardW, PLANK_HEIGHT, true);
  renderer.drawLine(boardX + 2, boardTop + 10, boardX + boardW - 3, boardTop + 10, false);
  renderer.fillRectDither(boardX + 4, boardTop + 8 + PLANK_HEIGHT, boardW - 8, 4, Color::DarkGray);

  for (size_t i = 0; i < placed; ++i) {
    const bookshelf::Placement& p = placements[i];
    const Rect rect{area.x + MARGIN + p.x, standY - p.height, p.width, p.height};
    if (p.itemIndex < 0) {
      drawBookend(renderer, rect, bookend);
      continue;
    }
    const Book& book = books[static_cast<size_t>(p.itemIndex)];
    if (p.front) {
      drawFront(renderer, rect, book);
    } else {
      drawSpine(renderer, rect, book, p.style);
    }
    if (slotCount_ < slots_.size()) {
      slots_[slotCount_] = Slot{rect, &book};
      if (static_cast<int>(slotCount_) == selection_) {
        renderer.drawRect(rect.x - 4, rect.y - 4, rect.width + 8, rect.height + 8, 3, true);
      }
      ++slotCount_;
    }
  }
}

void BookshelfHome::drawSpine(const GfxRenderer& renderer, const Rect rect, const Book& book,
                              const bookshelf::SpineStyle style) const {
  // The head of the book: a strip of page edges, as seen from slightly above.
  constexpr int HEAD = 5;
  renderer.fillRect(rect.x + 1, rect.y, rect.width - 2, HEAD, false);
  for (int px = rect.x + 3; px < rect.x + rect.width - 3; px += 3) {
    renderer.drawLine(px, rect.y + 1, px, rect.y + HEAD - 1, true);
  }
  renderer.drawRect(rect.x + 1, rect.y, rect.width - 2, HEAD + 1, 1, true);
  const Rect body{rect.x, rect.y + HEAD, rect.width, rect.height - HEAD};

  // The cover cloth, with a highlight down the left and a shadow down the
  // right so the spine reads as rounded.
  bool whiteText = true;
  switch (style) {
    case bookshelf::SpineStyle::Black:
    case bookshelf::SpineStyle::Banded:
      renderer.fillRect(body.x, body.y, body.width, body.height, true);
      renderer.fillRectDither(body.x + 2, body.y + 2, 2, body.height - 4, Color::LightGray);
      break;
    case bookshelf::SpineStyle::LightGray:
      renderer.fillRect(body.x, body.y, body.width, body.height, false);
      renderer.fillRectDither(body.x, body.y, body.width, body.height, Color::DarkGray);
      renderer.fillRect(body.x + body.width - 3, body.y, 3, body.height, true);
      renderer.drawRect(body.x, body.y, body.width, body.height, 1, true);
      break;
    case bookshelf::SpineStyle::White:
      renderer.fillRect(body.x, body.y, body.width, body.height, false);
      renderer.drawRect(body.x, body.y, body.width, body.height, 1, true);
      renderer.fillRectDither(body.x + body.width - 4, body.y + 1, 3, body.height - 2, Color::LightGray);
      whiteText = false;
      break;
  }

  // Gilt bands near the top and bottom of the spine.
  const bool bands = style == bookshelf::SpineStyle::Banded || style == bookshelf::SpineStyle::White;
  if (bands) {
    for (const int by : {body.y + 6, body.y + 9, body.y + body.height - 10, body.y + body.height - 7}) {
      renderer.drawLine(body.x + 2, by, body.x + body.width - 3, by, !whiteText);
    }
  }

  // Status mark: a ribbon for a book in progress, a tick for a finished one.
  int topReserved = bands ? 14 : 6;
  const int markX = body.x + body.width / 2;
  if (book.completed) {
    const int m = std::min(body.width - 8, 12);
    const int y0 = body.y + topReserved;
    renderer.drawLine(markX - m / 2, y0 + m / 2, markX - m / 6, y0 + m - 2, 2, !whiteText);
    renderer.drawLine(markX - m / 6, y0 + m - 2, markX + m / 2, y0, 2, !whiteText);
    topReserved += m + 6;
  } else if (book.started) {
    // The ribbon hangs from the head of the book, in front of the spine.
    const int w = std::min(body.width - 10, 10);
    const int xs[5] = {markX - w / 2, markX + w / 2, markX + w / 2, markX, markX - w / 2};
    const int ys[5] = {rect.y, rect.y, rect.y + HEAD + 16, rect.y + HEAD + 11, rect.y + HEAD + 16};
    renderer.fillPolygon(xs, ys, 5, !whiteText);
    renderer.fillPolygon(xs, ys, 5, !whiteText);
    topReserved = std::max(topReserved, 22);
  }

  // Title (and author, when the spine is wide enough) read bottom-to-top.
  const int fontId = SMALL_FONT_ID;
  const int textHeight = renderer.getTextHeight(fontId);
  const int bottomReserved = bands ? 14 : 8;
  const int length = body.height - topReserved - bottomReserved;
  if (length < 20 || body.width < textHeight + 2) return;
  const bool twoLines = !book.author.empty() && body.width >= 2 * textHeight + 10;
  const int columns = twoLines ? 2 : 1;
  const int blockWidth = columns * textHeight + (columns - 1) * 2;
  int columnX = body.x + (body.width - blockWidth) / 2;
  if (style == bookshelf::SpineStyle::LightGray) {
    // A black label on the gray cloth keeps the lettering crisp.
    renderer.fillRect(columnX - 3, body.y + topReserved - 2, blockWidth + 6, length + 4, true);
  }
  const std::string title = renderer.truncatedText(fontId, book.title.c_str(), length, EpdFontFamily::BOLD);
  const int titleW = renderer.getTextWidth(fontId, title.c_str(), EpdFontFamily::BOLD);
  renderer.drawTextRotated90CW(fontId, columnX, body.y + topReserved + (length + titleW) / 2, title.c_str(),
                               !whiteText, EpdFontFamily::BOLD);
  if (twoLines) {
    columnX += textHeight + 2;
    const std::string author = renderer.truncatedText(fontId, book.author.c_str(), length);
    const int authorW = renderer.getTextWidth(fontId, author.c_str());
    renderer.drawTextRotated90CW(fontId, columnX, body.y + topReserved + (length + authorW) / 2, author.c_str(),
                                 !whiteText);
  }
}

void BookshelfHome::drawFront(const GfxRenderer& renderer, const Rect rect, const Book& book) const {
  // A book standing face-on: its cover, with the block of pages showing along
  // the right edge.
  constexpr int PAGES = 5;
  const Rect cover{rect.x, rect.y, rect.width - PAGES, rect.height};
  renderer.fillRect(rect.x + cover.width, rect.y + 2, PAGES, rect.height - 2, false);
  for (int py = rect.y + 4; py < rect.y + rect.height - 1; py += 3) {
    renderer.drawLine(rect.x + cover.width, py, rect.x + rect.width - 2, py, true);
  }
  renderer.drawRect(rect.x + cover.width - 1, rect.y + 2, PAGES + 1, rect.height - 2, 1, true);
  if (!drawThumb(renderer, book.frontThumbPath, cover)) {
    renderer.fillRect(cover.x, cover.y, cover.width, cover.height, false);
    int y = cover.y + 20;
    drawWrappedLines(renderer, SMALL_FONT_ID, cover.x + 6, y, cover.width - 12, book.title.c_str(), 5,
                     EpdFontFamily::BOLD);
  }
  renderer.drawRect(cover.x, cover.y, cover.width, cover.height, 2, true);
  if (book.completed) {
    renderer.fillRect(cover.x + 4, cover.y + 4, 18, 18, false);
    renderer.drawRect(cover.x + 4, cover.y + 4, 18, 18, 1, true);
    renderer.drawLine(cover.x + 7, cover.y + 13, cover.x + 11, cover.y + 18, 2, true);
    renderer.drawLine(cover.x + 11, cover.y + 18, cover.x + 19, cover.y + 7, 2, true);
  }
}

#if defined(CROSSINK_ENABLE_POKEMON)
namespace {
// The Pokemon cut out of an art image as a 1-bit mask (1 bit per pixel, so a
// whole Pokedex-card window costs ~8 KB).
struct SpriteMask {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bits;
  bool at(const int x, const int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    const size_t i = static_cast<size_t>(y) * width + x;
    return (bits[i >> 3] & (0x80 >> (i & 7))) != 0;
  }
};

// Reads the inked pixels of `path` inside the window [x0,x1)x[y0,y1), given
// as thousandths of the image size.
bool loadSpriteMask(const char* path, const int x0, const int y0, const int x1, const int y1, SpriteMask& out) {
  FsFile file;
  if (path == nullptr || !Storage.openFileForRead("SHELF", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() > 1024) {
    file.close();
    return false;
  }
  const int w = bitmap.getWidth();
  const int h = bitmap.getHeight();
  const int left = w * x0 / 1000;
  const int top = h * y0 / 1000;
  out.width = w * x1 / 1000 - left;
  out.height = h * y1 / 1000 - top;
  if (out.width <= 0 || out.height <= 0) {
    file.close();
    return false;
  }
  out.bits.assign((static_cast<size_t>(out.width) * out.height + 7) / 8, 0);
  std::vector<uint8_t> row(static_cast<size_t>((w + 3) / 4), 0);
  std::vector<uint8_t> raw(static_cast<size_t>(bitmap.getRowBytes()), 0);
  bool any = false;
  for (int r = 0; r < h; ++r) {
    if (bitmap.readNextRow(row.data(), raw.data()) != BmpReaderError::Ok) break;
    const int y = (bitmap.isTopDown() ? r : h - 1 - r) - top;
    if (y < 0 || y >= out.height) continue;
    for (int x = 0; x < out.width; ++x) {
      const int sx = left + x;
      if (((row[sx / 4] >> (6 - (sx % 4) * 2)) & 0x3) >= 3) continue;  // white
      const size_t i = static_cast<size_t>(y) * out.width + x;
      out.bits[i >> 3] |= 0x80 >> (i & 7);
      any = true;
    }
  }
  file.close();
  return any;
}
}  // namespace
#endif

bool BookshelfHome::Bookend::at(const int x, const int y) const {
  if (x < 0 || y < 0 || x >= width || y >= height) return false;
  const size_t i = static_cast<size_t>(y) * width + x;
  return (bits[i >> 3] & (0x80 >> (i & 7))) != 0;
}

void BookshelfHome::setBookend(const size_t shelf, const uint16_t speciesId) {
  if (shelf >= bookends_.size()) return;
  Bookend& bookend = bookends_[shelf];
  bookend = Bookend{};
  if (speciesId == 0) return;
#if defined(CROSSINK_ENABLE_POKEMON)
  // The picture is cut out of the Pokemon's Pokedex card (cleaner and larger
  // than the battle sprite; the window skips the card's header line, borders
  // and name), falling back to the battle sprite, then cropped to its shape.
  char path[64]{};
  SpriteMask mask;
  if (!loadSpriteMask(pokemon::pokemonPokedexArtPath(speciesId, false, path, sizeof(path)), 110, 145, 886, 387,
                      mask) &&
      !loadSpriteMask(pokemon::pokemonSpeciesArtPath(speciesId, true, path, sizeof(path)), 0, 0, 1000, 1000, mask)) {
    return;
  }
  int minX = mask.width, minY = mask.height, maxX = -1, maxY = -1;
  for (int y = 0; y < mask.height; ++y) {
    for (int x = 0; x < mask.width; ++x) {
      if (!mask.at(x, y)) continue;
      minX = std::min(minX, x);
      maxX = std::max(maxX, x);
      minY = std::min(minY, y);
      maxY = std::max(maxY, y);
    }
  }
  if (maxX < 0) return;
  bookend.width = maxX - minX + 1 + 2 * BOOKEND_PAD;
  bookend.height = maxY - minY + 1 + 2 * BOOKEND_PAD;
  bookend.bits.assign((static_cast<size_t>(bookend.width) * bookend.height + 7) / 8, 0);
  for (int y = 0; y < bookend.height; ++y) {
    for (int x = 0; x < bookend.width; ++x) {
      if (!mask.at(minX - BOOKEND_PAD + x, minY - BOOKEND_PAD + y)) continue;
      const size_t i = static_cast<size_t>(y) * bookend.width + x;
      bookend.bits[i >> 3] |= 0x80 >> (i & 7);
    }
  }
#else
  (void)speciesId;
#endif
}

void BookshelfHome::drawBookend(const GfxRenderer& renderer, const Rect rect, const Bookend& bookend) const {
  // The party leader sits on the board at its original size (scaled down only
  // if it is wider than BOOKEND_MAX_WIDTH), with its own pixels, a crisp black
  // outline on its filled silhouette (the inked share of each 3x3
  // neighbourhood, so dithered areas count as solid) and a white halo
  // following that shape, so it stands clear of the wallpaper (no box).
  if (bookend.width <= 0) return;
  auto darkness = [&](const int x, const int y) {
    int sum = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) sum += bookend.at(x + dx, y + dy) ? 1 : 0;
    }
    return sum;
  };
  auto solid = [&](const int x, const int y) { return darkness(x, y) >= 2; };
  auto nearSolid = [&](const int x, const int y) {
    for (int dy = -2; dy <= 2; ++dy) {
      for (int dx = -2; dx <= 2; ++dx) {
        if (dx * dx + dy * dy <= 5 && solid(x + dx, y + dy)) return true;
      }
    }
    return false;
  };

  const int64_t scale = std::min<int64_t>({int64_t{1} << 16, (int64_t{rect.width} << 16) / bookend.width,
                                           (int64_t{rect.height + BOOKEND_PAD} << 16) / bookend.height});
  if (scale <= 0) return;
  const int outW = static_cast<int>((int64_t{bookend.width} * scale) >> 16);
  const int outH = static_cast<int>((int64_t{bookend.height} * scale) >> 16);
  const int originX = rect.x + (rect.width - outW) / 2;
  const int originY = rect.y + rect.height + static_cast<int>((int64_t{BOOKEND_PAD} * scale) >> 16) - outH;
  // A soft shadow on the board under its feet.
  renderer.fillRoundedRect(originX + outW / 5, rect.y + rect.height - 3, outW * 3 / 5, 6, 3, Color::DarkGray);
  for (int oy = 0; oy < outH; ++oy) {
    const int y = static_cast<int>((int64_t{oy} << 16) / scale);
    for (int ox = 0; ox < outW; ++ox) {
      const int x = static_cast<int>((int64_t{ox} << 16) / scale);
      const bool inside = solid(x, y);
      if (!inside && !nearSolid(x, y)) continue;
      const bool edge = inside && (!solid(x - 1, y) || !solid(x + 1, y) || !solid(x, y - 1) || !solid(x, y + 1));
      // Inside: the card's own pixels.
      renderer.drawPixel(originX + ox, originY + oy, edge || (inside && bookend.at(x, y)));
    }
  }
}
