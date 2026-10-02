#include <array>
#include <cstdio>
#include <string>

#include "components/themes/bookshelf/BookshelfLayout.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      std::fprintf(stderr, "%s:%d check failed: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                                       \
    }                                                                                   \
  } while (false)

namespace bs = bookshelf;

std::array<bs::ShelfItem, 30> manyBooks() {
  std::array<bs::ShelfItem, 30> items{};
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string path = "/Books/book-" + std::to_string(i) + ".epub";
    items[i].key = bs::keyForPath(path.c_str());
    items[i].titleLength = static_cast<uint16_t>(5 + i * 3);
  }
  return items;
}

void titlesComeFromFileNames() {
  CHECK(bs::titleFromFileName("/Books/Il_nome_della_rosa.epub") == "Il nome della rosa");
  CHECK(bs::titleFromFileName("Moby Dick.epub") == "Moby Dick");
  CHECK(bs::titleFromFileName("/a/b/no-extension") == "no-extension");
  CHECK(bs::titleFromFileName("/a/.hidden") == ".hidden");
  CHECK(bs::titleFromFileName(nullptr).empty());
}

void spinesStayInRangeAndAreStable() {
  const auto items = manyBooks();
  for (const auto& item : items) {
    const int width = bs::spineWidthFor(item);
    CHECK(width >= bs::SPINE_MIN_WIDTH && width <= bs::SPINE_MAX_WIDTH);
    const int height = bs::spineHeightFor(item, 200);
    CHECK(height >= 156 && height <= 200);
    CHECK(bs::spineWidthFor(item) == width);  // same book, same look
  }
  CHECK(bs::keyForPath("/a.epub") == bs::keyForPath("/a.epub"));
  CHECK(bs::keyForPath("/a.epub") != bs::keyForPath("/b.epub"));
}

void shelfFillsWithoutOverflowing() {
  const auto items = manyBooks();
  bs::ShelfGeometry geometry;
  geometry.width = 448;
  geometry.maxHeight = 180;
  std::array<bs::Placement, bs::MAX_PLACEMENTS> out{};
  const size_t placed = bs::layoutShelf(items.data(), items.size(), geometry, out.data(), out.size());
  CHECK(placed > 5 && placed < items.size());  // full, but not everything fits
  for (size_t i = 0; i < placed; ++i) {
    CHECK(out[i].x >= 0 && out[i].x + out[i].width <= geometry.width);
    if (i > 0) CHECK(out[i].x >= out[i - 1].x + out[i - 1].width);  // no overlap
    CHECK(out[i].itemIndex == static_cast<int>(i));                    // order kept
  }
}

void frontCoverAndBookendGetTheirRoom() {
  auto items = manyBooks();
  items[2].front = true;
  bs::ShelfGeometry geometry;
  geometry.width = 448;
  geometry.maxHeight = 180;
  geometry.frontWidth = 120;
  geometry.bookendWidth = 100;
  std::array<bs::Placement, bs::MAX_PLACEMENTS> out{};
  const size_t placed = bs::layoutShelf(items.data(), items.size(), geometry, out.data(), out.size());
  CHECK(placed >= 4);
  CHECK(out[2].front && out[2].width == 120 && out[2].height == 180);
  CHECK(out[2].x >= out[1].x + out[1].width + bs::FRONT_GAP);
  const bs::Placement& bookend = out[placed - 1];
  CHECK(bookend.itemIndex == -1 && bookend.width == 100);
  CHECK(bookend.x + bookend.width <= geometry.width);
  CHECK(bookend.x >= out[placed - 2].x + out[placed - 2].width);

  geometry.bookendFirst = true;
  const size_t placedFirst = bs::layoutShelf(items.data(), items.size(), geometry, out.data(), out.size());
  CHECK(placedFirst >= 2 && out[0].itemIndex == -1 && out[0].x == 0);
  CHECK(out[1].x >= 100 + bs::FRONT_GAP);
}

void degenerateInputsAreSafe() {
  const auto items = manyBooks();
  std::array<bs::Placement, bs::MAX_PLACEMENTS> out{};
  bs::ShelfGeometry geometry;
  CHECK(bs::layoutShelf(items.data(), items.size(), geometry, out.data(), out.size()) == 0);
  geometry.width = 10;
  geometry.maxHeight = 100;
  CHECK(bs::layoutShelf(items.data(), items.size(), geometry, out.data(), out.size()) == 0);
  geometry.width = 448;
  CHECK(bs::layoutShelf(items.data(), 0, geometry, out.data(), out.size()) == 0);
  CHECK(bs::layoutShelf(items.data(), items.size(), geometry, out.data(), 3) == 3);
}

}  // namespace

int main() {
  titlesComeFromFileNames();
  spinesStayInRangeAndAreStable();
  shelfFillsWithoutOverflowing();
  frontCoverAndBookendGetTheirRoom();
  degenerateInputsAreSafe();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
