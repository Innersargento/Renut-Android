// Replacements for the SDK's kernel exports that reNut needs to behave
// differently. The SDK is used exactly as released: the recompiled game calls
// an import by a symbol name (`__imp__<Export>`), and the build compiles the
// game's code with that name replaced by `renut_imp_<Export>` for each export
// listed in kernel_overrides.h. What is defined here is therefore what the
// game calls; the SDK's own function stays reachable under its real name for
// whatever the override does not take over.
#include <cstdint>
#include "platform/guest.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/kernel/guest_allocator.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/hook.h>
#include <rex/chrono.h>

#include "guest_address.h"
#include "platform/audio_threads.h"
#include "platform/log.h"
#include "host_support.h"
#include "d3d/frontend/native_d3d.h"
#include "plume/plume_gpu.h"


#define X(name) extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);
#include "kernel_overrides.h"
#undef X

namespace {

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

}  // namespace

// VdSwap(buffer, fetch, unk2, unk3, unk4, frontbuffer, format, color space,
// width, height): the game's present. With native sync the frame goes into the
// swapping device's list and the buffer is left as it is: the ring is a stub that
// nothing reads, so no swap packet is needed. The rest is the SDK's.
extern "C" void renut_imp_VdSwap(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t buffer_address = ctx.r3.u32;
  const uint32_t fetch_address = ctx.r4.u32;
  // The ninth and tenth arguments are on the stack (r1 + 0x54 and + 0x5C, the
  // ABI the SDK's own argument translator reads): they are not r11 and r12,
  // which are scratch at the call and held a pointer into the device.
  const uint32_t width_address = LoadBE32(renut::GuestPtr(base, ctx.r1.u32 + 0x54));
  const uint32_t height_address = LoadBE32(renut::GuestPtr(base, ctx.r1.u32 + 0x5C));
  if (buffer_address && fetch_address && width_address && height_address) {
    // The texture header's fetch constant holds the virtual address of the
    // frontbuffer, shifted right 12 (dword 1, bits 12 and up).
    const uint32_t virtual_base = (LoadBE32(renut::GuestPtr(base, fetch_address) + 4) >> 12) << 12;
    const uint32_t width = LoadBE32(renut::GuestPtr(base, width_address));
    const uint32_t height = LoadBE32(renut::GuestPtr(base, height_address));
    // RecordPresent turns the virtual address into the GPU address the resolves are keyed by.
    if (renut::native_d3d::RecordPresent(virtual_base, width, height)) {
      return;
    }
  }
  __imp__VdSwap(ctx, base);
}

// XamAlloc, XamAllocEx and XamFree on the title workspace. RexGlue 0.11.0 hands XAM a reference to the
// workspace when the title is created, and the kernel later builds a new workspace at the size the XEX
// declares (the old heap is destroyed): XAM's allocations then run on freed memory. These take the live
// workspace from the title runtime instead; whatever does not touch the workspace is still the SDK's.
namespace {
constexpr uint32_t kXamAllocWorkspace = 0x10000000, kXamAllocSystem = 0x04000000, kXamAllocNoZero = 0x00100000,
                   kXamAllocPhysical = 0x40000000;
std::mutex g_workspace_blocks_mutex;
std::unordered_set<uint32_t> g_workspace_blocks;

bool UsesWorkspace(uint32_t flags) {
  return !(flags & kXamAllocSystem) && (flags == 0 || (flags & kXamAllocWorkspace));
}

// Returns the HRESULT; the block address is written to the guest's out pointer.
uint32_t AllocWorkspaceBlock(uint32_t flags, uint32_t size, uint32_t out_address, uint8_t* base) {
  auto* title = renut::host::Title();
  if (!out_address || !title) return 0x80070057;  // E_INVALIDARG
  StoreBE32(renut::GuestPtr(base, out_address), 0);
  auto address = title->GetGuestWorkspace().Allocate(size, !(flags & kXamAllocNoZero));
  if (!address) return 0x8007000E;  // E_OUTOFMEMORY
  {
    std::lock_guard lock(g_workspace_blocks_mutex);
    g_workspace_blocks.insert(*address);
  }
  StoreBE32(renut::GuestPtr(base, out_address), *address);
  return 0;
}
}  // namespace

// (flags, size, out pointer)
extern "C" void renut_imp_XamAlloc(PPCContext& __restrict ctx, uint8_t* base) {
  if (!UsesWorkspace(ctx.r3.u32)) return __imp__XamAlloc(ctx, base);
  ctx.r3.u64 = AllocWorkspaceBlock(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, base);
}

// (attributes, flags, size, out pointer): the physical flavour is the SDK's.
extern "C" void renut_imp_XamAllocEx(PPCContext& __restrict ctx, uint8_t* base) {
  if ((ctx.r4.u32 & kXamAllocPhysical) || !UsesWorkspace(ctx.r4.u32)) return __imp__XamAllocEx(ctx, base);
  ctx.r3.u64 = AllocWorkspaceBlock(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, base);
}

