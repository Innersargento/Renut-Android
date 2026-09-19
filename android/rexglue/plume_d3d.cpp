// Direct3D handlers for renut_renderer = "plume".
//
// Every call still flows to the game's own Direct3D, which records the PM4 the
// Plume command processor (plume_gpu.cpp) executes. These only mirror the
// clear colour, which the renderer shows while no frame has been resolved.
#include "d3d_layer.h"
#include "plume_renderer.h"

#include <cstring>

#include <rex/ppc/func.h>

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

}  // namespace

void RegisterPlumeHandlers() {
  SetHandler(kD3DDevice_Clear, Clear);
  SetHandler(kD3DDevice_ClearF, ClearF);
}

}  // namespace renut::d3d
