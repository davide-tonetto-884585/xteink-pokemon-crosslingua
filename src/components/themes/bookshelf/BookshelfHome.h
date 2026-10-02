#pragma once

#include <GfxRenderer.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "BookshelfLayout.h"
#include "components/themes/BaseTheme.h"

struct RecentBook;
struct BookReadingStats;
struct GlobalReadingStats;

// The "Bookshelf" home screen: the book being read at the top (cover, title,
// author, stats, progress), then two shelves - books already read on the top
// one, a handful of random unread books from the SD card on the bottom one -
// mostly standing spine-out, a few face-on, like a real bookcase.
//
// Holds only what it draws; HomeActivity owns input and navigation and asks
// this class what a tap or a selection points at.
class BookshelfHome {
 public:
  // Thumbnail sizes HomeActivity generates for the bookshelf (see loadRecentCovers()).
  static constexpr int HEADER_COVER_W = 120;
  static constexpr int HEADER_COVER_H = 180;
  static constexpr int FRONT_COVER_W = 96;
  static constexpr int FRONT_COVER_H = 144;

  struct Book {
    std::string path;
    std::string title;
    std::string author;
    std::string frontThumbPath;  // non-empty: shown face-on with this cover
    uint32_t key = 0;
    bool completed = false;
    bool started = false;
  };

  struct CurrentBook {
    const RecentBook* book = nullptr;  // nullptr = nothing open yet
    std::string coverThumbPath;
    const BookReadingStats* stats = nullptr;
    const GlobalReadingStats* globalStats = nullptr;  // for the reading streak
    float progressPercent = -1.0f;
  };

  // Books already read: every recent book except the one at the top.
  // `frontThumbs` maps recent-book paths that already have a face-on thumbnail.
  void loadReadShelf(const std::string& currentPath, const std::vector<std::pair<std::string, std::string>>& frontThumbs);
  // A random pick of books on the SD card that were never opened. The scan is
  // bounded (depth, entries) and its result kept for the session, so coming
  // back to Home does not rescan or reshuffle.
  void loadUnreadShelf(const std::string& currentPath);
  bool unreadLoaded() const { return unreadLoaded_; }
  // Unread EPUBs that could stand face-on, best first (HomeActivity generates
  // the cover thumbnail of the first one that opens).
  std::vector<std::string> unreadFrontCandidates() const;
  // Turns that book face-on with its cover, and gives it its real title/author.
  void setUnreadFront(const std::string& path, const std::string& thumbPath, const std::string& title,
                      const std::string& author);

  // Pokemon standing on the shelves: `shelf` 0 is the top one (the party's
  // first Pokemon), 1 the bottom one (the second). Loads and crops the picture
  // once (see BookshelfHome.cpp); speciesId 0 clears it.
  void setBookend(size_t shelf, uint16_t speciesId);

  // Draws everything below the status bar and above `bottom`.
  void render(const GfxRenderer& renderer, Rect area, const CurrentBook& current);

  // Selection over the shelf books, for button navigation: -1 = none.
  int selectableCount() const { return static_cast<int>(slotCount_); }
  void setSelection(int slot) { selection_ = slot; }
  int selection() const { return selection_; }
  // Shelf book under a tap, or -1. `headerHit` is set when the tap is on the
  // current book at the top.
  int hitTest(int x, int y, bool& headerHit) const;
  const std::string* pathForSlot(int slot) const;

 private:
  struct Slot {
    Rect rect;
    const Book* book = nullptr;
  };

  void drawWallpaper(const GfxRenderer& renderer, Rect area) const;
  // Height of the top panel: cover with stats beside it, then title, author
  // and the streak / reader-type line.
  int headerHeight(const GfxRenderer& renderer, Rect area, const CurrentBook& current) const;
  void drawHeader(const GfxRenderer& renderer, Rect area, const CurrentBook& current) const;
  // CrossInk's reading stats for the book (as on its Dashboard home), as
  // right-aligned value/label pairs spread between `top` and `bottom`.
  void drawStats(const GfxRenderer& renderer, int x, int top, int width, int bottom, const CurrentBook& current) const;
  // The Pokemon picture standing on a shelf, cut to its own shape plus a
  // margin for its halo, 1 bit per pixel. Its width sizes its shelf slot.
  struct Bookend {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bits;
    bool at(int x, int y) const;
  };

  void drawShelf(const GfxRenderer& renderer, Rect area, const std::vector<Book>& books, const Bookend& bookend,
                 bool bookendFirst);
  void drawSpine(const GfxRenderer& renderer, Rect rect, const Book& book, bookshelf::SpineStyle style) const;
  void drawFront(const GfxRenderer& renderer, Rect rect, const Book& book) const;
  void drawBookend(const GfxRenderer& renderer, Rect rect, const Bookend& bookend) const;

  std::vector<Book> read_;
  std::vector<Book> unread_;
  bool unreadLoaded_ = false;
  std::array<Bookend, 2> bookends_;

  static constexpr size_t MAX_SLOTS = 2 * bookshelf::MAX_PLACEMENTS;
  std::array<Slot, MAX_SLOTS> slots_;
  size_t slotCount_ = 0;
  Rect headerRect_;
  int selection_ = -1;
};
