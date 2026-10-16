// What the GPU writes back for the CPU: fences, callbacks, the swap's wait.
#include "internal.h"

namespace renut::native_d3d {

Effects CallbackEffects(uint32_t flags, uint32_t function, uint32_t context) {
  Effects out;
  uint32_t mask = (flags >> 24) & 0x3F;
  if (!mask) mask = 4;
  if (!(flags & 6)) flags |= 6;
  if (flags & 2) {
    if (!(flags & 1)) out.push_back({kRegWaitUntil, 0x20000u});
    out.push_back({kRegCallbackAddress, function});
    out.push_back({kRegCallbackContext, context});
    out.push_back({kRegScratch0, mask});
    out.push_back({kEffectInterrupt, mask});
    out.push_back({kRegCallbackAck, mask});
  }
  if (flags & 4) out.push_back({kRegCallbackAddress, 0x0BADF00Du});
  return out;
}

void PushEffects(List& list, const Effects& effects) {
  list.words.push_back(kOpEffects | (uint32_t(effects.size()) << 8));
  for (const auto& [reg, value] : effects) {
    list.words.push_back(reg);
    list.words.push_back(value);
  }
  g_counts.effect_records.fetch_add(1, std::memory_order_relaxed);
}

void SetFence(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  std::lock_guard lock(g_front.mutex);
  if (!g_front.sync_recording) return;
  const uint32_t block = U32(base, dev + 10896);
  if (!block) return;
  List& list = Current(dev);
  list.words.push_back(kOpFence);
  list.words.push_back(Phys(block));
  list.words.push_back((U32(base, dev + 14920) & 3) | U32(base, dev + kRingWrite));
  list.words.push_back(U32(base, dev + 10908));
  g_counts.fences.fetch_add(1, std::memory_order_relaxed);
}

bool AddCallsToPrimaryBuffer(PPCContext& ctx, uint8_t*) {
  if (GetMode() != Mode::kNative) return false;
  std::lock_guard lock(g_front.mutex);
  if (g_front.main_device && ctx.r3.u32 != g_front.main_device) g_foreign_doorbells.fetch_add(1, std::memory_order_relaxed);
  g_counts.doorbells.fetch_add(1, std::memory_order_relaxed);
  if (g_front.sync_recording) g_counts.native_doorbells.fetch_add(1, std::memory_order_relaxed);
  const bool carried = g_front.sync_recording;
  Publish();
  const bool skip = carried && ctx.r3.u32 == g_front.main_device;
  if (skip) g_counts.doorbells_skipped.fetch_add(1, std::memory_order_relaxed);
  return skip;
}

bool InsertCallback(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, ring = ctx.r4.u32;
  const uint32_t flags = ctx.r5.u32, function = ctx.r6.u32, context = ctx.r7.u32;
  std::unique_lock lock(g_front.mutex);
  if (!g_front.sync_recording) return false;
  lock.unlock();
  __imp__sub_822052C8(ctx, base);
  const uint32_t end = ctx.r3.u32;
  if (end <= ring || end - ring > 4096) return true;
  lock.lock();
  List& list = Current(dev);
  g_counts.callbacks.fetch_add(1, std::memory_order_relaxed);
  PushEffects(list, CallbackEffects(flags, function, context));
  return true;
}

bool SynchronizeToPresentationInterval(PPCContext& ctx, uint8_t* base) {
  if (renut::pacing::ImmediatePresentation()) ctx.r4.u32 = 1;
  const uint32_t dev = ctx.r3.u32, immediate = ctx.r4.u32;
  std::unique_lock lock(g_front.mutex);
  if (!g_front.sync_recording) return false;
  const uint32_t block = U32(base, dev + 10900);
  const bool wait = !immediate && block;
  if (wait) {
    List& list = Current(dev);
    PushEffects(list, {{kRegScratch1, 1}});  // the present interval, written back
  }
  lock.unlock();
  __imp__sub_8228A748(ctx, base);
  if (wait) {
    lock.lock();
    List& list = Current(dev);
    list.words.push_back(kOpWaitMemory);
    list.words.push_back(Phys(block + 4));
    list.words.push_back(0);
    list.words.push_back(0xFFFFFFFFu);
  }
  return true;
}

void CheckWait(PPCContext& ctx, uint8_t* base) {
  struct Wait {
    uint32_t blocker = 0, done = 0, calls = 0;
    std::chrono::steady_clock::time_point since;
    bool fired = false;
  };
  thread_local Wait wait;
  if (++wait.calls & 1023) return;
  const uint32_t blocker = ctx.r3.u32, dev = U32(base, blocker);
  const uint32_t block = dev ? U32(base, dev + 10896) : 0;
  if (!block) return;
  const uint32_t done = U32(base, block);
  const auto now = std::chrono::steady_clock::now();
  if (blocker != wait.blocker || done != wait.done) {
    wait = Wait{blocker, done, wait.calls, now, false};
    return;
  }
  if (wait.fired || now - wait.since < std::chrono::milliseconds(1500)) return;
  // Only a wait nothing will end: every published list has run and the
  // command processor is idle.
  if (g_executor_busy.load(std::memory_order_relaxed) > 0 ||
      g_counts.lists_executed.load(std::memory_order_relaxed) <
          g_counts.lists_published.load(std::memory_order_relaxed)) {
    // A wait that stands still while the executor still owes work: not stuck yet.
    REXLOG_DEBUG("Renut: native sync: wait kind {} still after 1.5 s, executor busy {}, lists run {} of {} published, "
                "fence {:X} (issued {:X}), position {:X}",
                U32(base, blocker + 4), g_executor_busy.load(std::memory_order_relaxed),
                g_counts.lists_executed.load(std::memory_order_relaxed),
                g_counts.lists_published.load(std::memory_order_relaxed), done, U32(base, dev + 10908) - 2,
                U32(base, block + 4));
    wait.since = now;
    return;
  }
  wait.fired = true;
  g_counts.stuck_waits.fetch_add(1, std::memory_order_relaxed);
  const uint32_t issued = U32(base, dev + 10908) - 2;
  const uint32_t position = (U32(base, dev + 13244) & 3) | U32(base, dev + 13240);
  REXLOG_WARN("Renut: native sync: a wait (kind {}) stood still 1.5 s with every list run: fence {:X} of {:X}, "
              "position {:X} of {:X}; completing it",
              U32(base, blocker + 4), done, issued, U32(base, block + 4), position);
  S32(base, block + 4, position);
  S32(base, block, issued);
}

}  // namespace renut::native_d3d
