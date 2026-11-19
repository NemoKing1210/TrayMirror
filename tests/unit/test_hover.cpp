// Pure hit-testing for the overlay strip (spec 6.4): the cell list is
// strip-local, so a client point maps directly.
#include "render/Overlay.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using traymirror::HitTestIcon;

std::vector<RECT> Cells() {
  return {RECT{.left = 0, .top = 4, .right = 16, .bottom = 20},
          RECT{.left = 18, .top = 4, .right = 34, .bottom = 20}};
}

}  // namespace

TEST(HoverTest, HitInsideFirstCell) {
  size_t index = 99;
  EXPECT_TRUE(HitTestIcon(Cells(), POINT{.x = 8, .y = 12}, &index));
  EXPECT_EQ(index, 0U);
}

TEST(HoverTest, HitInsideSecondCell) {
  size_t index = 99;
  EXPECT_TRUE(HitTestIcon(Cells(), POINT{.x = 20, .y = 12}, &index));
  EXPECT_EQ(index, 1U);
}

TEST(HoverTest, HitGapIsNoHit) {
  size_t index = 99;
  EXPECT_FALSE(HitTestIcon(Cells(), POINT{.x = 17, .y = 12}, &index));
}

TEST(HoverTest, HitBeyondLastCellIsNoHit) {
  size_t index = 99;
  EXPECT_FALSE(HitTestIcon(Cells(), POINT{.x = 40, .y = 12}, &index));
}
