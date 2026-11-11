// Command buffers, predication, the surface clip and Swap.
#include "d3d/frontend/internal.h"

namespace renut::native_d3d {

void BeginCommandBuffer(PPCContext& ctx) {
  const uint32_t dev = ctx.r3.u32, cb = ctx.r4.u32;
  std::lock_guard lock(g_front.mutex);
  Bump(g_counts.recordings);
  auto [it, added] = g_front.recording.try_emplace(dev);
  if (added) Bump(g_counts.devices);
  if (it->second) {
    Bump(g_counts.nested_recordings);
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
  Bump(g_counts.runs);
  Capture(ctx, base, dev, [&](Device& d) {
    uint64_t masks[5];
    for (uint32_t i = 0; i < 5; ++i) masks[i] = U64(base, dev + i * 8) & U64(base, cb + 24 + i * 8);
    Flush(d, masks);
    auto it = g_front.command_buffers.find(cb);
    if (it == g_front.command_buffers.end()) {
      Bump(g_counts.unknown_runs);
      return;
    }
    d.out.Run(it->second);
  });
  // A command buffer sets its own window scissor (BeginCommandBuffer does);
  // Direct3D's RunCommandBuffer then sets the device's again.
  g_front.immediate_clip_known = false;
}

void SetSurfaceClip(PPCContext& ctx, uint8_t* base) {
  RecordSurfaceClip(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

void RecordSurfaceClip(uint8_t* base, uint32_t dev, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2) {
  auto lock = LockUnlessOwn(dev);
  Bump(g_counts.surface_clips);
  if (U8(base, dev + 10940) & 0x30) Bump(g_counts.tiled_surface_clips);
  const std::array<uint32_t, 3> values = {0, ((y1 << 16) & 0x3FFF0000u) | (x1 & 0x3FFF),
                                          ((y2 << 16) & 0x3FFF0000u) | (x2 & 0x3FFF)};
  if (List* own = OwnRecording(dev)) {
    Builder(*own).State(RENUT_STATE_WORD(window_offset), values.data(), 3);
    return;
  }
  if (g_front.main_device && dev != g_front.main_device) {
    g_foreign_clip.store((uint64_t(x1 & 0xFFFF) << 48) | (uint64_t(y1 & 0xFFFF) << 32) | ((x2 & 0xFFFF) << 16) |
                             (y2 & 0xFFFF),
                         std::memory_order_relaxed);
  }
  g_front.window_clip[dev] = values;
  List& list = Current(dev);
  // (Current has just put this device's scissor back into the immediate list.)
  if (&list != g_front.immediate.get()) Builder(list).State(RENUT_STATE_WORD(window_offset), values.data(), 3);
}

void Swap(PPCContext& ctx) {
  std::lock_guard lock(g_front.mutex);
  g_front.main_device = ctx.r3.u32;
  ++g_front.swaps_seen;
  Builder(*g_front.immediate).Swap(ctx.r4.u32, 0, 0);
  Publish();
}

}  // namespace renut::native_d3d
