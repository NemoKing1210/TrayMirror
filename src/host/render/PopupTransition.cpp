#include "render/PopupTransition.h"

#include <windows.h>

#include <algorithm>

namespace traymirror {

PopupTransition::PopupTransition() : PopupTransition(Config{}) {}

PopupTransition::PopupTransition(Config config) : config_(config) {
  if (config_.durationMs < 0) {
    config_.durationMs = 0;
  }
}

int PopupTransition::ElapsedForAlpha(int alpha) const {
  if (config_.durationMs <= 0) {
    return 0;
  }
  const int clamped = std::clamp(alpha, 0, 255);
  return clamped * config_.durationMs / 255;
}

PopupTransition::Step PopupTransition::Open() {
  Step step;
  if (phase_ == PopupPhase::kOpen || phase_ == PopupPhase::kOpening) {
    // Already opening or open: keep the displayed opacity. Returning a
    // default step here would blank a visible popup for one tick.
    step.alpha = alpha_;
    return step;
  }
  if (phase_ == PopupPhase::kClosing) {
    // Reverse from the displayed opacity; the window is already shown.
    phase_ = PopupPhase::kOpening;
    elapsedMs_ = ElapsedForAlpha(alpha_);
    step.alpha = alpha_;
    return step;
  }

  // Closed: the caller must show the window.
  phase_ = PopupPhase::kOpening;
  alpha_ = 0;
  elapsedMs_ = 0;
  if (!config_.animationsEnabled || config_.durationMs <= 0) {
    phase_ = PopupPhase::kOpen;
    alpha_ = 255;
  }
  step.alpha = alpha_;
  step.command = PopupCommand::kShow;
  return step;
}

PopupTransition::Step PopupTransition::Close() {
  Step step;
  if (phase_ == PopupPhase::kClosed || phase_ == PopupPhase::kClosing) {
    // Closing is idempotent: a second Close (Esc twice, Esc plus an
    // outside click) must not jump the displayed alpha to zero.
    step.alpha = alpha_;
    return step;
  }
  if (!config_.animationsEnabled || config_.durationMs <= 0 || alpha_ <= 0) {
    phase_ = PopupPhase::kClosed;
    alpha_ = 0;
    elapsedMs_ = 0;
    step.alpha = 0;
    step.command = PopupCommand::kDestroy;
    return step;
  }
  phase_ = PopupPhase::kClosing;
  elapsedMs_ = ElapsedForAlpha(255 - alpha_);
  step.alpha = alpha_;
  return step;
}

PopupTransition::Step PopupTransition::Advance(int elapsedMs) {
  Step step = {.alpha = alpha_, .command = PopupCommand::kNone};
  if (!animating()) {
    return step;
  }
  elapsedMs_ += (std::max)(0, elapsedMs);
  const int progress = (std::min)(
      255, static_cast<int>(static_cast<long long>(elapsedMs_) * 255 /
                            config_.durationMs));
  if (phase_ == PopupPhase::kOpening) {
    alpha_ = progress;
    if (alpha_ >= 255) {
      alpha_ = 255;
      phase_ = PopupPhase::kOpen;
    }
  } else {
    alpha_ = 255 - progress;
    if (alpha_ <= 0) {
      alpha_ = 0;
      phase_ = PopupPhase::kClosed;
      step.command = PopupCommand::kDestroy;
    }
  }
  step.alpha = alpha_;
  return step;
}

void PopupTransition::FinishImmediately() {
  phase_ = PopupPhase::kClosed;
  alpha_ = 0;
  elapsedMs_ = 0;
}

bool SystemAnimationsEnabled() {
  BOOL enabled = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) ==
      FALSE) {
    return true;
  }
  return enabled != FALSE;
}

}  // namespace traymirror
