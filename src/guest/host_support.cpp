#include "host_support.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <algorithm>
#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#endif
#include <chrono>
#include <thread>
#include <vector>

#include <rex/chrono.h>
#include <rex/kernel/thread_state.h>
#include "platform/guest.h"

namespace renut {
namespace {
#if !defined(__ANDROID__) && !defined(__linux__)
inline int gettid() { return 0; }
#endif
std::atomic<bool> g_paused{false};
std::mutex g_mutex;
std::condition_variable g_resumed;
}  // namespace

void SetTitlePaused(bool paused) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_paused.store(paused, std::memory_order_release);
  }
  if (!paused) g_resumed.notify_all();
}

bool IsTitlePaused() { return g_paused.load(std::memory_order_acquire); }

namespace {
thread_local bool tls_host_thread = false;
}

void MarkHostThread() { tls_host_thread = true; }

void WaitIfTitlePaused() {
  if (!g_paused.load(std::memory_order_acquire)) return;
  if (tls_host_thread || !rex::kernel::ThreadState::Get()) return;
  std::unique_lock<std::mutex> lock(g_mutex);
  g_resumed.wait(lock, [&] {
    if (!g_paused.load(std::memory_order_acquire)) return true;
    const rex::kernel::TitleRuntime* title = host::Title();
    return !title || !title->IsRunning();
  });
}

}  // namespace renut

// The frame counter of the host window, and the hook the frame-pacing hint
// (adpf.cpp) hangs on.
namespace {
std::atomic<uint64_t> g_guest_swaps{0};
// The instant (steady clock, ns) of each of the last swaps, for the frame-time readout.
constexpr size_t kSwapRing = 512;
std::atomic<uint64_t> g_swap_ns[kSwapRing];
uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}
extern "C" void (*renut_frame_hook)() = nullptr;
extern "C" void renut_note_guest_swap() {
  const uint64_t n = g_guest_swaps.fetch_add(1, std::memory_order_relaxed);
  g_swap_ns[n % kSwapRing].store(SteadyNs(), std::memory_order_relaxed);
  if (auto hook = renut_frame_hook) hook();
}
extern "C" uint64_t renut_guest_swap_count() { return g_guest_swaps.load(std::memory_order_relaxed); }
// The last `max` (at most 256) intervals between swaps in milliseconds, oldest first; none when the game has not
// swapped for two seconds (paused, loading, or not started).
extern "C" size_t renut_frame_times(float* out, size_t max) {
  const uint64_t count = g_guest_swaps.load(std::memory_order_relaxed);
  if (count < 2) return 0;
  const uint64_t newest = g_swap_ns[(count - 1) % kSwapRing].load(std::memory_order_relaxed);
  if (SteadyNs() - newest > 2000000000ull) return 0;
  const size_t wanted = std::min<size_t>({max, 256, size_t(count - 1)});
  size_t written = 0;
  for (uint64_t i = count - wanted; i < count; ++i) {
    const uint64_t now = g_swap_ns[i % kSwapRing].load(std::memory_order_relaxed);
    const uint64_t before = g_swap_ns[(i - 1) % kSwapRing].load(std::memory_order_relaxed);
    if (now > before) out[written++] = float(double(now - before) / 1e6);
  }
  return written;
}
