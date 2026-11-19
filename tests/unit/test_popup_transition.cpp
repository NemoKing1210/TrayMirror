// The flyout open/close state machine (plan task 6). Time is injected, so
// every phase, reversal and completion is deterministic; the real timer
// and layered alpha are covered by the integration tests.
#include "render/PopupTransition.h"

#include <windows.h>

#include <gtest/gtest.h>

namespace {

using traymirror::PopupCommand;
using traymirror::PopupPhase;
using traymirror::PopupTransition;

PopupTransition Animated(int durationMs = 160) {
  return PopupTransition(
      PopupTransition::Config{.animationsEnabled = true,
                              .durationMs = durationMs});
}

// Advances until the transition settles, returning the number of ticks.
int Settle(PopupTransition* transition, int maxTicks = 100) {
  int ticks = 0;
  while (transition->animating() && ticks < maxTicks) {
    transition->Advance(PopupTransition::kIntervalMs);
    ++ticks;
  }
  return ticks;
}

}  // namespace

TEST(PopupTransition, OpenStartsInvisibleAndShowsTheWindow) {
  PopupTransition transition = Animated();
  const auto step = transition.Open();
  EXPECT_EQ(step.command, PopupCommand::kShow);
  EXPECT_EQ(step.alpha, 0);
  EXPECT_EQ(transition.phase(), PopupPhase::kOpening);
  EXPECT_EQ(transition.alpha(), 0);
}

TEST(PopupTransition, OpenSettlesOpaqueWithoutAShowCommand) {
  PopupTransition transition = Animated();
  transition.Open();
  int ticks = 0;
  PopupCommand last = PopupCommand::kNone;
  int previous = transition.alpha();
  while (transition.phase() == PopupPhase::kOpening && ticks < 100) {
    const auto step = transition.Advance(PopupTransition::kIntervalMs);
    EXPECT_GE(step.alpha, previous);  // monotonic
    previous = step.alpha;
    last = step.command;
    ++ticks;
  }
  EXPECT_EQ(transition.phase(), PopupPhase::kOpen);
  EXPECT_EQ(transition.alpha(), 255);
  EXPECT_EQ(last, PopupCommand::kNone);
  // 160 ms at 16 ms per tick: completes on the tenth tick.
  EXPECT_EQ(ticks, 10);
}

TEST(PopupTransition, DisabledAnimationsOpenAndCloseImmediately) {
  PopupTransition transition(PopupTransition::Config{
      .animationsEnabled = false, .durationMs = 160});
  auto step = transition.Open();
  EXPECT_EQ(step.command, PopupCommand::kShow);
  EXPECT_EQ(step.alpha, 255);
  EXPECT_EQ(transition.phase(), PopupPhase::kOpen);

  step = transition.Close();
  EXPECT_EQ(step.command, PopupCommand::kDestroy);
  EXPECT_EQ(step.alpha, 0);
  EXPECT_EQ(transition.phase(), PopupPhase::kClosed);
}

TEST(PopupTransition, CloseFadesFromOpaqueAndDestroysAtZero) {
  PopupTransition transition = Animated();
  transition.Open();
  Settle(&transition);
  ASSERT_EQ(transition.phase(), PopupPhase::kOpen);

  auto step = transition.Close();
  EXPECT_EQ(step.command, PopupCommand::kNone);
  EXPECT_EQ(step.alpha, 255);
  EXPECT_EQ(transition.phase(), PopupPhase::kClosing);
  EXPECT_FALSE(transition.interactive());

  int previous = transition.alpha();
  step = transition.Advance(PopupTransition::kIntervalMs);
  EXPECT_LT(step.alpha, previous);
  EXPECT_EQ(step.command, PopupCommand::kNone);

  const int ticks = Settle(&transition);
  EXPECT_GT(ticks, 0);
  EXPECT_EQ(transition.phase(), PopupPhase::kClosed);
  EXPECT_EQ(transition.alpha(), 0);
}

TEST(PopupTransition, DestroyIsReportedExactlyOnce) {
  PopupTransition transition = Animated();
  transition.Open();
  Settle(&transition);
  transition.Close();

  int destroys = 0;
  while (transition.animating()) {
    if (transition.Advance(PopupTransition::kIntervalMs).command ==
        PopupCommand::kDestroy) {
      ++destroys;
    }
  }
  EXPECT_EQ(destroys, 1);
  // A closed transition never asks for another destroy.
  EXPECT_EQ(transition.Close().command, PopupCommand::kNone);
  EXPECT_EQ(transition.Advance(PopupTransition::kIntervalMs).command,
            PopupCommand::kNone);
}

TEST(PopupTransition, RepeatedCallsKeepTheDisplayedAlpha) {
  PopupTransition transition = Animated();
  transition.Open();
  for (int i = 0; i < 4; ++i) {
    transition.Advance(PopupTransition::kIntervalMs);
  }
  const int opening = transition.alpha();
  ASSERT_GT(opening, 0);

  // Asking to open again is a no-op, not a jump to transparent.
  const auto reopen = transition.Open();
  EXPECT_EQ(reopen.alpha, opening);
  EXPECT_EQ(reopen.command, PopupCommand::kNone);

  transition.Close();
  transition.Advance(PopupTransition::kIntervalMs);
  const int closing = transition.alpha();
  ASSERT_GT(closing, 0);

  // A second close (Esc pressed twice, Esc plus deactivation) must not
  // blank the popup for one timer tick before the fade resumes.
  const auto closeAgain = transition.Close();
  EXPECT_EQ(closeAgain.alpha, closing);
  EXPECT_EQ(closeAgain.command, PopupCommand::kNone);
  EXPECT_EQ(transition.alpha(), closing);
}

