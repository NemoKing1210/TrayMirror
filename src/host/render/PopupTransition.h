// The overflow flyout's open/close transition (plan task 6).
//
// The acrylic material is a window-level DWM effect: DirectComposition
// visuals cannot include it, and AnimateWindow blocks the calling thread.
// The whole window is therefore faded with a layered-window constant
// alpha, which measured to fade the material and the content together
// (marker 255 -> 183 at alpha 128; material moved toward the backdrop).
// This class is the deterministic part: phases, reversal and the
// completion command. The caller drives it from a short timer, so no
// message handler ever blocks for the animation.
#pragma once

#include <cstdint>

namespace traymirror {

enum class PopupPhase : std::uint8_t {
  kClosed,
  kOpening,
  kOpen,
  kClosing,
};

enum class PopupCommand : std::uint8_t {
  kNone,
  kShow,     // the window must become visible now
  kDestroy,  // the fade reached zero: destroy the window and finish
};

class PopupTransition {
 public:
  // 167 ms is the WinUI control fast animation duration; the native
  // overflow motion was not measured (task 1 profile), so this is the
  // provisional value and the owner pass may retune it.
  static constexpr int kDefaultDurationMs = 167;
  static constexpr int kIntervalMs = 16;

  struct Config {
    bool animationsEnabled = true;
    int durationMs = kDefaultDurationMs;
  };

  struct Step {
    int alpha = 0;
    PopupCommand command = PopupCommand::kNone;
  };

  PopupTransition();
  explicit PopupTransition(Config config);

  // Idempotent for the same direction; a call against the current
  // direction reverses from the displayed alpha, so a rapid reopen can
  // never be destroyed by the fade that was already running.
  Step Open();
  Step Close();
  // Advances by `elapsedMs`; `elapsedMs` is the real time since the last
  // tick, so a late timer skips ahead instead of stretching the motion.
  Step Advance(int elapsedMs);
  // Closes without a transition (the owner is going away).
  void FinishImmediately();

  [[nodiscard]] PopupPhase phase() const noexcept { return phase_; }
  [[nodiscard]] int alpha() const noexcept { return alpha_; }
  // Input is ignored while closing: the window is still displayed but no
  // longer present a target to the user.
  [[nodiscard]] bool interactive() const noexcept {
    return phase_ == PopupPhase::kOpening || phase_ == PopupPhase::kOpen;
  }
  [[nodiscard]] bool visible() const noexcept {
    return phase_ != PopupPhase::kClosed;
  }
  [[nodiscard]] bool animating() const noexcept {
    return phase_ == PopupPhase::kOpening || phase_ == PopupPhase::kClosing;
  }

 private:
  // The elapsed time that produced `alpha`, so a reversal keeps the
  // displayed opacity continuous.
  [[nodiscard]] int ElapsedForAlpha(int alpha) const;

  Config config_;
  PopupPhase phase_ = PopupPhase::kClosed;
  int alpha_ = 0;
  int elapsedMs_ = 0;
};

// SPI_GETCLIENTAREAANIMATION: the "Animation effects" accessibility
// setting, which the shell's flyouts follow.
bool SystemAnimationsEnabled();

}  // namespace traymirror
