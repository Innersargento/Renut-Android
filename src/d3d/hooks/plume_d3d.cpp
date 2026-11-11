// Direct3D handlers for the native Plume renderer.
//
// Handlers that mirror the clear colour, which the renderer shows while no frame
// has been resolved.
#include "d3d/hooks/layer.h"
#include "d3d/device/state.h"
#include "plume/plume_renderer.h"
#include "guest/guest_address.h"
#include "guest/host_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>

namespace renut::d3d {
namespace {

// Xbox 360 D3DCLEAR_ZBUFFER / D3DCLEAR_STENCIL; any other bit clears colour.
constexpr uint32_t kClearDepthStencil = 0x2 | 0x4;

float LoadBEFloat(const uint8_t* p) {
  uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

// Called by the native Clear and ClearF (scene/frame.cpp), which are the handlers of those calls.
// D3DDevice_Clear(device, count, rects, flags, D3DCOLOR color, z, stencil)
void MirrorClearColor(PPCContext& ctx) {
  if (auto* renderer = PlumeRenderer::Get(); renderer && (ctx.r6.u32 & ~kClearDepthStencil)) {
    uint32_t argb = ctx.r7.u32;
    renderer->SetClearColor(((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f,
                            (argb & 0xFF) / 255.f, (argb >> 24) / 255.f);
  }
}

// D3DDevice_ClearF(device, flags, rect, const D3DVECTOR4* color, z, stencil)
void MirrorClearColorF(PPCContext& ctx, uint8_t* base) {
  if (auto* renderer = PlumeRenderer::Get();
      renderer && (ctx.r4.u32 & ~kClearDepthStencil) && ctx.r6.u32) {
    const uint8_t* color = GuestPtr(base, ctx.r6.u32);
    renderer->SetClearColor(LoadBEFloat(color), LoadBEFloat(color + 4), LoadBEFloat(color + 8),
                            LoadBEFloat(color + 12));
  }
}

void RegisterPlumeHandlers() {
  // Layer 1a of the native Direct3D port: the state setters (d3d/device/).
  port::RegisterStateHandlers();
}

}  // namespace renut::d3d