TEST(PopupTransition, CloseDuringOpeningReversesAndDestroys) {
  PopupTransition transition = Animated();
  transition.Open();
  for (int i = 0; i < 4; ++i) {
    transition.Advance(PopupTransition::kIntervalMs);
  }
  const int mid = transition.alpha();
  ASSERT_GT(mid, 0);
  ASSERT_LT(mid, 255);

  const auto step = transition.Close();
  EXPECT_EQ(step.alpha, mid);  // no jump
  EXPECT_EQ(transition.phase(), PopupPhase::kClosing);

  bool destroyed = false;
  while (transition.animating()) {
    destroyed = destroyed ||
                transition.Advance(PopupTransition::kIntervalMs).command ==
                    PopupCommand::kDestroy;
  }
  EXPECT_TRUE(destroyed);
  EXPECT_EQ(transition.phase(), PopupPhase::kClosed);
}

TEST(PopupTransition, ReopenDuringClosingStopsAndReturnsToOpen) {
  PopupTransition transition = Animated();
  transition.Open();
  Settle(&transition);
  transition.Close();
  for (int i = 0; i < 3; ++i) {
    transition.Advance(PopupTransition::kIntervalMs);
  }
  const int mid = transition.alpha();
  ASSERT_GT(mid, 0);
  ASSERT_LT(mid, 255);

  const auto step = transition.Open();
  EXPECT_EQ(step.alpha, mid);  // no flash to transparent
  EXPECT_EQ(step.command, PopupCommand::kNone);
  EXPECT_EQ(transition.phase(), PopupPhase::kOpening);
  EXPECT_TRUE(transition.interactive());

  for (int i = 0; i < 100 && transition.animating(); ++i) {
    EXPECT_NE(transition.Advance(PopupTransition::kIntervalMs).command,
              PopupCommand::kDestroy);
  }
  EXPECT_EQ(transition.phase(), PopupPhase::kOpen);
  EXPECT_EQ(transition.alpha(), 255);
}

TEST(PopupTransition, RepeatedOpenAndCloseAreIdempotent) {
  PopupTransition transition = Animated();
  EXPECT_EQ(transition.Open().command, PopupCommand::kShow);
  EXPECT_EQ(transition.Open().command, PopupCommand::kNone);
  EXPECT_EQ(transition.phase(), PopupPhase::kOpening);

  transition.Advance(PopupTransition::kIntervalMs);
  ASSERT_GT(transition.alpha(), 0);
  transition.Close();
  EXPECT_EQ(transition.Close().command, PopupCommand::kNone);
  EXPECT_EQ(transition.phase(), PopupPhase::kClosing);
}

TEST(PopupTransition, ClosingAnInvisibleOpeningPopupDestroysImmediately) {
  PopupTransition transition = Animated();
  transition.Open();
  // alpha is still zero: there is nothing to fade, so no timer is needed.
  const auto step = transition.Close();
  EXPECT_EQ(step.command, PopupCommand::kDestroy);
  EXPECT_EQ(transition.phase(), PopupPhase::kClosed);
}

TEST(PopupTransition, InteractivityFollowsTheDisplayedPhase) {
  PopupTransition transition = Animated();
  EXPECT_FALSE(transition.interactive());  // closed
  EXPECT_FALSE(transition.visible());

  transition.Open();
  EXPECT_TRUE(transition.interactive());  // opening is already a target
  EXPECT_TRUE(transition.visible());

  Settle(&transition);
  EXPECT_TRUE(transition.interactive());

  transition.Close();
  EXPECT_FALSE(transition.interactive());  // closing is no longer a target
  EXPECT_TRUE(transition.visible());
}

TEST(PopupTransition, ALateTickJumpsAheadInsteadOfStretching) {
  PopupTransition transition = Animated();
  transition.Open();
  const auto step = transition.Advance(5000);
  EXPECT_EQ(step.alpha, 255);
  EXPECT_EQ(transition.phase(), PopupPhase::kOpen);
}

TEST(PopupTransition, FinishImmediatelyClosesWithoutACommand) {
  PopupTransition transition = Animated();
  transition.Open();
  transition.Advance(48);
  transition.FinishImmediately();
  EXPECT_EQ(transition.phase(), PopupPhase::kClosed);
  EXPECT_EQ(transition.alpha(), 0);
  EXPECT_FALSE(transition.animating());
}

TEST(PopupTransition, SystemAnimationsFlagMatchesTheSystemSetting) {
  BOOL enabled = TRUE;
  ASSERT_NE(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0),
            FALSE);
  EXPECT_EQ(traymirror::SystemAnimationsEnabled(), enabled != FALSE);
}
