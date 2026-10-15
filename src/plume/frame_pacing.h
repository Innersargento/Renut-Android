#pragma once
// The frame rate the player asked for, and the clock that holds the game to it.
//
// One target, read live by every part that paces or presents:
//   30        the game's own rate. Its presentation interval holds it to every
//             second vertical blank, and the vblank clock (plume_gpu.cpp) is what
//             makes that steady.
//   31..240   the game presents immediately (no wait for the vblank) and the
//             command thread holds it to the target with a Pacer.
//   0         unlimited: immediate presentation, no pacer and, so that the
//             display's refresh rate is not a ceiling either, a swapchain that
//             does not queue (Mailbox, or Immediate): see PresentVsync().
//
// The target comes from the renut_fps_target cvar at start and from the sidebar
// while playing (nativeSetFpsTarget), and is only ever read through here.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace renut::pacing {

inline constexpr int kOriginalFps = 30;
inline constexpr int kMinFps = 30;
inline constexpr int kMaxFps = 240;
inline constexpr int kUnlimited = 0;
// Up to here the swapchain keeps vertical sync: every display refreshes at least
// this fast, so it is no ceiling, and what is shown never tears.
inline constexpr int kVsyncCeilingFps = 60;

inline std::atomic<int>& TargetCell() {
  static std::atomic<int> target{kOriginalFps};
  return target;
}

// Anything at or below zero is unlimited; the rest stays inside the slider.
inline int Sanitize(int fps) {
  if (fps <= 0) return kUnlimited;
  return std::clamp(fps, kMinFps, kMaxFps);
}

inline int Target() { return TargetCell().load(std::memory_order_relaxed); }
inline void SetTarget(int fps) { TargetCell().store(Sanitize(fps), std::memory_order_relaxed); }

// Takes the target from the renut_fps_target cvar (game_fixes.cpp), once the
// configuration has been loaded.
void ApplyConfiguredTarget();

// Whether the interval the game has set is the immediate one (index 0): its own
// choice, or the hook's (game_fixes.cpp rex_setParamInterval) once the target asked
// for it. Then nothing waits for the vblank until the game selects another.
inline std::atomic<bool>& GameIntervalCell() {
  static std::atomic<bool> immediate{false};
  return immediate;
}
inline void NoteGameInterval(uint32_t index) {
  GameIntervalCell().store(index == 0, std::memory_order_relaxed);
}

// The target asks for the game's swap not to wait for the vblank.
inline bool ImmediatePresentation() { return Target() != kOriginalFps; }
// The vblank is what holds the game: its own 30 FPS target and an interval that
// waits for it. Otherwise the command thread's Pacer holds the target.
inline bool VblankPaces() {
  return Target() == kOriginalFps && !GameIntervalCell().load(std::memory_order_relaxed);
}
// The swapchain presents on the display's vertical sync.
inline bool PresentVsync() {
  const int fps = Target();
  return fps != kUnlimited && fps <= kVsyncCeilingFps;
}

using Clock = std::chrono::steady_clock;

// How much of a wait is spun instead of slept: the wake-up of a sleep has its own
// jitter (the timer slack, the scheduler) that must not reach the frame.
#if defined(_WIN32)
// Sleeps there are only as fine as the system timer.
inline constexpr std::chrono::microseconds kSpinTail{2000};
#else
inline constexpr std::chrono::microseconds kSpinTail{300};
#endif

// Blocks until `deadline`. The wait itself costs no CPU: a sleep to just short of
// the deadline, then a spin for the last of it. Never returns early.
inline void SleepUntil(Clock::time_point deadline) {
  const auto now = Clock::now();
  if (deadline - now > kSpinTail) std::this_thread::sleep_until(deadline - kSpinTail);
  while (Clock::now() < deadline) std::this_thread::yield();
}

// Holds one thread to a frame rate. Deadlines are absolute and advance by exactly
// one period, so a frame that runs long is paid back by the next one instead of
// the error piling up (a plain sleep(period) drifts); but a hitch of more than a
// whole frame is forgotten, not caught up with a run of fast frames.
class Pacer {
 public:
  // Call once per frame, after the frame was handed over. fps <= 0 does nothing.
  void Wait(int fps) {
    if (fps <= 0) {
      armed_ = false;
      return;
    }
    const auto now = Clock::now();
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / fps);
    if (!armed_ || fps != fps_) {
      armed_ = true;
      fps_ = fps;
      next_ = now;  // the frame that just ended is on time
    }
    next_ += period;
    if (now >= next_) {
      if (now - next_ > period) next_ = now;
      return;
    }
    SleepUntil(next_);
  }

 private:
  bool armed_ = false;
  int fps_ = 0;
  Clock::time_point next_{};
};

// The steady rate of a clock that ticks `hz` times a second and may be late: the
// next deadline after `deadline`, which is `now` plus one period when it fell more
// than two periods behind (so a stall costs one late tick, never a burst).
inline Clock::time_point NextTick(Clock::time_point deadline, Clock::time_point now,
                                  std::chrono::nanoseconds period) {
  deadline += period;
  if (now - deadline > period * 2) deadline = now + period;
  return deadline;
}

}  // namespace renut::pacing
