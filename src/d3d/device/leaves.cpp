// Small D3D internals, natively (see leaves.h). Transcribed from the retail executable's
// PowerPC with idalib; the comments name the registers the original used.
#include "d3d/device/leaves.h"

#include <chrono>
#include <cstring>

#include "d3d/hooks/layer.h"
#include "d3d/device/internal.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

extern "C" {
__attribute__((weak)) void sub_82D08B7C(PPCContext& __restrict, uint8_t*);  // the video flags
}

namespace renut::d3d::port {
namespace {

// D3D::FitScalerParameters(device, width, height, const Scaler* in, Scaler* out)
// [0x8265F3B0]: `in` copied to `out` with its source rectangle (the whole source when
// empty) and the destination size filled in: the width as asked or the device's, the
// height from the aspect ratio unless it does not fit.
bool FitScalerParameters(PPCContext& ctx, uint8_t* base) {
  if (!sub_82D08B7C)
    return false;
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  u8* in = renut::GuestPtr(base, ctx.r6.u32);
  u8* out = renut::GuestPtr(base, ctx.r7.u32);
  u32 rect[4];
  for (u32 i = 0; i < 4; ++i)
    rect[i] = LoadBE32(in + 4 * i);
  if (!rect[0] && !rect[1] && !rect[2] && !rect[3]) {
    rect[2] = ctx.r4.u32;
    rect[3] = ctx.r5.u32;
  }
  const u32 source_width = rect[2] - rect[0];   // r10
  const u32 source_height = rect[3] - rect[1];  // r29
  u32 width = LoadBE32(in + 0x10);              // r28
  if (width == 0)
    width = d.W(0x5428);
  u32 height = LoadBE32(in + 0x14);  // r30
  if (height == 0) {
    // divwu, which the original follows with a trap on a zero divisor.
    height = source_width ? (width * source_height) / source_width : 0;
    bool use_maximum = (d.W(0x3514) & 1) != 0;
    if (!use_maximum) {
      PPCContext call = ctx;
      sub_82D08B7C(call, base);
      use_maximum = (call.r3.u32 & 1) != 0;
    }
    if (use_maximum || !source_width || height > source_height || height > d.W(0x542C))
      height = d.W(0x542C);
  }
  std::memmove(out, in, 0x38);
  StoreBE32(out + 0x10, width);
  StoreBE32(out + 0x14, height);
  for (u32 i = 0; i < 4; ++i)
    StoreBE32(out + 4 * i, rect[i]);
  return true;
}

}  // namespace

void RegisterLeaves() {
  SetHandler(kD3D_FitScalerParameters, FitScalerParameters);
  RegisterCommandLists();
}

}  // namespace renut::d3d::port
