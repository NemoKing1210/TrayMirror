// Cell geometry for the strip and the overflow flyout (spec 6.1/6.4).
// Pure arithmetic with no drawing: both layouts are unit-tested without
// a GPU, and the same numbers decide what is drawn and what is hit.
#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

namespace traymirror {

// The strip's laid-out cells, in strip-local pixels. A cell is the whole
// tray button — the icon plus `pad` on every side — because that is what
// Windows highlights and what the pointer must hit; the icon is centred
// inside it. The chevron, when present, precedes the icon buttons.
struct StripLayout {
  std::vector<RECT> icons;
  RECT chevron = {};
  bool hasChevron = false;
};

// `iconCount` cells and, when `overflow`, one chevron cell. The chevron
// reserves its cell first: a strip too narrow for everything still shows
// the chevron, because without it the icons it hides are unreachable.
[[nodiscard]] StripLayout LayoutStrip(int width, int height, int iconPx,
                                      int pad, std::size_t iconCount,
                                      bool overflow, SIZE button = {});

// The flyout grid: full 40 DIP hit areas, centered glyphs and an outer
// margin. Window size includes the margin on both sides.
struct GridLayout {
  std::vector<RECT> cells;  // client-local, one per icon
  int width = 0;
  int height = 0;
};

[[nodiscard]] GridLayout LayoutGrid(int iconPx, std::size_t iconCount);

// The visual hover highlight inside a full native button: a 2 DIP
// horizontal inset and a 40 DIP tall box centred vertically, both clamped
// to the button so a shorter row never draws outside its own cell. The
// hit region stays the whole button; this inset is the visual, applied
// once and never added to the hit geometry.
[[nodiscard]] RECT HighlightBounds(const RECT& button, int dpi);

}  // namespace traymirror
