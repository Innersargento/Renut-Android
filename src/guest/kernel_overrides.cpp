// Replacements for the SDK's kernel exports that reNut needs to behave
// differently. The SDK is used exactly as released: the recompiled game calls
// an import by a symbol name (`__imp__<Export>`), and the build compiles the
// game's code with that name replaced by `renut_imp_<Export>` for each export
// listed in kernel_overrides.inc. What is defined here is therefore what the
// game calls; the SDK's own function stays reachable under its real name for
// whatever the override does not take over.
#include <cstdint>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/chrono/clock.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

#include "guest_address.h"
#include "host_support.h"
#include "d3d/native_d3d.h"
#include "plume/plume_gpu.h"
#if defined(__ANDROID__)
#include "platform/android/renut_audio.h"
#endif

// The SDK's X_STATUS_* macros are written with the unqualified type.
using rex::X_STATUS;

#define X(name) extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);
#include "kernel_overrides.inc"
#undef X

namespace {

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

// VdSwap(buffer, fetch, unk2, unk3, unk4, frontbuffer, format, color space,
// width, height): the game's present. With native sync the frame goes into the
// swapping device's list, and the ring gets 64 words of no-ops where the swap
// packet would have been. The rest is the SDK's.
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
    const uint32_t physical = rex::system::kernel_memory()->GetPhysicalAddress(virtual_base);
    const uint32_t width = LoadBE32(renut::GuestPtr(base, width_address));
    const uint32_t height = LoadBE32(renut::GuestPtr(base, height_address));
    if (physical != UINT32_MAX && renut::native_d3d::RecordPresent(physical, width, height)) {
      uint8_t* words = renut::GuestPtr(base, buffer_address);
      for (uint32_t i = 0; i < 64; ++i) {
        // A type-2 packet (a no-op), big-endian.
        words[i * 4 + 0] = 0x80;
        words[i * 4 + 1] = 0;
        words[i * 4 + 2] = 0;
        words[i * 4 + 3] = 0;
      }
      return;
    }
  }
  __imp__VdSwap(ctx, base);
}

// The video exports the SDK routes to its graphics system (xboxkrnl_video.cpp:
// the interrupt callback, the ring buffer and its read pointer write-back). The
// Runtime has none, since the app hosts the GPU (plume_gpu.h): these are the
// app's, and the SDK's own functions are not reached.
// (callback, user data)
extern "C" void renut_imp_VdSetGraphicsInterruptCallback(PPCContext& __restrict ctx, uint8_t*) {
  renut::PlumeGpuSetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

// (physical address, log2 of the size)
extern "C" void renut_imp_VdInitializeRingBuffer(PPCContext& __restrict ctx, uint8_t*) {
  renut::PlumeGpuInitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// (address, log2 of the block size)
extern "C" void renut_imp_VdEnableRingBufferRPtrWriteBack(PPCContext& __restrict ctx, uint8_t*) {
  renut::PlumeGpuEnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

#if defined(__ANDROID__)
// The XAudio render-client exports: reNut's own audio output (renut_audio.cpp)
// owns the clients, so the title's registrations, frames and shutdown reach it
// directly. The title's handle is 0x4155xxxx; the SDK only asserts on a wrong
// one and its asserts are compiled out, and Banjo does pass a stray value while
// it re-registers its render client, so a handle that is not ours is a no-op.
extern "C" void renut_imp_XAudioRegisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t*) {
  ctx.r3.u64 = renut::audio::RegisterRenderClient(ctx.r3.u32, ctx.r4.u32);
}

extern "C" void renut_imp_XAudioUnregisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t*) {
  ctx.r3.u64 = renut::audio::UnregisterRenderClient(ctx.r3.u32);
}

extern "C" void renut_imp_XAudioSubmitRenderDriverFrame(PPCContext& __restrict ctx, uint8_t*) {
  ctx.r3.u64 = renut::audio::SubmitRenderFrame(ctx.r3.u32, ctx.r4.u32);
}

// (driver, out): which voice categories changed volume. None ever do. The SDK
// sleeps a microsecond per call because a title polls this in a loop.
extern "C" void renut_imp_XAudioGetVoiceCategoryVolumeChangeMask(PPCContext& __restrict ctx, uint8_t* base) {
  if (const uint32_t out = ctx.r4.u32) {
    uint8_t* word = renut::GuestPtr(base, out);
    word[0] = word[1] = word[2] = word[3] = 0;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(1));
  ctx.r3.u64 = 0;
}
#else
// Platforms with their own audio output or text entry take the SDK's exports.
#define RENUT_PASS_THROUGH(name)                                                     extern "C" void renut_imp_##name(PPCContext& __restrict ctx, uint8_t* base) {        __imp__##name(ctx, base);                                                        }
RENUT_PASS_THROUGH(XAudioRegisterRenderDriverClient)
RENUT_PASS_THROUGH(XAudioUnregisterRenderDriverClient)
RENUT_PASS_THROUGH(XAudioSubmitRenderDriverFrame)
RENUT_PASS_THROUGH(XAudioGetVoiceCategoryVolumeChangeMask)
RENUT_PASS_THROUGH(XamShowKeyboardUI)  // Android's is in text_input_bridge.cpp
RENUT_PASS_THROUGH(XamUserGetXUID)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserGetSigninState)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserGetSigninInfo)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserGetName)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserGetGamerTag)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserReadProfileSettings)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserReadProfileSettingsEx)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserWriteProfileSettings)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserCheckPrivilege)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserContentRestrictionGetFlags)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserContentRestrictionGetRating)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserGetMembershipTier)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserAreUsersFriends)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamShowSigninUI)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamInputGetState)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamInputGetKeystroke)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamInputGetKeystrokeEx)  // Android's are in xam_users.cpp
RENUT_PASS_THROUGH(XamUserGetDeviceContext)  // Android's are in xam_users.cpp
#undef RENUT_PASS_THROUGH
#endif

