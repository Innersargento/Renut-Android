// What the GPU writes back for the CPU: fences, callbacks, the swap's wait.
#include "d3d/frontend/internal.h"
#include "guest/host_support.h"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ctime>
#endif

REXCVAR_DEFINE_BOOL(renut_stub_command_buffers, true, "reNut",
                    "A device recording a command buffer writes into the ring sink like any other, with no "
                    "segments chained into the buffer (the draws are recorded on the host anyway). Off: the "
                    "recompiled kicks and allocators build the buffer's segments, as before");

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

namespace {

// The ring is a stub: every device's ring pointers are pointed at one scratch block nothing
// reads, so the packets the recompiled code still writes into it go nowhere.
constexpr uint32_t kRingLimit = 52, kRingGuarantee = 56;
constexpr uint32_t kRingSinkMargin = 256u << 10;
constexpr uint32_t kFenceTag = 10908, kFenceCompletion = 10896;
constexpr uint32_t kSegmentStart = 14928;

uint32_t RingSink() {
  static std::once_flag made;
  static uint32_t sink = 0;
  std::call_once(made, [] {
    auto* memory = rex::system::kernel_memory();
    sink = memory ? memory->SystemHeapAlloc(kRingSinkSize, 4096, rex::memory::kSystemHeapPhysical) : 0;
    if (!sink) REXLOG_WARN("Renut: native Direct3D: no memory for the ring sink");
  });
  return sink;
}

// Scratch the inline data (vertices of inline draws, clear rectangles) goes to, in place of the
// ring segments: nothing reads it either. Handed out in a circle over its first half.
constexpr uint32_t kRingDataSize = 1u << 20;

uint32_t GuestBlock(uint32_t bytes) {
  auto* memory = rex::system::kernel_memory();
  return memory ? memory->SystemHeapAlloc(bytes, 4096, rex::memory::kSystemHeapPhysical) : 0;
}

// XMemAlloc(size, attributes), the allocator the original takes these two blocks from, so that
// the device's teardown frees them as it frees what it allocated itself.
uint32_t GameAlloc(const PPCContext& caller, uint8_t* base, uint32_t bytes, uint32_t attributes) {
  PPCContext call = caller;
  call.r3.u32 = bytes;
  call.r4.u32 = attributes;
  sub_82265B90(call, base);
  return call.r3.u32;
}

uint32_t RingData(uint32_t bytes, uint32_t alignment) {
  static std::once_flag made;
  static uint32_t data = 0;
  static std::atomic<uint32_t> offset{0};
  std::call_once(made, [] { data = GuestBlock(kRingDataSize); });
  if (!data || bytes > kRingDataSize / 2) return 0;
  if (alignment < 4 || (alignment & (alignment - 1))) alignment = 4;
  uint32_t at = offset.fetch_add(bytes + alignment, std::memory_order_relaxed) % (kRingDataSize / 2);
  at = (at + alignment - 1) & ~(alignment - 1);
  return data + at;
}

void RewindRing(uint8_t* base, uint32_t dev) {
  const uint32_t sink = RingSink();
  if (!sink) return;
  S32(base, dev + kRingWrite, sink);
  S32(base, dev + kRingLimit, sink + kRingSinkSize);
  S32(base, dev + kRingGuarantee, sink + kRingSinkSize - kRingSinkMargin);
  // A device recording a command buffer also names where its segment starts (the first dword
  // written, one past the ring pointer): EndCommandBuffer sizes the segment from it, and a size
  // beyond the segment limit makes it report the buffer out of memory.
  if (U32(base, dev + kRecording)) S32(base, dev + kSegmentStart, sink + 4);
}

// A device recording a command buffer is the original's when the cvar is off: its kicks and its
// allocators chain real segments into the buffer.
bool KeepsOriginal(const uint8_t* base, uint32_t dev) {
  return U32(base, dev + kRecording) && !REXCVAR_GET(renut_stub_command_buffers);
}

// CDevice::KickOff without a ring: the fence covering the work so far is issued as the original
// KickOff issues it (SetFence records the write the backend completes when it runs this list, in
// order with the callbacks and the presents, and moves the tag on), the recorded list goes to the
// backend and the ring starts over. A device recording a command buffer only starts over.
void KickDevice(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  if (!U32(base, dev + kRecording)) {
    PPCContext call = ctx;
    call.r3.u32 = dev;
    call.r4.u32 = U32(base, dev + kRingWrite);
    sub_82278528(call, base);
    {
      std::lock_guard lock(g_front.mutex);
      Publish();
    }
  }
  RewindRing(base, dev);
}

}  // namespace

