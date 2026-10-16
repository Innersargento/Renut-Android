// The recording lists, publishing and scratch guest memory.
#include "internal.h"

namespace renut::native_d3d {

List* OwnRecording(uint32_t dev) {
  if (dev != t_recording_dev || !t_recording || !t_recording->open.load(std::memory_order_acquire)) return nullptr;
  return t_recording.get();
}

std::unique_lock<std::recursive_mutex> LockUnlessOwn(uint32_t dev) {
  if (OwnRecording(dev)) return {};
  return std::unique_lock<std::recursive_mutex>(g_front.mutex);
}

void PutBackClip(uint32_t dev, List& list) {
  auto it = g_front.window_clip.find(dev);
  if (it == g_front.window_clip.end()) return;
  if (g_front.immediate_clip_known && g_front.immediate_clip == it->second) return;
  Builder(list).Regs(0x2080, it->second.data(), 3);
  g_front.immediate_clip = it->second;
  g_front.immediate_clip_known = true;
}

List& Current(uint32_t dev) {
  if (List* own = OwnRecording(dev)) return *own;
  auto it = g_front.recording.find(dev);
  if (it != g_front.recording.end() && it->second) return *it->second;
  const int mode = g_foreign_mode.load(std::memory_order_relaxed);
  if (g_front.main_device && dev != g_front.main_device) {
    g_counts.foreign_immediate.fetch_add(1, std::memory_order_relaxed);
    if (t_function < g_foreign.size()) g_foreign[t_function].fetch_add(1, std::memory_order_relaxed);
    g_foreign_device.store(dev, std::memory_order_relaxed);
    if (mode == 2) {
      if (g_front.discard.words.size() > (1u << 20)) g_front.discard.words.clear();
      return g_front.discard;
    }
  }
  if (mode == 1) PutBackClip(dev, *g_front.immediate);
  return *g_front.immediate;
}

void Publish() {
  if (g_front.immediate->words.empty()) return;
  std::shared_ptr<const List> done = std::move(g_front.immediate);
  g_front.immediate = std::make_shared<List>();
  g_front.immediate->words.reserve(done->words.size());
  {
    std::lock_guard lock(g_front.published_mutex);
    g_front.published.push_back(std::move(done));
  }
  g_counts.publishes.fetch_add(1, std::memory_order_relaxed);
  g_counts.lists_published.fetch_add(1, std::memory_order_relaxed);
  if (g_front.sync_recording) {
    g_work_pending.store(true, std::memory_order_release);
    std::shared_ptr<std::function<void()>> wake;
    {
      std::lock_guard lock(g_wake_mutex);
      wake = g_wake;
    }
    if (wake && *wake) (*wake)();
  }
}

uint32_t ScratchAlloc(uint32_t bytes) {
  std::call_once(g_scratch.made, [] {
    auto* memory = rex::system::kernel_memory();
    g_scratch.base =
        memory ? memory->SystemHeapAlloc(g_scratch.size, 4096, rex::memory::kSystemHeapPhysical) : 0;
    if (g_scratch.base) {
      REXLOG_INFO("Renut: native Direct3D scratch {:08X} ({} MB)", g_scratch.base, g_scratch.size >> 20);
    }
  });
  if (!g_scratch.base) {
    g_counts.no_scratch.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  bytes = (bytes + 63) & ~63u;
  if (bytes > g_scratch.size / 4) return 0;
  uint32_t at = g_scratch.offset.load(std::memory_order_relaxed), end;
  do {
    end = at + bytes > g_scratch.size ? bytes : at + bytes;
  } while (!g_scratch.offset.compare_exchange_weak(at, end, std::memory_order_relaxed));
  return g_scratch.base + end - bytes;
}

uint32_t ProgramSlot() {
  thread_local uint32_t slot = 0;
  if (!slot) {
    if (auto* memory = rex::system::kernel_memory()) slot = memory->SystemHeapAlloc(16, 16);
  }
  return slot;
}

}  // namespace renut::native_d3d
