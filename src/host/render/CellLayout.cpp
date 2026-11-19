#include "render/CellLayout.h"

#include <algorithm>

namespace traymirror {
namespace {

// Win11's overflow flyout shows four icons per row; more columns would
// grow the window wider than the strip it opens above.
constexpr std::size_t kFlyoutColumns = 4;

int FlyoutPadPx(int iconPx) { return MulDiv(8, iconPx, 16); }

}  // namespace

StripLayout LayoutStrip(int width, int height, int iconPx, int pad,
                        std::size_t iconCount, bool overflow, SIZE button) {
  StripLayout out;
  if (width <= 0 || height <= 0 || iconPx <= 0 || pad < 0) {
    return out;
  }
  // The tray button is the icon plus equal padding on every side, and it
  // is centred vertically in the taskbar row (Win11: a 32 px button in a
  // 48 px taskbar at 100 %).
  const int slot = button.cx > 0 ? button.cx : iconPx + (2 * pad);
  const int cellHeight = button.cy > 0 ? button.cy : iconPx + (2 * pad);
  const int top = (height - cellHeight) / 2;
  int x = 0;
  if (overflow && slot <= width) {
    out.chevron = RECT{.left = 0, .top = top, .right = slot,
                       .bottom = top + cellHeight};
    out.hasChevron = true;
    x = slot;
  }
  for (std::size_t i = 0; i < iconCount; ++i) {
    if (x + slot > width) {
      break;  // the tail clips; the chevron still shows the hidden ones
    }
    out.icons.push_back(
        RECT{.left = x, .top = top, .right = x + slot, .bottom = top + cellHeight});
    x += slot;
  }
  return out;
}

GridLayout LayoutGrid(int iconPx, std::size_t iconCount) {
  GridLayout out;
  if (iconPx <= 0 || iconCount == 0) {
    return out;
  }
  const int pad = FlyoutPadPx(iconPx);
  const int stride = MulDiv(40, iconPx, 16);
  const std::size_t columns =
      kFlyoutColumns < iconCount ? kFlyoutColumns : iconCount;
  const auto rows = (iconCount + columns - 1) / columns;
  out.cells.reserve(iconCount);
  for (std::size_t i = 0; i < iconCount; ++i) {
    const int col = static_cast<int>(i % columns);
    const int row = static_cast<int>(i / columns);
    const int x = pad + (col * stride);
    const int y = pad + (row * stride);
    out.cells.push_back(
        RECT{.left = x, .top = y, .right = x + stride, .bottom = y + stride});
  }
  out.width = (static_cast<int>(columns) * stride) + (2 * pad);
  out.height = (static_cast<int>(rows) * stride) + (2 * pad);
  return out;
}

RECT HighlightBounds(const RECT& button, int dpi) {
  if (dpi <= 0) {
    return button;
  }
  const int inset = MulDiv(2, dpi, 96);
  const LONG maxHeight = MulDiv(40, dpi, 96);
  const LONG width = (std::max)(0L, (button.right - button.left) - (2L * inset));
  const LONG available =
      (std::max)(0L, (button.bottom - button.top) - (2L * inset));
  const LONG height = (std::min)(maxHeight, available);
  const LONG top =
      button.top + (((button.bottom - button.top) - height) / 2);
  const LONG left = button.left + inset;
  return RECT{.left = left, .top = top, .right = left + width,
              .bottom = top + height};
}

}  // namespace traymirror
