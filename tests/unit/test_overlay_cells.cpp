// Strip cell layout (spec 6.1/6.4): the icon cells and the overflow
// chevron's cell are pure geometry, so they are pinned here rather than
// through a rendered window.
#include "render/CellLayout.h"

#include <gtest/gtest.h>

namespace {

using traymirror::GridLayout;
using traymirror::HighlightBounds;
using traymirror::LayoutGrid;
using traymirror::LayoutStrip;
using traymirror::StripLayout;

constexpr int kIcon = 16;
constexpr int kGap = 8;  // padding on each side of the glyph
constexpr int kSlot = 32;
constexpr int kHeight = 48;

TEST(StripLayout, NoChevronWithoutOverflow) {
  const StripLayout layout =
      LayoutStrip(kSlot * 3, kHeight, kIcon, kGap, 3, false);
  ASSERT_EQ(layout.icons.size(), 3U);
  EXPECT_FALSE(layout.hasChevron);
  EXPECT_EQ(layout.icons[0].left, 0);
  EXPECT_EQ(layout.icons[0].right, kSlot);
  EXPECT_EQ(layout.icons[2].left, 2 * kSlot);
}

TEST(StripLayout, CellsAreVerticallyCentered) {
  const StripLayout layout = LayoutStrip(kSlot, kHeight, kIcon, kGap, 1, false);
  ASSERT_EQ(layout.icons.size(), 1U);
  EXPECT_EQ(layout.icons[0].top, (kHeight - kSlot) / 2);
  EXPECT_EQ(layout.icons[0].bottom, (kHeight - kSlot) / 2 + kSlot);
}

TEST(StripLayout, ChevronPrecedesTheIcons) {
  const StripLayout layout =
      LayoutStrip(kSlot * 6, kHeight, kIcon, kGap, 3, true);
  ASSERT_EQ(layout.icons.size(), 3U);
  ASSERT_TRUE(layout.hasChevron);
  EXPECT_EQ(layout.chevron.left, 0);
  EXPECT_EQ(layout.chevron.right, kSlot);
  EXPECT_EQ(layout.icons[0].left, kSlot);
  EXPECT_EQ(layout.chevron.top, layout.icons[0].top);
}

TEST(StripLayout, ChevronKeepsItsSlotWhenTheStripIsFull) {
  // Three slots, five icons: only two icons fit once the chevron has
  // reserved the third slot — without the chevron the other three would
  // be unreachable.
  const StripLayout layout =
      LayoutStrip(kSlot * 3, kHeight, kIcon, kGap, 5, true);
  ASSERT_TRUE(layout.hasChevron);
  ASSERT_EQ(layout.icons.size(), 2U);
  EXPECT_EQ(layout.chevron.left, 0);
  EXPECT_LE(layout.chevron.right, kSlot * 3);
}

TEST(StripLayout, NoChevronWhenItsSlotCannotFit) {
  const StripLayout layout =
      LayoutStrip(kIcon - 1, kHeight, kIcon, kGap, 1, true);
  EXPECT_TRUE(layout.icons.empty());
  EXPECT_FALSE(layout.hasChevron);
}

TEST(StripLayout, EmptyStripDrawsNothing) {
  const StripLayout layout = LayoutStrip(0, kHeight, kIcon, kGap, 3, true);
  EXPECT_TRUE(layout.icons.empty());
  EXPECT_FALSE(layout.hasChevron);
}

TEST(GridLayout, FullButtonsAreInsideTheWindow) {
  const GridLayout grid = LayoutGrid(kIcon, 2);
  ASSERT_EQ(grid.cells.size(), 2U);
  EXPECT_GT(grid.width, grid.cells[1].right);
  EXPECT_GT(grid.height, grid.cells[0].bottom);
  EXPECT_EQ(grid.cells[0].right - grid.cells[0].left, 40);
  EXPECT_EQ(grid.cells[0].right, grid.cells[1].left);  // adjacent full hit areas
}

TEST(GridLayout, FourColumnsThenANewRow) {
  const GridLayout grid = LayoutGrid(kIcon, 6);
  ASSERT_EQ(grid.cells.size(), 6U);
  EXPECT_EQ(grid.cells[3].top, grid.cells[0].top);   // still row 0
  EXPECT_EQ(grid.cells[4].left, grid.cells[0].left); // row 1, first column
  EXPECT_GT(grid.cells[4].top, grid.cells[0].top);
}

TEST(GridLayout, EmptyGridHasNoSize) {
  const GridLayout grid = LayoutGrid(kIcon, 0);
  EXPECT_TRUE(grid.cells.empty());
  EXPECT_EQ(grid.width, 0);
  EXPECT_EQ(grid.height, 0);
}

// The hit region is always the whole native button; the highlight is a
// separate visual inset, so the profile is not padded twice.
TEST(HighlightBounds, IsInsetFromTheFullNativeButton) {
  const RECT button{100, 100, 132, 148};  // 32 x 48 at 96 DPI
  const RECT highlight = HighlightBounds(button, 96);
  EXPECT_EQ(highlight.left, button.left + 2);
  EXPECT_EQ(highlight.right, button.right - 2);
  EXPECT_EQ(highlight.bottom - highlight.top, 40);
  EXPECT_EQ(highlight.top, button.top + 4);  // centred
  EXPECT_EQ(highlight.bottom, button.bottom - 4);
}

TEST(HighlightBounds, StaysInsideAShorterButton) {
  const RECT button{0, 0, 32, 32};
  const RECT highlight = HighlightBounds(button, 96);
  EXPECT_LE(highlight.top, highlight.bottom);
  EXPECT_GE(highlight.top, button.top);
  EXPECT_LE(highlight.bottom, button.bottom);
  EXPECT_EQ(highlight.bottom - highlight.top, 28);
  EXPECT_EQ(highlight.top, button.top + 2);
}

TEST(HighlightBounds, ScalesWithDpi) {
  const RECT button{0, 0, 48, 72};  // the 96 DPI button at 150 %
  const RECT highlight = HighlightBounds(button, 144);
  EXPECT_EQ(highlight.left, button.left + 3);
  EXPECT_EQ(highlight.right, button.right - 3);
  EXPECT_EQ(highlight.bottom - highlight.top, 60);
  EXPECT_EQ(highlight.top, button.top + 6);
}

TEST(HighlightBounds, DegenerateButtonStaysUsable) {
  const RECT button{0, 0, 2, 2};
  const RECT highlight = HighlightBounds(button, 96);
  EXPECT_LE(highlight.left, highlight.right);
  EXPECT_LE(highlight.top, highlight.bottom);
  EXPECT_GE(highlight.top, button.top);
  EXPECT_LE(highlight.bottom, button.bottom);
}

}  // namespace