uint32_t RingSinkAddress() { return RingSink(); }

void RewindRecordingRing(uint8_t* base, uint32_t device) { RewindRing(base, device); }

// D3DDevice_SetRingBufferParameters(device, parameters) (sub_82657508), the device's first act.
// The original allocates the ring and its secondary segment, registers them with the kernel's
// video code (VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack) and writes the packets that
// set the GPU up. None of that is needed: the ring is a stub. What the rest of Direct3D reads
// from it stays: the fence completion word (+10896) and the scratch writeback block (+10900),
// the first fence tag, and the ring pointers.
void SetRingBufferParameters(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  ctx.r3.u64 = 0;  // S_OK
  if (!dev) return;
  uint32_t completion = U32(base, dev + kFenceCompletion), writeback = U32(base, dev + 10900);
  if (!completion) {
    completion = GameAlloc(ctx, base, 96, 0xA6800000u);
    S32(base, dev + kFenceCompletion, completion);
  }
  if (!writeback) {
    writeback = GameAlloc(ctx, base, 32, 0x95800000u);
    S32(base, dev + 10900, writeback);
  }
  if (!completion || !writeback) {
    ctx.r3.u64 = 0x8007000Eu;  // E_OUTOFMEMORY
    return;
  }
  std::memset(GuestPtr(base, completion), 0, 96);
  std::memset(GuestPtr(base, writeback), 0, 32);
  if (!U32(base, dev + kFenceTag)) S32(base, dev + kFenceTag, 3);
  S32(base, dev + 10952, 0);
  S32(base, dev + 14916, 0);
  RewindRing(base, dev);
  // The fence before the current one is done, at the ring position the device starts from.
  S32(base, completion, U32(base, dev + kFenceTag) - 2);
  S32(base, completion + 4, (U32(base, dev + 14920) & 3) | U32(base, dev + kRingWrite));
  // The two halfwords the original leaves for the display set-up (+10924, +10926).
  uint8_t* fields = GuestPtr(base, dev + 10924);
  fields[0] = 0, fields[1] = 1, fields[2] = 0x08, fields[3] = 0x00;
}

// CDevice::BeginRingAlloc(device, dwords, alignment) (sub_8222D240): room for inline data. The
// inline draws a recording device makes are hooked and take their data from their own scratch
// (ScratchAlloc, which the list names by address), so what the ring scratch holds here is not
// something a list reads.
bool BeginRingAlloc(PPCContext& ctx, uint8_t* base) {
  if (KeepsOriginal(base, ctx.r3.u32)) return false;
  ctx.r3.u64 = RingData(ctx.r4.u32 * 4, ctx.r5.u32);
  return true;
}

// D3D::RingAllocate(device, flags, &bytes, alignment) (sub_8222D3E0): the same, by byte count.
bool RingAllocate(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = ctx.r5.u32 ? RingData(U32(base, ctx.r5.u32), ctx.r6.u32) : 0;
  return true;
}

// CDevice::StartNewSegment(device, dwords) (sub_822873E8): the next ring segment, which here is the
// sink again.
bool StartNewSegment(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  if (KeepsOriginal(base, dev)) return false;
  RewindRing(base, dev);
  ctx.r3.u64 = U32(base, dev + kRingWrite);
  return true;
}

