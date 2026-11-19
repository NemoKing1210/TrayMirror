// Strip cell layout (spec 6.1/6.4): the icon cells and the overflow
// chevron's cell are pure geometry, so they are pinned here rather than
// through a rendered window.
#include "render/CellLayout.h"

#include <gtest/gtest.h>

namespace {

using traymirror::GridLayout;
using traymirror::LayoutGrid;
using traymirror::LayoutStrip;
using traymirror::StripLayout;

constexpr int kIcon = 16;
constexpr int kGap = 2;
constexpr int kSlot = kIcon + kGap;
constexpr int kHeight = 48;

TEST(StripLayout, NoChevronWithoutOverflow) {
  const StripLayout layout =
      LayoutStrip(kSlot * 3, kHeight, kIcon, kGap, 3, false);
  ASSERT_EQ(layout.icons.size(), 3U);
  EXPECT_FALSE(layout.hasChevron);
  EXPECT_EQ(layout.icons[0].left, 0);
  EXPECT_EQ(layout.icons[0].right, kIcon);
  EXPECT_EQ(layout.icons[2].left, 2 * kSlot);
}

TEST(StripLayout, CellsAreVerticallyCentered) {
  const StripLayout layout = LayoutStrip(kSlot, kHeight, kIcon, kGap, 1, false);
  ASSERT_EQ(layout.icons.size(), 1U);
  EXPECT_EQ(layout.icons[0].top, (kHeight - kIcon) / 2);
  EXPECT_EQ(layout.icons[0].bottom, (kHeight - kIcon) / 2 + kIcon);
}

TEST(StripLayout, ChevronFollowsTheLastIcon) {
  const StripLayout layout =
      LayoutStrip(kSlot * 6, kHeight, kIcon, kGap, 3, true);
  ASSERT_EQ(layout.icons.size(), 3U);
  ASSERT_TRUE(layout.hasChevron);
  EXPECT_EQ(layout.chevron.left, 3 * kSlot);
  EXPECT_EQ(layout.chevron.right, 3 * kSlot + kIcon);
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
  EXPECT_EQ(layout.chevron.left, 2 * kSlot);
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

TEST(GridLayout, CellsAreIconSizedAndInsideTheWindow) {
  const GridLayout grid = LayoutGrid(kIcon, 2);
  ASSERT_EQ(grid.cells.size(), 2U);
  EXPECT_GT(grid.width, grid.cells[1].right);
  EXPECT_GT(grid.height, grid.cells[0].bottom);
  EXPECT_EQ(grid.cells[0].right - grid.cells[0].left, kIcon);
  EXPECT_LT(grid.cells[0].right, grid.cells[1].left);  // left to right
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

}  // namespace
