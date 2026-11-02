// Fence queries and waits, natively. Ported from reNut's hooks/device.cpp
// (FencePending, IsFencePending_hook, BlockOnFence_hook; masterspike52's reNut) and
// adapted to this app's sync: the fence is completed by the GPU backend when it
// runs the list that carries it (d3d/frontend/sync.cpp, kOpFence), not by a render
// thread, so the wait polls the completion word the backend writes.
#include "d3d/sync/fence.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/logging.h>

#include "d3d/device/internal.h"

namespace renut::d3d::port {

namespace {

// CDevice-private fields, all outside the range Layout() moves.
constexpr u32 kFenceCompletion = 0x2A90;  // -> the completed-fence word
constexpr u32 kInKickOff = 0x33B0;        // nonzero while a KickOff is in progress
constexpr u8 kFlags1ImmediateFences = 0x02;

// A fence usually completes within tens of microseconds, so a few yields come first; then
// the wait sleeps until the backend wakes it (kWaitTimeoutUs only bounds a missed wake;
// kSleep is the same wait without the hooks).
constexpr u32 kSpinBeforeWait = 16;
constexpr u32 kWaitTimeoutUs = 1000;
constexpr auto kSleep = std::chrono::microseconds(100);
// A wait that stands still this long with every list run has nothing to end it.
constexpr auto kStuck = std::chrono::milliseconds(1500);

u32 CompletedFence(Device device) {
  const u32 completion = device.W(kFenceCompletion);
  if (!completion)
    return 0;
  // Written by the thread running the GPU backend. Acquire, so what the backend wrote
  // before completing the fence (a resolve's output, a query) is visible once the fence
  // reads as done: a volatile read orders nothing on arm64.
  auto* word = reinterpret_cast<u32*>(renut::GuestPtr(device.base, completion));
  return __builtin_bswap32(std::atomic_ref<u32>(*word).load(std::memory_order_acquire));
}

}  // namespace

bool FencePending(Device device, u32 fence) {
  if (!device || !device.W(kFenceCompletion))
    return false;
  const u32 tag = device.W(off::FenceTag);
  return tag - fence < tag - CompletedFence(device);
}

void BlockOnFence(Device device, u32 fence, bool no_kick, const FenceHooks& hooks) {
  if (!device || !fence || !FencePending(device, fence))
    return;
  // Devices with immediate fences (the command buffer recorders) complete theirs as
  // they write them; the original returns at once for them too.
  if (device.B(off::Flags1) & kFlags1ImmediateFences)
    return;
  const bool covered = i32(fence - device.W(off::FenceTag)) < 0;
  if (!covered) {
    if (no_kick || device.W(kInKickOff))
      return;
    if (hooks.kick)
      hooks.kick(hooks.user, device.va);
  }
  auto last_progress = std::chrono::steady_clock::now();
  u32 last_completed = CompletedFence(device);
  u32 spins = 0;
  for (;;) {
    // The epoch is read before the fence is tested, so a completion between the two
    // moves it and the wait below returns at once.
    const u32 epoch = hooks.epoch ? hooks.epoch() : 0;
    if (!FencePending(device, fence))
      break;
    if (hooks.pause)
      hooks.pause();
    if (++spins <= kSpinBeforeWait)
      std::this_thread::yield();
    else if (hooks.wait)
      hooks.wait(epoch, kWaitTimeoutUs);
    else
      std::this_thread::sleep_for(kSleep);
    const u32 completed = CompletedFence(device);
    const auto now = std::chrono::steady_clock::now();
    if (completed != last_completed) {
      last_completed = completed;
      last_progress = now;
      spins = 0;
      continue;
    }
    if (now - last_progress < kStuck || !hooks.executor_idle || !hooks.executor_idle())
      continue;
    // Every list has run and the fence still stands: nothing will complete it.
    // Complete it as the front end's stuck-wait watchdog does (native/sync.cpp).
    const u32 issued = device.W(off::FenceTag) - 2;
    REXLOG_WARN("Renut: native fence: a wait for fence {:X} stood still 1.5 s with every list run "
                "(completed {:X}, issued {:X}); completing it",
                fence, completed, issued);
    StoreBE32(renut::GuestPtr(device.base, device.W(kFenceCompletion)), issued);
    break;
  }
}

}  // namespace renut::d3d::port
