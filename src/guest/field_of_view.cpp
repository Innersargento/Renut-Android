// The field of view option (renut_field_of_view): the game's vertical field of view, scaled from 100% (its own)
// to 150%. Ported from reNut's for the PC (src/renut_engine/game/field_of_view.cpp of masterspike52/reNut).
// sub_822150D0 turns the camera's lens into the vertical field of view in degrees; projection, culling and
// level of detail all read what it writes, so they stay in agreement. It runs for every view, so a change
// reaches the next frame.

#include <algorithm>
#include <bit>
#include <cstring>

#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "guest/guest_address.h"
#include "platform/settings.h"

namespace {

// The widest vertical field of view the projection gets, in degrees.
constexpr float kMaxFieldOfView = 150.0f;

uint32_t LoadU32(uint8_t* base, uint32_t va) {
  uint32_t value;
  std::memcpy(&value, renut::GuestPtr(base, va), sizeof(value));
  return __builtin_bswap32(value);
}

void StoreU32(uint8_t* base, uint32_t va, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(renut::GuestPtr(base, va), &value, sizeof(value));
}

}  // namespace

// void sub_822150D0(int lens, float* fov_degrees, double focal) (PC sub_821E4540).
extern "C" void __imp__sub_822150D0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_822150D0) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_822150D0(ctx, base);
  const int32_t percent = std::clamp<int32_t>(renut::GetSettings().field_of_view.Get(), 100, 150);
  if (!out || percent == 100) return;
  const float fov = std::bit_cast<float>(LoadU32(base, out));
  if (!(fov > 0.0f)) return;
  const float wider = std::min(fov * float(percent) / 100.0f, std::max(fov, kMaxFieldOfView));
  StoreU32(base, out, std::bit_cast<uint32_t>(wider));
}
