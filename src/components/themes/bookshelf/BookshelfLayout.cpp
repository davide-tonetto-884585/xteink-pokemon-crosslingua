#include "BookshelfLayout.h"

#include <algorithm>
#include <cstring>

namespace bookshelf {

uint32_t keyForPath(const char* path) {
  uint32_t hash = 2166136261U;
  for (const char* c = path; c != nullptr && *c != '\0'; ++c) {
    hash ^= static_cast<uint8_t>(*c);
    hash *= 16777619U;
  }
  return hash;
}

std::string titleFromFileName(const char* path) {
  if (path == nullptr) return {};
  const char* slash = std::strrchr(path, '/');
  std::string name = slash == nullptr ? path : slash + 1;
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.resize(dot);
  std::replace(name.begin(), name.end(), '_', ' ');
  return name;
}

int spineWidthFor(const ShelfItem& item) {
  // A little thicker for long titles, plus a stable per-book jitter.
  const int width = SPINE_MIN_WIDTH + static_cast<int>(item.key % 5U) * 4 + std::min<int>(item.titleLength, 40) / 4;
  return std::clamp(width, SPINE_MIN_WIDTH, SPINE_MAX_WIDTH);
}

int spineHeightFor(const ShelfItem& item, const int maxHeight) {
  // 78-97% of the shelf, so the row has the uneven skyline of a real shelf.
  const int percent = 78 + static_cast<int>((item.key >> 8) % 20U);
  return maxHeight * percent / 100;
}

SpineStyle spineStyleFor(const ShelfItem& item) {
  return static_cast<SpineStyle>((item.key >> 16) % SPINE_STYLE_COUNT);
}

size_t layoutShelf(const ShelfItem* items, const size_t count, const ShelfGeometry& geometry, Placement* out,
                   const size_t capacity) {
  if (out == nullptr || capacity == 0 || geometry.width <= 0 || geometry.maxHeight <= 0) return 0;
  size_t placed = 0;
  int cursor = 0;
  // Room kept free for a bookend that goes after the books.
  const int reserved = geometry.bookendWidth > 0 && !geometry.bookendFirst ? geometry.bookendWidth + FRONT_GAP : 0;

  if (geometry.bookendWidth > 0 && geometry.bookendFirst && geometry.bookendWidth <= geometry.width) {
    out[placed++] = Placement{-1, 0, geometry.bookendWidth, geometry.maxHeight, SpineStyle::Black, false};
    cursor = geometry.bookendWidth + FRONT_GAP;
  }

  for (size_t i = 0; i < count && placed < capacity; ++i) {
    const ShelfItem& item = items[i];
    const bool front = item.front && geometry.frontWidth > 0;
    const int width = front ? geometry.frontWidth : spineWidthFor(item);
    const int before = placed > 0 && (front || out[placed - 1].front) ? FRONT_GAP : (placed > 0 ? SPINE_GAP : 0);
    if (cursor + before + width > geometry.width - reserved) break;
    cursor += before;
    Placement placement;
    placement.itemIndex = static_cast<int>(i);
    placement.x = cursor;
    placement.width = width;
    // A face-on cover keeps a book's 2:3 shape rather than stretching to the shelf height.
    placement.height =
        front ? std::min(geometry.maxHeight, geometry.frontWidth * 3 / 2) : spineHeightFor(item, geometry.maxHeight);
    placement.style = spineStyleFor(item);
    placement.front = front;
    out[placed++] = placement;
    cursor += width;
  }

  if (reserved > 0 && placed < capacity) {
    // The bookend sits right after the last book, not at the far wall.
    const int x = cursor + (placed > 0 ? FRONT_GAP : 0);
    if (x + geometry.bookendWidth <= geometry.width) {
      out[placed++] = Placement{-1, x, geometry.bookendWidth, geometry.maxHeight, SpineStyle::Black, false};
    }
  }
  return placed;
}

}  // namespace bookshelf
