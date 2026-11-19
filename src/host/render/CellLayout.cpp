#include "render/CellLayout.h"

namespace traymirror {
namespace {

// Win11's overflow flyout shows four icons per row; more columns would
// grow the window wider than the strip it opens above.
constexpr std::size_t kFlyoutColumns = 4;

int FlyoutPadPx(int iconPx) { return iconPx / 4 > 3 ? iconPx / 4 : 3; }

}  // namespace

StripLayout LayoutStrip(int width, int height, int iconPx, int gap,
                        std::size_t iconCount, bool overflow) {
  StripLayout out;
  if (width <= 0 || height <= 0 || iconPx <= 0) {
    return out;
  }
  const int top = (height - iconPx) / 2;
  const int reserve = overflow ? iconPx + gap : 0;
  const int iconLimit = width - reserve;
  int x = 0;
  for (std::size_t i = 0; i < iconCount; ++i) {
    if (x + iconPx > iconLimit) {
      break;  // the tail clips; the chevron still shows the hidden ones
    }
    out.icons.push_back(
        RECT{.left = x, .top = top, .right = x + iconPx, .bottom = top + iconPx});
    x += iconPx + gap;
  }
  if (overflow && x + iconPx <= width) {
    out.chevron = RECT{
        .left = x, .top = top, .right = x + iconPx, .bottom = top + iconPx};
    out.hasChevron = true;
  }
  return out;
}

GridLayout LayoutGrid(int iconPx, std::size_t iconCount) {
  GridLayout out;
  if (iconPx <= 0 || iconCount == 0) {
    return out;
  }
  const int pad = FlyoutPadPx(iconPx);
  const int stride = iconPx + (2 * pad);
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
        RECT{.left = x, .top = y, .right = x + iconPx, .bottom = y + iconPx});
  }
  out.width = (static_cast<int>(columns) * stride) + pad;
  out.height = (static_cast<int>(rows) * stride) + pad;
  return out;
}

}  // namespace traymirror
