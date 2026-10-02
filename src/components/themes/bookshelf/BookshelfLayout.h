#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure geometry for the Bookshelf home: how the books on one shelf are laid
// out (spine widths and heights, which ones face front, where the bookend
// goes). No renderer or storage here, so the host tests cover it.
namespace bookshelf {

// Stable per-book number (FNV-1a of its path): the same book always gets the
// same spine width, height and style, so the shelf does not reshuffle its
// look between redraws.
uint32_t keyForPath(const char* path);

// "Il_nome_della_rosa.epub" / "/Books/x/Il nome.epub" -> "Il nome della rosa".
std::string titleFromFileName(const char* path);

enum class SpineStyle : uint8_t {
  Black,      // black spine, white lettering
  White,      // outlined white spine, black lettering
  LightGray,  // light-gray (dithered) spine, black lettering
  Banded,     // black spine with two white bands, white lettering
};
constexpr uint8_t SPINE_STYLE_COUNT = 4;

struct ShelfItem {
  uint32_t key = 0;
  uint16_t titleLength = 0;  // longer titles get slightly thicker spines
  bool front = false;        // shown face-on with its cover instead of its spine
};

struct Placement {
  int itemIndex = -1;  // index into the ShelfItem array; -1 for the bookend
  int x = 0;           // relative to the shelf's left edge
  int width = 0;
  int height = 0;  // the book stands on the shelf, so it rises this far above it
  SpineStyle style = SpineStyle::Black;
  bool front = false;
};

struct ShelfGeometry {
  int width = 0;        // usable shelf width
  int maxHeight = 0;    // tallest a book may be
  int frontWidth = 0;   // width of a face-on cover
  int bookendWidth = 0; // 0 = no bookend on this shelf
  bool bookendFirst = false;
};

constexpr int SPINE_MIN_WIDTH = 22;
constexpr int SPINE_MAX_WIDTH = 50;
constexpr int SPINE_GAP = 2;
constexpr int FRONT_GAP = 8;
constexpr size_t MAX_PLACEMENTS = 40;

int spineWidthFor(const ShelfItem& item);
int spineHeightFor(const ShelfItem& item, int maxHeight);
SpineStyle spineStyleFor(const ShelfItem& item);

// Lays out items left to right (bookend first or last when given), stopping at
// the first one that no longer fits. Returns how many placements were written.
size_t layoutShelf(const ShelfItem* items, size_t count, const ShelfGeometry& geometry, Placement* out,
                   size_t capacity);

}  // namespace bookshelf
