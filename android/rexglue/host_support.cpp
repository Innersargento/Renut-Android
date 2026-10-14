#include "host_support.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

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

}  // namespace renut

// The frame counter of the host window, and the hook the frame-pacing hint
// (adpf.cpp) hangs on. On Android these are still the ones the patched SDK's
// command processor carries.
#if !defined(__ANDROID__)
namespace {
std::atomic<uint64_t> g_guest_swaps{0};
}
extern "C" void (*renut_frame_hook)() = nullptr;
extern "C" void renut_note_guest_swap() {
  g_guest_swaps.fetch_add(1, std::memory_order_relaxed);
  if (auto hook = renut_frame_hook) hook();
}
extern "C" uint64_t renut_guest_swap_count() { return g_guest_swaps.load(std::memory_order_relaxed); }
// Frames of silence the output played: counted by the SDK's audio driver only
// where that is patched.
extern "C" uint64_t renut_audio_silent_frames() { return 0; }
#endif
