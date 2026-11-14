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

#include <rex/chrono/clock.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

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

void WaitIfTitlePaused() {
  if (!g_paused.load(std::memory_order_acquire)) return;
  if (!rex::system::XThread::IsInThread()) return;
  rex::system::XThread* self = rex::system::XThread::GetCurrentThread();
  if (!self || !self->is_guest_thread()) return;
  rex::system::KernelState* kernel = self->kernel_state();
  std::unique_lock<std::mutex> lock(g_mutex);
  g_resumed.wait(lock, [&] {
    return !g_paused.load(std::memory_order_acquire) || (kernel && kernel->is_terminating_title());
  });
}

namespace {
constexpr uint32_t kWatchSlots = 4;
std::atomic<uint32_t> g_watched[kWatchSlots];
std::atomic<void (*)(int)> g_signaler_callback{nullptr};
thread_local bool tls_signaler_told = false;
}  // namespace

bool WatchSignalObject(uint32_t slot, uint32_t guest_address) {
  if (slot >= kWatchSlots) return false;
  return g_watched[slot].exchange(guest_address, std::memory_order_relaxed) != guest_address;
}

bool AnyWatched() {
  for (uint32_t i = 0; i < kWatchSlots; ++i) {
    if (g_watched[i].load(std::memory_order_relaxed)) return true;
  }
  return false;
}

void SetSignalerCallback(void (*callback)(int tid)) { g_signaler_callback.store(callback); }

void NoteSignal(uint32_t guest_address) {
  if (tls_signaler_told) return;
  for (uint32_t i = 0; i < kWatchSlots; ++i) {
    if (g_watched[i].load(std::memory_order_relaxed) == guest_address && guest_address) {
      tls_signaler_told = true;
      if (auto callback = g_signaler_callback.load()) callback(static_cast<int>(gettid()));
      return;
    }
  }
}

namespace {
thread_local bool tls_wait_probe = false;
std::mutex g_probe_mutex;
std::vector<WaitProbeEntry> g_probe;
}  // namespace

void MarkWaitProbeThread() { tls_wait_probe = true; }
bool IsWaitProbeThread() { return tls_wait_probe; }

void NoteProbedWait(uint32_t what, uint32_t object, uint32_t caller, uint64_t nanoseconds) {
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  for (WaitProbeEntry& entry : g_probe) {
    if (entry.what == what && entry.object == object && entry.caller == caller) {
      ++entry.count;
      entry.nanoseconds += nanoseconds;
      return;
    }
  }
  if (g_probe.size() < 64) g_probe.push_back({what, object, caller, 1, nanoseconds});
}

void TakeWaitProbe(WaitProbeEntry* out, size_t limit, size_t* count) {
  std::vector<WaitProbeEntry> taken;
  {
    std::lock_guard<std::mutex> lock(g_probe_mutex);
    taken.swap(g_probe);
  }
  std::sort(taken.begin(), taken.end(),
            [](const WaitProbeEntry& a, const WaitProbeEntry& b) { return a.nanoseconds > b.nanoseconds; });
  *count = std::min(limit, taken.size());
  for (size_t i = 0; i < *count; ++i) out[i] = taken[i];
}

namespace {
std::thread g_kernel_time_thread;
std::atomic<bool> g_kernel_time_running{false};
}  // namespace

void StartKernelTime(rex::system::KernelState* kernel) {
  if (!kernel || g_kernel_time_running.exchange(true)) return;
  g_kernel_time_thread = std::thread([kernel] {
    using rex::system::XThread;
    while (g_kernel_time_running.load(std::memory_order_relaxed)) {
      const uint32_t now = rex::chrono::Clock::QueryGuestUptimeMillis();
      for (auto& thread : kernel->object_table()->GetObjectsByType<XThread>()) {
        if (thread && thread->guest_object()) {
          // X_KTHREAD 0x58 (the SDK's header names it unk_58).
          rex::memory::store_and_swap<uint32_t>(thread->guest_object<uint8_t>() + 0x58, now);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
  });
}

void StopKernelTime() {
  if (!g_kernel_time_running.exchange(false)) return;
  if (g_kernel_time_thread.joinable()) g_kernel_time_thread.join();
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
#if !defined(__ANDROID__)
// Frames of silence the output played: counted by the audio output
// (renut_audio.cpp) on Android, which has its own.
extern "C" uint64_t renut_audio_silent_frames() { return 0; }
#endif