// The player's pause (host_support.h): a guest thread parks here, at the point
// where it would have waited, delayed or yielded anyway, and carries on into
// the SDK's own function when the pause is released. Parking a thread anywhere
// else could leave it holding the allocator or a kernel lock that the threads
// still running need.
#define RENUT_PAUSE_POINT(name, index)                                             \
  extern "C" void renut_imp_##name(PPCContext& __restrict ctx, uint8_t* base) {    \
    renut::WaitIfTitlePaused();                                                    \
    if (renut::IsWaitProbeThread()) {                                              \
      const uint32_t object = ctx.r3.u32, caller = uint32_t(ctx.lr);               \
      const auto started = std::chrono::steady_clock::now();                       \
      __imp__##name(ctx, base);                                                    \
      renut::NoteProbedWait(                                                       \
          index, object, caller,                                                   \
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(           \
                       std::chrono::steady_clock::now() - started).count()));      \
      return;                                                                      \
    }                                                                              \
    __imp__##name(ctx, base);                                                      \
  }
RENUT_PAUSE_POINT(KeDelayExecutionThread, 1)
RENUT_PAUSE_POINT(NtYieldExecution, 2)
RENUT_PAUSE_POINT(KeWaitForSingleObject, 3)
RENUT_PAUSE_POINT(NtWaitForSingleObjectEx, 4)
RENUT_PAUSE_POINT(NtWaitForMultipleObjectsEx, 6)
RENUT_PAUSE_POINT(NtSignalAndWaitForSingleObjectEx, 7)
#undef RENUT_PAUSE_POINT

