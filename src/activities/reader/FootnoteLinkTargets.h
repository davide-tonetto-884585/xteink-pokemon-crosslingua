#pragma once

#include <Epub/FootnoteEntry.h>
#include <Epub/PageFontSet.h>

#include <array>
#include <cstdint>
#include <vector>

class GfxRenderer;
class Page;

struct FootnoteLinkTarget {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

using FootnoteLinkTargets = std::array<FootnoteLinkTarget, EPUB_MAX_FOOTNOTES_PER_PAGE>;

// Match serialized link IDs to visible words without a heap allocation. Each line is measured in
// the font its role resolves to in `fonts` (Lingua lays some lines out in other fonts).
FootnoteLinkTargets buildFootnoteLinkTargets(const Page& page, const std::vector<FootnoteEntry>& footnotes,
                                             const GfxRenderer& renderer, const PageFontSet& fonts, int marginTop,
                                             int marginLeft);