extern "C" void renut_imp_XamFree(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t address = ctx.r3.u32;
  bool ours = false;
  {
    std::lock_guard lock(g_workspace_blocks_mutex);
    ours = g_workspace_blocks.erase(address) != 0;
  }
  if (!ours) return __imp__XamFree(ctx, base);
  if (auto* title = renut::host::Title()) title->GetGuestWorkspace().Free(address);
}

// XAudioRegisterRenderDriverClient(callback, driver*) and XAudioUnregisterRenderDriverClient(driver). On the
// console, unregistering does not return while the client's callback is running, and Banjo relies on it: its
// audio engine's destructor (IDB 0x82666C80) unregisters the client, then sets the engine event 0x82FA7C3C once
// and waits for its mixer leader thread (sub_82664508) to end. The callback (sub_82664B70) stores its thread at
// engine+0x12C while it runs, sets that same event to ask the leader for a frame, and clears 0x12C before it
// returns; the leader ends only when it wakes and finds 0x12C clear. The SDK's unregister returns at once, so a
// callback still running took the destructor's wake-up for a frame request: the leader went back to waiting and
// the destructor waited for it for ever (the first 3D scene, or the next load, froze with the sound cut). (The
// leftover signal the next engine's leader would trip on is game_fixes.cpp's, at sub_82664680.)
//
// The callback is counted while it runs. Unregistering first marks the client closing, so a callback the SDK's
// worker had already picked up before the unregister returns without running the title's code, then waits for
// the one running to finish. Banjo has one render client.
namespace {
std::mutex g_render_mutex;
std::condition_variable g_render_done;
std::atomic<int> g_render_running{0};
std::atomic<bool> g_render_closing{false};
thread_local bool t_in_render_callback = false;
}  // namespace

extern "C" void renut_imp_XAudioRegisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t* base) {
  g_render_closing.store(false);
  __imp__XAudioRegisterRenderDriverClient(ctx, base);
}

extern "C" void renut_imp_XAudioUnregisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t* base) {
  g_render_closing.store(true);
  __imp__XAudioUnregisterRenderDriverClient(ctx, base);
  if (t_in_render_callback) return;  // from inside the callback itself: it cannot wait for itself
  std::unique_lock lock(g_render_mutex);
  // Bounded, as the old audio output was, in case the caller holds something the callback is blocked on.
  if (!g_render_done.wait_for(lock, std::chrono::seconds(1), [] { return g_render_running.load() == 0; }))
    REXLOG_CAT_WARN(rex::log::renut, "Renut: the audio render callback was still running 1 s after its client was unregistered");
}

// The title's audio render callback (registered by its audio engine; IDB 0x82667770).
extern "C" void __imp__sub_82664B70(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82664B70) {
  renut::audio::OnRenderCallback();
  g_render_running.fetch_add(1);
  if (!g_render_closing.load()) {
    t_in_render_callback = true;
    __imp__sub_82664B70(ctx, base);
    t_in_render_callback = false;
  }
  if (g_render_running.fetch_sub(1) == 1) {
    std::lock_guard lock(g_render_mutex);
    g_render_done.notify_all();
  }
}

// The video export that carries the graphics interrupt callback (xboxkrnl_video.cpp). The app's GPU driver
// (plume_gpu.h) needs the callback's user data, the device the vertical blank updates; the kernel keeps the
// callback itself for the interrupts the driver raises.
// (The ring buffer exports are not overridden: the ring is a stub, and the only code that
// called them, D3DDevice_SetRingBufferParameters, no longer runs.)
// (callback, user data)
extern "C" void renut_imp_VdSetGraphicsInterruptCallback(PPCContext& __restrict ctx, uint8_t* base) {
  renut::PlumeGpuSetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
  __imp__VdSetGraphicsInterruptCallback(ctx, base);  // the kernel's dispatcher runs it for RaiseGraphicsInterrupt
}

// The player's pause (host_support.h): a guest thread parks here, at the point
// where it would have waited, delayed or yielded anyway, and carries on into
// the SDK's own function when the pause is released. Parking a thread anywhere
// else could leave it holding the allocator or a kernel lock that the threads
// still running need.
#define RENUT_PAUSE_POINT(name)                                                    extern "C" void renut_imp_##name(PPCContext& __restrict ctx, uint8_t* base) {        renut::WaitIfTitlePaused();                                                        __imp__##name(ctx, base);                                                        }
RENUT_PAUSE_POINT(KeDelayExecutionThread)
RENUT_PAUSE_POINT(NtYieldExecution)
RENUT_PAUSE_POINT(KeWaitForSingleObject)
RENUT_PAUSE_POINT(NtWaitForSingleObjectEx)
RENUT_PAUSE_POINT(KeWaitForMultipleObjects)
RENUT_PAUSE_POINT(NtWaitForMultipleObjectsEx)
RENUT_PAUSE_POINT(NtSignalAndWaitForSingleObjectEx)
#undef RENUT_PAUSE_POINT
