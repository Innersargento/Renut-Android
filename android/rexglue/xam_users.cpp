// Local multiplayer: a second gamepad is a second signed-in guest user, for as long
// as it is attached (touch_input.h: PlayerAttached). The SDK knows one user, user 0:
// its XamUser* exports answer "no such user" for any other index and its input
// exports pin "any user" to user 0. These replace those exports (the build renames
// the game's calls, kernel_overrides.cmake) and hand everything that is about user 0
// to the SDK's own function.
//
// There is one stored profile, user 0's. The other users are generated guests that
// read the same settings, get xuid + index and the name "Player N", and whose
// profile writes are accepted and dropped, so that a guest never replaces the real
// player's settings.
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/input/input_system.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include "guest_address.h"
#include "touch_input.h"

#define X(name) extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);
#include "kernel_overrides.inc"
#undef X

using rex::X_RESULT;
using rex::X_STATUS;
using rex::X_HRESULT;

namespace {

constexpr uint32_t kXInputFlagGamepad = 0x01;
constexpr uint32_t kXInputFlagAnyUser = 1u << 30;
constexpr uint32_t kUsers = rex::input::kMaxGuestUsers;

// User 0 (the touch controller) is always there; the others exist while a pad is attached.
bool UserConnected(uint32_t user) { return user == 0 || (user < kUsers && renut::PlayerAttached(int(user))); }

// A guest user the SDK would not know: attached, and not user 0.
bool IsGuest(uint32_t user) { return user != 0 && user < kUsers && renut::PlayerAttached(int(user)); }

rex::input::InputSystem* Input() {
  auto* kernel = rex::system::kernel_state();
  return kernel && kernel->emulator()
             ? static_cast<rex::input::InputSystem*>(kernel->emulator()->input_system())
             : nullptr;
}

uint64_t LoadBE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
  return v;
}
void StoreBE64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = uint8_t(v);
    v >>= 8;
  }
}
void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

std::string GuestName(uint32_t user) { return "Player " + std::to_string(user + 1); }

// XN_SYS_SIGNINCHANGED carries one bit per user that is signed in.
uint32_t SignedInMask() {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kUsers; ++i) {
    if (UserConnected(i)) mask |= 1u << i;
  }
  return mask;
}

// For the exports whose only difference for a guest is the user index: the SDK
// answers them as it would for user 0, the one profile there is.
void AsUserZero(uint64_t& argument, uint32_t user) {
  if (IsGuest(user)) argument = 0;
}

}  // namespace

// ---- XamUser* ----

// XamUserGetXUID(user, type_mask, xuid*): the guest's xuid is user 0's plus its index.
extern "C" void renut_imp_XamUserGetXUID(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t xuid_address = ctx.r5.u32;
  const bool guest = IsGuest(user);
  if (guest) ctx.r3.u64 = 0;
  __imp__XamUserGetXUID(ctx, base);
  if (guest && xuid_address && uint32_t(ctx.r3.u32) == uint32_t(X_E_SUCCESS)) {
    uint8_t* word = renut::GuestPtr(base, xuid_address);
    StoreBE64(word, LoadBE64(word) + user);
  }
}

extern "C" void renut_imp_XamUserGetSigninState(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r3.u64, ctx.r3.u32);
  __imp__XamUserGetSigninState(ctx, base);
}

// XamUserGetSigninInfo(user, flags, info*): X_USER_SIGNIN_INFO is xuid (8), two
// unknown words, signin_state (4 at +8), two more, then char name[16] at +24.
extern "C" void renut_imp_XamUserGetSigninInfo(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t info_address = ctx.r5.u32;
  const bool guest = IsGuest(user);
  if (guest) ctx.r3.u64 = 0;
  __imp__XamUserGetSigninInfo(ctx, base);
  if (guest && info_address && uint32_t(ctx.r3.u32) == uint32_t(X_E_SUCCESS)) {
    uint8_t* info = renut::GuestPtr(base, info_address);
    StoreBE64(info, LoadBE64(info) + user);
    char* name = reinterpret_cast<char*>(info + 24);
    std::memset(name, 0, 16);
    const std::string text = GuestName(user);
    std::memcpy(name, text.data(), std::min<size_t>(text.size(), 15));
  }
}

