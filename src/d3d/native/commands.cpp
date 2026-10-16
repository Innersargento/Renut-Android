// Command buffers, predication, the surface clip and Swap.
#include "internal.h"

namespace renut::native_d3d {

void BeginCommandBuffer(PPCContext& ctx) {
  const uint32_t dev = ctx.r3.u32, cb = ctx.r4.u32;
  std::lock_guard lock(g_front.mutex);
  g_counts.recordings.fetch_add(1, std::memory_order_relaxed);
  auto [it, added] = g_front.recording.try_emplace(dev);
  if (added) g_counts.devices.fetch_add(1, std::memory_order_relaxed);
  if (it->second) {
    g_counts.nested_recordings.fetch_add(1, std::memory_order_relaxed);
    it->second->open.store(false, std::memory_order_release);
  }
  auto list = std::make_shared<List>();
  list->command_buffer = cb;
  if (auto words = g_front.command_buffer_words.find(cb); words != g_front.command_buffer_words.end()) {
    list->words.reserve(words->second + words->second / 8 + 64);
  }
  list->open.store(true, std::memory_order_release);
  it->second = list;
  t_recording_dev = dev;
  t_recording = std::move(list);
}

void EndCommandBuffer(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  std::lock_guard lock(g_front.mutex);
  auto it = g_front.recording.find(dev);
  if (it == g_front.recording.end() || !it->second) return;
  const uint32_t cb = U32(base, dev + kRecording);
  if (cb && (U32(base, cb + 108) & 0x8000)) Capture(ctx, base, dev, [&](Device& d) { FlushAll(d); });
  it->second->open.store(false, std::memory_order_release);
  if (t_recording == it->second) {
    t_recording.reset();
    t_recording_dev = 0;
  }
  g_front.command_buffer_words[it->second->command_buffer] = it->second->words.size();
  g_front.command_buffers[it->second->command_buffer] = it->second;
  it->second.reset();
}

void RunCommandBuffer(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, cb = ctx.r4.u32;
  std::lock_guard lock(g_front.mutex);
  g_counts.runs.fetch_add(1, std::memory_order_relaxed);
  Capture(ctx, base, dev, [&](Device& d) {
    uint64_t masks[5];
    for (uint32_t i = 0; i < 5; ++i) masks[i] = U64(base, dev + i * 8) & U64(base, cb + 24 + i * 8);
    Flush(d, masks);
    auto it = g_front.command_buffers.find(cb);
    if (it == g_front.command_buffers.end()) {
      g_counts.unknown_runs.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    d.out.Run(it->second);
  });
  // A command buffer sets its own window scissor (BeginCommandBuffer does);
  // Direct3D's RunCommandBuffer then sets the device's again.
  g_front.immediate_clip_known = false;
}

void SetPredication(PPCContext& ctx) {
  auto lock = LockUnlessOwn(ctx.r3.u32);
  Builder(Current(ctx.r3.u32)).Bin(0, ctx.r4.u32 ? ctx.r4.u32 : 0xFFFFFFFFu);
}

void SetCommandBufferPredication(PPCContext& ctx) {
  auto lock = LockUnlessOwn(ctx.r3.u32);
  const uint32_t lo = ctx.r4.u32, hi = ctx.r5.u32;
  Builder b(Current(ctx.r3.u32));
  if (!lo && !hi) {
    b.Bin(0, 0xFFFFFFFFu);
    b.Bin(1, 0);
  } else {
    b.Bin(0, lo);
    b.Bin(1, hi);
  }
}

void SetSurfaceClip(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  auto lock = LockUnlessOwn(dev);
  g_counts.surface_clips.fetch_add(1, std::memory_order_relaxed);
  if (U8(base, dev + 10940) & 0x30) g_counts.tiled_surface_clips.fetch_add(1, std::memory_order_relaxed);
  const uint32_t x1 = ctx.r4.u32, y1 = ctx.r5.u32, x2 = ctx.r6.u32, y2 = ctx.r7.u32;
  const std::array<uint32_t, 3> values = {0, ((y1 << 16) & 0x3FFF0000u) | (x1 & 0x3FFF),
                                          ((y2 << 16) & 0x3FFF0000u) | (x2 & 0x3FFF)};
  if (List* own = OwnRecording(dev)) {
    Builder(*own).Regs(0x2080, values.data(), 3);
    return;
  }
  if (g_front.main_device && dev != g_front.main_device) {
    g_foreign_clip.store((uint64_t(x1 & 0xFFFF) << 48) | (uint64_t(y1 & 0xFFFF) << 32) | ((x2 & 0xFFFF) << 16) |
                             (y2 & 0xFFFF),
                         std::memory_order_relaxed);
  }
  g_front.window_clip[dev] = values;
  List& list = Current(dev);
  if (&list != g_front.immediate.get()) {
    Builder(list).Regs(0x2080, values.data(), 3);
  } else if (g_foreign_mode.load(std::memory_order_relaxed) != 1) {
    // (In mode 1 Current has just put this device's scissor back.)
    Builder(list).Regs(0x2080, values.data(), 3);
    g_front.immediate_clip = values;
    g_front.immediate_clip_known = true;
  }
}

void Swap(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  g_front.main_device = ctx.r3.u32;
  ++g_front.swaps_seen;
  Builder(*g_front.immediate).Swap(ctx.r4.u32, 0, 0);
  Publish();
}

}  // namespace renut::native_d3d
