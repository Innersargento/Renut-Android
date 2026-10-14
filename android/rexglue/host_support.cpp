#include "host_support.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <chrono>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

namespace renut {
namespace {
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
}
extern "C" void (*renut_frame_hook)() = nullptr;
extern "C" void renut_note_guest_swap() {
  g_guest_swaps.fetch_add(1, std::memory_order_relaxed);
  if (auto hook = renut_frame_hook) hook();
}
extern "C" uint64_t renut_guest_swap_count() { return g_guest_swaps.load(std::memory_order_relaxed); }
#if !defined(__ANDROID__)
// Frames of silence the output played: counted by the audio output
// (renut_audio.cpp) on Android, which has its own.
extern "C" uint64_t renut_audio_silent_frames() { return 0; }
#endif