// KeInitializeEvent(event, type, initial_state). The SDK zeroes the KEVENT, which
// drops the handle it stashed in the header; the next use then builds a second
// native event, and a thread still blocked on the first one is never woken.
// Banjo initialises its static XAudio events again while the render thread
// waits on them, and then deadlocks joining that thread. The guest identifies an
// event by its address, so an event that is already backed by a native object of
// the same kind keeps it, and only has its state reset.
extern "C" void renut_imp_KeInitializeEvent(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace rex::system;
  const uint32_t event_type = ctx.r4.u32;
  const uint32_t initial_state = ctx.r5.u32;
  auto* event = reinterpret_cast<X_KEVENT*>(renut::GuestPtr(base, ctx.r3.u32));
  X_DISPATCH_HEADER& header = event->header;
  if (header.wait_list_flink == kXObjSignature && header.type == event_type) {
    if (auto existing = XObject::GetNativeObject<XEvent>(kernel_state(), event, event_type);
        existing && existing->type() == XObject::Type::Event) {
      const uint32_t handle = header.wait_list_blink;
      std::memset(event, 0, sizeof(*event));
      header.type = event_type;
      header.signal_state = initial_state;
      header.wait_list_flink = kXObjSignature;
      header.wait_list_blink = handle;
      if (initial_state) {
        existing->Set(0, false);
      } else {
        existing->Reset();
      }
      return;
    }
  }
  __imp__KeInitializeEvent(ctx, base);
}

// KeWaitForMultipleObjects(count, objects, wait_type, reason, mode, alertable, timeout, block).
//
// The SDK waits on several objects by polling them: every millisecond it takes
// all their locks, looks, and sleeps, so a wait that is satisfied after 100 us
// returns after 1 ms and more, and a sleep that overshoots under load returns
// after 3-5 ms. The audio render callback of the title makes one of these waits
// for every 5.33 ms frame it mixes (measured: 1.2 ms when idle, up to 4.9 ms
// while loading, on a frame whose whole budget is 5.33), which is what made the
// output starve into clicks.
//
// A wait for any one of a few objects does not need that: the object that
// satisfied this call site last time is waited on for real, on its condition
// variable, in slices of a millisecond, and the others are looked at between the
// slices. The common case is woken the instant it is signalled; the others are
// noticed within a slice, which is what polling gave them anyway. Everything
// else (wait for all, more than four objects) goes to the SDK.
extern "C" void renut_imp_KeWaitForMultipleObjects(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace rex::system;
  renut::WaitIfTitlePaused();
  const uint32_t count = ctx.r3.u32, objects_address = ctx.r4.u32, wait_type = ctx.r5.u32;
  const uint32_t wait_reason = ctx.r6.u32, processor_mode = ctx.r7.u32, alertable = ctx.r8.u32;
  const uint32_t timeout_address = ctx.r9.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  auto* kernel = kernel_state();
  if (wait_type != 1 || count < 2 || count > 4 || !objects_address || !kernel) {
    __imp__KeWaitForMultipleObjects(ctx, base);
    return;
  }

  object_ref<XObject> objects[4];
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t object_address = LoadBE32(renut::GuestPtr(base, objects_address + i * 4));
    objects[i] = XObject::GetNativeObject<XObject>(kernel, renut::GuestPtr(base, object_address));
    if (!objects[i]) {
      ctx.r3.u64 = uint32_t(X_STATUS_INVALID_PARAMETER);
      return;
    }
  }

  // The timeout, as the SDK reads it: guest ticks (100 ns, negative = relative),
  // scaled by the guest clock. No pointer: wait for ever.
  bool finite = false;
  uint32_t total_ms = 0;
  if (timeout_address) {
    const uint8_t* word = renut::GuestPtr(base, timeout_address);
    uint64_t ticks = 0;
    for (int i = 0; i < 8; ++i) ticks = ticks << 8 | word[i];
    finite = true;
    // Relative timeouts are negative; the SDK takes an absolute one (positive) as zero.
    const int64_t relative = int64_t(ticks);
    total_ms = rex::chrono::Clock::ScaleGuestDurationMillis(relative < 0 ? uint32_t(-relative / 10000) : 0u);
  }

  // Which object ended this call site's last waits (a few sites, per thread).
  struct Hot {
    uint32_t caller;
    uint32_t index;
  };
  thread_local Hot hot_sites[8];
  thread_local uint32_t hot_count = 0;
  Hot* hot = nullptr;
  for (uint32_t i = 0; i < hot_count; ++i) {
    if (hot_sites[i].caller == caller) hot = &hot_sites[i];
  }
  if (!hot && hot_count < 8) {
    hot = &hot_sites[hot_count++];
    hot->caller = caller;
    hot->index = 0;
  }
  uint32_t first = hot ? std::min(hot->index, count - 1) : 0;

  if (renut::IsWaitProbeThread()) {
    // Whoever signals these is the thread this one is waiting for.
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t address = LoadBE32(renut::GuestPtr(base, objects_address + i * 4));
      // The dispatcher header's type byte (0/1 event, 5 semaphore, 2 mutant, 8/9
      // timer) is noted the first time an address shows up.
      if (renut::WatchSignalObject(i, address)) {
        renut::NoteProbedWait(20 + (*renut::GuestPtr(base, address) & 0x1F), address, i, 0);
      }
    }
  }

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::milliseconds(total_ms);
  X_STATUS status = X_STATUS_TIMEOUT;
  uint32_t satisfied = count;  // none
  for (;;) {
    // Wait for real on the likely one, for at most a millisecond.
    uint64_t slice = (finite && total_ms == 0) ? 0 : uint64_t(int64_t(-10000));
    status = objects[first]->Wait(wait_reason, processor_mode, alertable, &slice);
    if (status == X_STATUS_SUCCESS) {
      satisfied = first;
      break;
    }
    if (status != X_STATUS_TIMEOUT) break;  // APC, abandoned, failure: as the SDK would say
    // Then look at the others, without waiting.
    bool done = false;
    for (uint32_t i = 0; i < count && !done; ++i) {
      if (i == first) continue;
      uint64_t now_only = 0;
      const X_STATUS other = objects[i]->Wait(wait_reason, processor_mode, 0, &now_only);
      if (other == X_STATUS_SUCCESS) {
        satisfied = i;
        status = X_STATUS_SUCCESS;
        done = true;
      } else if (other != X_STATUS_TIMEOUT) {
        status = other;
        done = true;
      }
    }
    if (done) break;
    if (finite && std::chrono::steady_clock::now() >= deadline) {
      status = X_STATUS_TIMEOUT;
      break;
    }
  }

  if (status == X_STATUS_SUCCESS && satisfied < count) {
    if (hot) hot->index = satisfied;
    status = X_STATUS(satisfied);  // STATUS_WAIT_0 + index
  } else if (alertable && status == X_STATUS_USER_APC) {
    XThread::GetCurrentThread()->DeliverAPCs();
  }
  if (renut::IsWaitProbeThread()) {
    renut::NoteProbedWait(5, count, caller,
                          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started).count()));
  }
  ctx.r3.u64 = uint32_t(status);
}

