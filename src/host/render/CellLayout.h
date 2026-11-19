// Cell geometry for the strip and the overflow flyout (spec 6.1/6.4).
// Pure arithmetic with no drawing: both layouts are unit-tested without
// a GPU, and the same numbers decide what is drawn and what is hit.
#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

namespace traymirror {

// The strip's laid-out cells, in strip-local pixels. `icons` runs left
// to right from the strip's left edge; the chevron, when present,
// follows the last icon so it stays next to the icons it reveals.
struct StripLayout {
  std::vector<RECT> icons;
  RECT chevron = {};
  bool hasChevron = false;
};

// `iconCount` icons and, when `overflow`, one chevron cell. The chevron
// reserves its slot first: a strip too narrow for everything still shows
// the chevron, because without it the icons it hides are unreachable.
[[nodiscard]] StripLayout LayoutStrip(int width, int height, int iconPx,
                                      int gap, std::size_t iconCount,
                                      bool overflow);

// The flyout grid (spec 6.4): icon-sized cells in up to four columns,
// each surrounded by padding, and the window size that holds them.
struct GridLayout {
  std::vector<RECT> cells;  // client-local, one per icon
  int width = 0;
  int height = 0;
};

[[nodiscard]] GridLayout LayoutGrid(int iconPx, std::size_t iconCount);

}  // namespace traymirror