// XamUserGetName(user, char* buffer, length)
extern "C" void renut_imp_XamUserGetName(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32;
  if (IsGuest(user)) {
    const uint32_t buffer_address = ctx.r4.u32, length = ctx.r5.u32;
    if (!buffer_address || !length) {
      ctx.r3.u64 = uint32_t(X_E_INVALIDARG);
      return;
    }
    char* buffer = reinterpret_cast<char*>(renut::GuestPtr(base, buffer_address));
    const std::string text = GuestName(user);
    const size_t room = std::min<size_t>(length, 16);
    const size_t count = std::min(text.size(), room ? room - 1 : 0);
    std::memcpy(buffer, text.data(), count);
    if (room) buffer[count] = 0;
    ctx.r3.u64 = uint32_t(X_E_SUCCESS);
    return;
  }
  __imp__XamUserGetName(ctx, base);
}

// XamUserGetGamerTag(user, wchar_t* buffer, length): big-endian UTF-16.
extern "C" void renut_imp_XamUserGetGamerTag(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32;
  if (IsGuest(user)) {
    const uint32_t buffer_address = ctx.r4.u32, length = ctx.r5.u32;
    if (!buffer_address || length < 16) {
      ctx.r3.u64 = uint32_t(X_E_INVALIDARG);
      return;
    }
    uint8_t* buffer = renut::GuestPtr(base, buffer_address);
    const std::string text = GuestName(user);
    const size_t count = std::min<size_t>(text.size(), std::min<size_t>(length, 16) - 1);
    for (size_t i = 0; i < count; ++i) {
      buffer[i * 2] = 0;
      buffer[i * 2 + 1] = uint8_t(text[i]);
    }
    buffer[count * 2] = buffer[count * 2 + 1] = 0;
    ctx.r3.u64 = uint32_t(X_E_SUCCESS);
    return;
  }
  __imp__XamUserGetGamerTag(ctx, base);
}

// (title_id, user, ...): a guest reads the settings of the one stored profile.
extern "C" void renut_imp_XamUserReadProfileSettings(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r4.u64, ctx.r4.u32);
  __imp__XamUserReadProfileSettings(ctx, base);
}
extern "C" void renut_imp_XamUserReadProfileSettingsEx(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r4.u64, ctx.r4.u32);
  __imp__XamUserReadProfileSettingsEx(ctx, base);
}

// XamUserWriteProfileSettings(title_id, user, count, settings, overlapped): a
// guest is told the write worked and nothing is kept.
extern "C" void renut_imp_XamUserWriteProfileSettings(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r4.u32;
  if (IsGuest(user)) {
    const uint32_t overlapped = ctx.r7.u32;
    auto* kernel = rex::system::kernel_state();
    if (overlapped && kernel) {
      kernel->CompleteOverlappedImmediate(overlapped, X_ERROR_SUCCESS);
      ctx.r3.u64 = uint32_t(X_ERROR_IO_PENDING);
    } else {
      ctx.r3.u64 = uint32_t(X_ERROR_SUCCESS);
    }
    return;
  }
  __imp__XamUserWriteProfileSettings(ctx, base);
}

extern "C" void renut_imp_XamUserCheckPrivilege(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r3.u64, ctx.r3.u32);
  __imp__XamUserCheckPrivilege(ctx, base);
}
extern "C" void renut_imp_XamUserContentRestrictionGetFlags(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r3.u64, ctx.r3.u32);
  __imp__XamUserContentRestrictionGetFlags(ctx, base);
}
extern "C" void renut_imp_XamUserContentRestrictionGetRating(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r3.u64, ctx.r3.u32);
  __imp__XamUserContentRestrictionGetRating(ctx, base);
}
extern "C" void renut_imp_XamUserGetMembershipTier(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r3.u64, ctx.r3.u32);
  __imp__XamUserGetMembershipTier(ctx, base);
}
extern "C" void renut_imp_XamUserAreUsersFriends(PPCContext& __restrict ctx, uint8_t* base) {
  AsUserZero(ctx.r3.u64, ctx.r3.u32);
  __imp__XamUserAreUsersFriends(ctx, base);
}

// The sign-in UI is answered without showing anything: the sign-in notification
// carries every user that is signed in (the SDK's carries user 0 alone), and the
// UI notification says it is gone. Games sit and loop until they see both.
extern "C" void renut_imp_XamShowSigninUI(PPCContext& __restrict ctx, uint8_t*) {
  if (auto* kernel = rex::system::kernel_state()) {
    kernel->BroadcastNotification(0x0000000A, SignedInMask());  // XN_SYS_SIGNINCHANGED
    kernel->BroadcastNotification(0x00000009, 0);                // XN_SYS_UI (off)
  }
  ctx.r3.u64 = uint32_t(X_ERROR_SUCCESS);
}