// KeSetEvent(event, increment, wait) and KeReleaseSemaphore(semaphore, increment,
// adjustment, wait): the thread that signals what the audio worker waits on is
// the title's mixer (host_support.h: NoteSignal).
extern "C" void renut_imp_KeSetEvent(PPCContext& __restrict ctx, uint8_t* base) {
  renut::NoteSignal(ctx.r3.u32);
  __imp__KeSetEvent(ctx, base);
}

extern "C" void renut_imp_KeReleaseSemaphore(PPCContext& __restrict ctx, uint8_t* base) {
  renut::NoteSignal(ctx.r3.u32);
  __imp__KeReleaseSemaphore(ctx, base);
}

// NtSetEvent(handle, previous*) and NtReleaseSemaphore(handle, count, previous*): the
// same, through the handle (the kernel object's own address is what is watched).
namespace {
void NoteHandleSignal(uint32_t handle) {
  if (!renut::AnyWatched()) return;
  if (auto* kernel = rex::system::kernel_state()) {
    if (auto object = kernel->object_table()->LookupObject<rex::system::XObject>(handle)) {
      renut::NoteSignal(object->guest_object());
    }
  }
}
}  // namespace

extern "C" void renut_imp_NtSetEvent(PPCContext& __restrict ctx, uint8_t* base) {
  NoteHandleSignal(ctx.r3.u32);
  __imp__NtSetEvent(ctx, base);
}

extern "C" void renut_imp_NtReleaseSemaphore(PPCContext& __restrict ctx, uint8_t* base) {
  NoteHandleSignal(ctx.r3.u32);
  __imp__NtReleaseSemaphore(ctx, base);
}
