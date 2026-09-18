// Direct3D handlers for renut_renderer = "plume".
//
// Stage 1 keeps every call flowing to the emulated Xenos as well, which runs
// without presentation, and mirrors only what the native renderer can already
// show: clear colours, presented on the game's own Swap.
#include "d3d_layer.h"
#include "plume_renderer.h"

#include <cstring>

#include <rex/ppc/func.h>

extern "C" void __imp__sub_82287590(PPCContext& __restrict, uint8_t*);  // D3DDevice_Swap

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

// D3DDevice_Clear(device, count, rects, flags, D3DCOLOR color, z, stencil)
bool Clear(PPCContext& ctx, uint8_t*) {
  if (auto* renderer = PlumeRenderer::Get(); renderer && (ctx.r6.u32 & ~kClearDepthStencil)) {
    uint32_t argb = ctx.r7.u32;
    renderer->SetClearColor(((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f,
                            (argb & 0xFF) / 255.f, (argb >> 24) / 255.f);
  }
  return false;
}

// D3DDevice_ClearF(device, flags, rect, const D3DVECTOR4* color, z, stencil)
bool ClearF(PPCContext& ctx, uint8_t* base) {
  if (auto* renderer = PlumeRenderer::Get();
      renderer && (ctx.r4.u32 & ~kClearDepthStencil) && ctx.r6.u32) {
    const uint8_t* color = base + ctx.r6.u32;
    renderer->SetClearColor(LoadBEFloat(color), LoadBEFloat(color + 4), LoadBEFloat(color + 8),
                            LoadBEFloat(color + 12));
  }
  return false;
}

bool Swap(PPCContext& ctx, uint8_t* base) {
  __imp__sub_82287590(ctx, base);
  if (auto* renderer = PlumeRenderer::Get()) {
    renderer->Present();
  }
  return true;
}

}  // namespace

void RegisterPlumeHandlers() {
  SetHandler(kD3DDevice_Clear, Clear);
  SetHandler(kD3DDevice_ClearF, ClearF);
  SetHandler(kD3DDevice_Swap, Swap);
}

}  // namespace renut::d3d