// CDevice::SetFence(device, ring) (sub_82278528), without the packets. The original writes ten words
// at `ring` (two EVENT_WRITE_SHDs: the ring position to completion+4, the fence to completion+0),
// remembers the ring position in +13240/+13244, completes the fence itself on a device with immediate
// fences, and moves the fence tag on by 2. The writes go nowhere here, so what is kept is the rest:
// the position, the tag, the immediate completion, and the record the backend completes the fence
// from when it runs the list (kOpFence), in order with the callbacks and the presents.
// A device with no completion word and the time before the first act are the original's. So is a
// device recording a command buffer that does not have immediate fences (the recorders do): the
// fence would be a packet in the buffer. One that does completes its own fence and records nothing,
// as the original's packets only matter to a GPU that runs the buffer.
constexpr uint32_t kFenceBytes = 40;  // the ten words the original writes
bool SetFence(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, ring = ctx.r4.u32;
  const bool recording = U32(base, dev + kRecording) != 0;
  const bool immediate = (*GuestPtr(base, dev + 10941) & 2) != 0;
  if (recording && (!immediate || !REXCVAR_GET(renut_stub_command_buffers))) return false;
  const uint32_t block = U32(base, dev + kFenceCompletion);
  if (!block) return false;
  std::unique_lock lock(g_front.mutex, std::defer_lock);
  if (!recording) {
    lock.lock();
    if (!g_front.sync_recording) return false;
  }
  const uint32_t fence = U32(base, dev + kFenceTag);
  const uint32_t position = (U32(base, dev + 14920) & 3) | U32(base, dev + kRingWrite);
  S32(base, dev + 13240, U32(base, dev + kRingWrite));
  S32(base, dev + 13244, U32(base, dev + 14920));
  if (!recording) {
    List& list = Current(dev);
    list.words.push_back(kOpFence);
    list.words.push_back(Phys(block));
    list.words.push_back(position);
    list.words.push_back(fence);
    g_counts.fences.fetch_add(1, std::memory_order_relaxed);
  }
  // A device with immediate fences (and no memory export in progress, +21532) completes its own.
  if (!U32(base, dev + 21532) && (*GuestPtr(base, dev + 10941) & 2)) {
    S32(base, block, fence);
    S32(base, block + 4, position);
  }
  S32(base, dev + kFenceTag, fence + 2);
  ctx.r3.u64 = ring + kFenceBytes;  // the ring past the packets
  return true;
}

// CDevice::KickOff(device) returns the new write pointer; KickOffSegment ignores r3. A device
// recording a command buffer only starts over (KickDevice): the original's kick would chain a
// segment into the buffer, which EndCommandBuffer and RunCommandBuffer (the originals) then walk,
// and they find none; its segment start is kept with the ring (RewindRing).
bool KickOff(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  if (KeepsOriginal(base, dev)) return false;
  KickDevice(ctx, base, dev);
  ctx.r3.u64 = U32(base, dev + kRingWrite);
  return true;
}

bool AddCallsToPrimaryBuffer(PPCContext& ctx, uint8_t*) {
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
  // D3D::InsertCallback(device, ring, flags, function, context) returns the ring past the packets
  // that make the GPU call the function. The ring is a stub, so there are none: the callback
  // goes into the list as an effect and the ring comes back as it was.
  const uint32_t dev = ctx.r3.u32, ring = ctx.r4.u32;
  const uint32_t flags = ctx.r5.u32, function = ctx.r6.u32, context = ctx.r7.u32;
  std::unique_lock lock(g_front.mutex);
  if (!g_front.sync_recording) return false;
  ctx.r3.u64 = ring;
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

void WaitForBackend(uint32_t max_lists_ahead, uint32_t timeout_ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const uint64_t published = g_counts.lists_published.load(std::memory_order_relaxed);
    const uint64_t executed = g_counts.lists_executed.load(std::memory_order_relaxed);
    if (published <= executed + max_lists_ahead) return;
    if (std::chrono::steady_clock::now() >= until) {
      if (g_counts.brake_timeouts.fetch_add(1, std::memory_order_relaxed) == 0)
        REXLOG_WARN("Renut: the command buffer brake timed out ({} lists ahead of the backend)", published - executed);
      return;
    }
    FenceWait(FenceEpoch(), 1000);
  }
}

uint32_t MainDevice() {
  std::lock_guard lock(g_front.mutex);
  return g_front.main_device;
}

bool ExecutorIdle() {
  return g_executor_busy.load(std::memory_order_relaxed) <= 0 &&
         g_counts.lists_executed.load(std::memory_order_relaxed) >=
             g_counts.lists_published.load(std::memory_order_relaxed);
}

namespace {
std::atomic<uint32_t> g_fence_epoch{0};
std::atomic<uint32_t> g_fence_waiters{0};
}  // namespace

uint32_t FenceEpoch() { return g_fence_epoch.load(std::memory_order_seq_cst); }

void FenceWait(uint32_t epoch, uint32_t timeout_us) {
#if defined(__linux__)
  g_fence_waiters.fetch_add(1, std::memory_order_seq_cst);
  timespec timeout{};
  timeout.tv_sec = timeout_us / 1000000;
  timeout.tv_nsec = long(timeout_us % 1000000) * 1000;
  // Returns at once when the epoch already moved (EAGAIN), so a completion that raced
  // this call is not slept through.
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_fence_epoch), FUTEX_WAIT_PRIVATE, epoch, &timeout,
          nullptr, 0);
  g_fence_waiters.fetch_sub(1, std::memory_order_seq_cst);