// ---- XamInput* ----

// XamInputGetState(user, flags, state*): "any user" is the first pad that is
// attached (user 0 always is), as on the console, not user 0 alone.
extern "C" void renut_imp_XamInputGetState(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32;
  if ((flags & 0xFF) && !(flags & kXInputFlagGamepad)) {
    ctx.r3.u64 = uint32_t(X_ERROR_DEVICE_NOT_CONNECTED);
    return;
  }
  auto* input = Input();
  if (input && ((user & 0xFF) == 0xFF || (flags & kXInputFlagAnyUser))) {
    auto* state = ctx.r5.u32 ? reinterpret_cast<rex::input::X_INPUT_STATE*>(renut::GuestPtr(base, ctx.r5.u32))
                             : nullptr;
    for (uint32_t u = 0; u < kUsers; ++u) {
      if (input->GetState(u, state) == X_ERROR_SUCCESS) {
        ctx.r3.u64 = uint32_t(X_ERROR_SUCCESS);
        return;
      }
    }
    ctx.r3.u64 = uint32_t(X_ERROR_DEVICE_NOT_CONNECTED);
    return;
  }
  __imp__XamInputGetState(ctx, base);
}

namespace {

// XUSER_INDEX_ANY for keystrokes: the first user with a pending one, as on the
// console. The title reads which user it came from out of the result.
X_RESULT KeystrokeFromAnyUser(rex::input::InputSystem* input, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* keystroke) {
  X_RESULT first_error = X_ERROR_DEVICE_NOT_CONNECTED;
  for (uint32_t u = 0; u < kUsers; ++u) {
    const X_RESULT result = input->GetKeystroke(u, flags, keystroke);
    if (result == X_ERROR_SUCCESS) return result;
    if (result == X_ERROR_EMPTY) first_error = result;
  }
  return first_error;
}

}  // namespace

// XamInputGetKeystroke(user, flags, keystroke*)
extern "C" void renut_imp_XamInputGetKeystroke(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, flags = ctx.r4.u32;
  auto* input = Input();
  if (input && ctx.r5.u32 && !((flags & 0xFF) && !(flags & kXInputFlagGamepad)) &&
      ((user & 0xFF) == 0xFF || (flags & kXInputFlagAnyUser))) {
    auto* keystroke = reinterpret_cast<rex::input::X_INPUT_KEYSTROKE*>(renut::GuestPtr(base, ctx.r5.u32));
    ctx.r3.u64 = uint32_t(KeystrokeFromAnyUser(input, flags, keystroke));
    return;
  }
  __imp__XamInputGetKeystroke(ctx, base);
}

// XamInputGetKeystrokeEx(user*, flags, keystroke*): the same, and the user the
// keystroke came from is written back through the pointer.
extern "C" void renut_imp_XamInputGetKeystrokeEx(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user_address = ctx.r3.u32, flags = ctx.r4.u32;
  auto* input = Input();
  if (input && user_address && ctx.r5.u32 && !((flags & 0xFF) && !(flags & kXInputFlagGamepad))) {
    uint8_t* user_word = renut::GuestPtr(base, user_address);
    const uint32_t user = uint32_t(user_word[0]) << 24 | uint32_t(user_word[1]) << 16 |
                          uint32_t(user_word[2]) << 8 | user_word[3];
    auto* keystroke = reinterpret_cast<rex::input::X_INPUT_KEYSTROKE*>(renut::GuestPtr(base, ctx.r5.u32));
    X_RESULT result;
    if ((user & 0xFF) == 0xFF || (flags & kXInputFlagAnyUser)) {
      result = KeystrokeFromAnyUser(input, flags, keystroke);
    } else {
      result = input->GetKeystroke(user, flags, keystroke);
    }
    if (result == X_ERROR_SUCCESS) StoreBE32(user_word, keystroke->user_index);
    ctx.r3.u64 = uint32_t(result);
    return;
  }
  __imp__XamInputGetKeystrokeEx(ctx, base);
}

// XamUserGetDeviceContext(user, unk, out*): titles check the result, and assume
// zero when it fails.
extern "C" void renut_imp_XamUserGetDeviceContext(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, out_address = ctx.r5.u32;
  if (out_address) StoreBE32(renut::GuestPtr(base, out_address), 0);
  ctx.r3.u64 = uint32_t(((user & 0xFF) == 0xFF || UserConnected(user)) ? X_E_SUCCESS
                                                                          : X_E_DEVICE_NOT_CONNECTED);
}
