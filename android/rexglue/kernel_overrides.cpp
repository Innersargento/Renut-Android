// Replacements for the SDK's kernel exports that reNut needs to behave
// differently. The SDK is used exactly as released: the recompiled game calls
// an import by a symbol name (`__imp__<Export>`), and the build compiles the
// game's code with that name replaced by `renut_imp_<Export>` for each export
// listed in kernel_overrides.inc. What is defined here is therefore what the
// game calls; the SDK's own function stays reachable under its real name for
// whatever the override does not take over.
#include <cstdint>
#include <cstring>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "guest_address.h"
#include "native_d3d.h"

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
  const uint32_t width_address = ctx.r11.u32;
  const uint32_t height_address = ctx.r12.u32;
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
