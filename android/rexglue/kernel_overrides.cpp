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

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xobject.h>

#include "guest_address.h"
#include "host_support.h"
#include "native_d3d.h"
#if defined(__ANDROID__)
#include "renut_audio.h"
#endif

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
#define RENUT_PAUSE_POINT(name)                                                      extern "C" void renut_imp_##name(PPCContext& __restrict ctx, uint8_t* base) {        renut::WaitIfTitlePaused();                                                        __imp__##name(ctx, base);                                                        }
RENUT_PAUSE_POINT(KeDelayExecutionThread)
RENUT_PAUSE_POINT(NtYieldExecution)
RENUT_PAUSE_POINT(KeWaitForSingleObject)
RENUT_PAUSE_POINT(NtWaitForSingleObjectEx)
RENUT_PAUSE_POINT(KeWaitForMultipleObjects)
RENUT_PAUSE_POINT(NtWaitForMultipleObjectsEx)
RENUT_PAUSE_POINT(NtSignalAndWaitForSingleObjectEx)
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