#else
  (void)epoch;
  std::this_thread::sleep_for(std::chrono::microseconds(std::min(timeout_us, 100u)));
#endif
}

void NotifyFenceCompleted() {
  g_fence_epoch.fetch_add(1, std::memory_order_seq_cst);
#if defined(__linux__)
  if (g_fence_waiters.load(std::memory_order_seq_cst))
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_fence_epoch), FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr,
            nullptr, 0);
#endif
}

namespace {

// The wait watchdog's memory of the waits in progress, one entry per blocker (a Direct3D wait keeps its
// blocker on the stack of the thread that waits, so the address names the wait). The slow path of
// CheckWait runs at most a thousand times a second per waiting thread, so a mutex is cheap here.
struct WaitWatch {
  uint32_t blocker = 0, done = 0;
  std::chrono::steady_clock::time_point since, seen;
  bool fired = false;
};
std::mutex g_watch_mutex;
std::array<WaitWatch, 8> g_watch;

}  // namespace

// D3D_CBlocker_Check(blocker) (sub_8264DCD0), the poll in every Direct3D wait: the game calls it in a
// loop with nothing in between until the GPU has caught up. On the console each pass costs a few
// hundred cycles the recompiler drops, so here the loop is a bare busy wait: tens of millions of calls
// a second, three game threads at 100% (86% of the main thread's cycles, 60% of the process's). Only
// every 64th call looks at the wait; when the GPU has not moved since the blocker last looked (the
// device's fence word at [[device + 10896]] against the copy the original keeps at blocker + 8) the
// thread sleeps until the backend completes a fence (NotifyFenceCompleted), or a millisecond. The call
// itself still runs, so the blocker's bookkeeping and its own timeout keep their behaviour.
// The same slow path is the watchdog: a wait that has stood still for 1.5 s with every list run has
// nothing that could end it, and is completed.
void CheckWait(PPCContext& ctx, uint8_t* base) {
  static uint32_t polls = 0;  // plain: a heuristic, the threads that wait are few and need no ordering
  if (++polls & 63) return;
  // The player's pause: this loop calls no kernel wait, so it parks here.
  renut::WaitIfTitlePaused();
  const uint32_t blocker = ctx.r3.u32;
  if (!blocker) return;
  const uint32_t dev = U32(base, blocker);
  const uint32_t block = dev ? U32(base, dev + 10896) : 0;
  if (!block) return;
  // Bit 1 of +10941 marks a device that is hung or done: the original returns at once.
  if (*GuestPtr(base, dev + 10941) & 2) return;
  const uint32_t done = U32(base, block);
  if (done != U32(base, blocker + 8)) return;  // the GPU moved; the original records the new value
  // Not moving: sleep until a fence completes. The epoch is read before the word is tested again, so a
  // completion between the two moves it and the wait returns at once.
  const uint32_t epoch = FenceEpoch();
  if (U32(base, block) == U32(base, blocker + 8)) FenceWait(epoch, 1000);

  const auto now = std::chrono::steady_clock::now();
  WaitWatch* wait = nullptr;
  std::lock_guard lock(g_watch_mutex);
  for (WaitWatch& entry : g_watch) {
    if (entry.blocker == blocker) wait = &entry;
  }
  if (!wait) {
    wait = &g_watch[0];
    for (WaitWatch& entry : g_watch) {
      if (!entry.blocker) { wait = &entry; break; }
      if (entry.seen < wait->seen) wait = &entry;
    }
    *wait = WaitWatch{blocker, done, now, now, false};
    return;
  }
  // A new wait of the same blocker (the stack slot is reused every frame), or the fence moved.
  if (done != wait->done || now - wait->seen > std::chrono::milliseconds(100)) {
    *wait = WaitWatch{blocker, done, now, now, false};
    return;
  }
  wait->seen = now;
  if (wait->fired || now - wait->since < std::chrono::milliseconds(1500)) return;
  // Only a wait nothing will end: every published list has run and the
  // GPU backend is idle.
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
    wait->since = now;
    return;
  }
  wait->fired = true;
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
