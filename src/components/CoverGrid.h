#pragma once

#include <functional>
#include <string>

class GfxRenderer;
struct Rect;

// 3x3 cover-thumbnail grid used by the BookShelf browser (a fork feature; upstream has no grid
// component to host it). Geometry comes from CoverGridLayout.h, which BookShelfActivity also uses
// to size the thumbs it generates.
//
// Lives outside the theme classes on purpose: upstream owns BaseTheme/LyraTheme/RoundedRaffTheme
// and this grid is ours, so keeping it here leaves those files byte-identical to upstream and a
// future merge does not conflict over it. The two looks the themes used to express as a virtual
// override are now data (CoverGridStyle), resolved once from the active theme.
struct CoverGridStyle {
  // Selection fill: 0 paints a square black cell, > 0 a rounded light-gray one.
  int selectionRadius = 0;
  // Whether cell content (glyphs, title) flips to white on the selected cell. False keeps it black,
  // which is what a light-gray selection fill needs.
  bool invertSelectedContent = true;
};

// Style of the theme the user is currently on.
CoverGridStyle activeCoverGridStyle();

// Paints the whole page; pass selectedIndex = -1 for a clean, selection-free buffer.
// isPending(i) marks a cover-bearing entry whose thumbnail is still being generated: the cell draws
// a loading placeholder instead of the blank of a processed, cover-less book.
// Issues no display refresh.
void drawCoverGrid(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex, int pageOffset,
                   const std::function<std::string(int)>& getTitle, const std::function<std::string(int)>& getThumbPath,
                   const std::function<bool(int)>& isDirectory, const std::function<bool(int)>& isPending);

// Repaints only the single selected cell over an already-painted grid. Issues no display refresh.
void drawCoverGridSelection(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex, int pageOffset,
                            const std::function<std::string(int)>& getTitle,
                            const std::function<std::string(int)>& getThumbPath,
                            const std::function<bool(int)>& isDirectory, const std::function<bool(int)>& isPending);
