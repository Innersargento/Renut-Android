// BeginTiling, EndTiling, RunCommandBuffer and AllocateEdramMemory, natively (see tiling.h).
//
// Offsets are this release's, read off the retail disassembly (not through Layout()).
// Neither writes the ring or builds the tile lists the GPU would replay per tile.
#include "d3d/scene/tiling.h"

#include <bit>
#include <cstring>

#include "d3d/hooks/layer.h"
#include "d3d/device/guest_frame.h"
#include "d3d/device/internal.h"

extern "C" {
REX_FUNC(sub_82210AB0);  // CDevice::KickOff
REX_FUNC(sub_82222EA0);  // D3DDevice_SetViewport
REX_FUNC(sub_82223178);  // D3DDevice_SetScissorRect
REX_FUNC(sub_82223270);  // D3D::SetSurfaceClip
REX_FUNC(sub_822811A0);  // D3DDevice_ClearF
REX_FUNC(sub_8222AA18);  // D3DDevice_SetPredication
REX_FUNC(sub_822899D0);  // D3DDevice_SetRenderState_HiZWriteEnable
REX_FUNC(sub_8222D6C8);  // D3DDevice_Resolve
REX_FUNC(sub_826572C8);  // CDevice::QueueIndirectBuffers
REX_FUNC(sub_82210138);  // D3D::EndTilingLists
__attribute__((weak)) void sub_82222F20(PPCContext& __restrict, uint8_t*);  // the viewport from floats
__attribute__((weak)) void sub_8220F610(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82287D38(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8222D0E8(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8222BF10(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8226AAB8(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8222CB90(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8222AB88(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8220E2B0(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82657A20(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82657978(PPCContext& __restrict, uint8_t*);
}

namespace renut::d3d::port {
namespace {

constexpr u32 kViewportDefault = 0x820018DC, kScissorDefault = 0x82001190;
constexpr u32 kOutOfMemoryHresult = 0x8007000E;

u32 Kick(const GuestFrame& f, u32 dev) { return f.Call(sub_82210AB0, {dev}); }

// A call with a float in f1 and the stack arguments the callers keep at +0x5C and +0x64.
u32 CallWithFloat(const GuestFrame& f, GuestFn function, std::array<u32, 8> args, float value) {
  PPCContext call = *f.ctx;
  call.r1.u32 = f.sp;
  call.r3.u32 = args[0];
  call.r4.u32 = args[1];
  call.r5.u32 = args[2];
  call.r6.u32 = args[3];
  call.r7.u32 = args[4];
  call.r8.u32 = args[5];
  call.r9.u32 = args[6];
  call.r10.u32 = args[7];
  call.f1.f64 = double(value);
  function(call, f.base);
  return call.r3.u32;
}


// What BeginTiling, EndTiling and BeginCommandBuffer compute after a mode change: whether
// the device draws in a tiling-predicated mode (bit 0 of the flags byte).
u32 TiledBit(Device d) {
  const u8 mode = d.B(0x2ABC);
  if ((mode & 0x08) || (mode & 0x04) || d.B(0x2F9B) != 0)
    return 0;
  return TilingBound(d) ? 1 : 0;
}

void SetTiledBit(Device d, u32 tiled) {
  d.SetB(0x2ABC, u8((d.B(0x2ABC) & 0xFE) | tiled));
}

// A viewport restored from the 7 words at `saved` (the floats of the original viewport and
// the last word in r10).
void RestoreViewport(const GuestFrame& f, u32 dev, u32 saved) {
  if (!sub_82222F20)
    return;
  PPCContext call = *f.ctx;
  call.r1.u32 = f.sp;
  call.r3.u32 = dev;
  call.f1.f64 = std::bit_cast<float>(f.Get(saved));
  call.f2.f64 = std::bit_cast<float>(f.Get(saved + 4));
  call.f3.f64 = std::bit_cast<float>(f.Get(saved + 8));
  call.f4.f64 = std::bit_cast<float>(f.Get(saved + 12));
  call.f5.f64 = std::bit_cast<float>(f.Get(saved + 16));
  call.f6.f64 = std::bit_cast<float>(f.Get(saved + 20));
  call.r10.u32 = f.Get(saved + 24);
  sub_82222F20(call, f.base);
}

// --- BeginTiling ----------------------------------------------------------------

bool HaveBegin() {
  return sub_82222F20 && sub_8220F610 && sub_82287D38;
}

// D3DDevice_BeginTiling(device, flags, count, tile rects, clear colour [vector])
bool BeginTiling(PPCContext& ctx, uint8_t* base) {
  if (!HaveBegin())
    return false;
  GuestFrame f(ctx, base, 0xC0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 dev = d.va, count = ctx.r5.u32, rects = ctx.r6.u32, color = ctx.r7.u32;
  const u32 clear_depth_bits = f.Get(0x82000D18);
  const float clear_depth = std::bit_cast<float>(clear_depth_bits);

  d.SetW(0x31CC, count);
  u32 extent_x = 0, extent_y = 0;
  for (u32 i = 0; i < count; ++i) {
    const u32 x1 = f.Get(rects + 16 * i), y1 = f.Get(rects + 16 * i + 4);
    const u32 x2 = f.Get(rects + 16 * i + 8), y2 = f.Get(rects + 16 * i + 12);
    if (!(extent_x > x2))
      extent_x = x2;
    if (!(extent_y > y2))
      extent_y = y2;
    d.SetW(0x31D0 + 16 * i, x1);
    d.SetW(0x31D4 + 16 * i, y1);
    d.SetW(0x31D8 + 16 * i, x2);
    d.SetW(0x31DC + 16 * i, y2);
    d.SetW(0x32C0 + 8 * i, x1 & ~31u);
    d.SetW(0x32C4 + 8 * i, y1 & ~31u);
  }
  d.SetW(0x337C, extent_x);
  d.SetW(0x3380, extent_y);
  d.SetW(0x3384, 0);

  if (!(d.B(0x2ABF) & 0x20)) {
    // One clear over a tile's worth of the target (the widest and tallest tile).
    u32 tile_w = 0, tile_h = 0;
    for (u32 i = 0; i < count; ++i) {
      const u32 width = d.W(0x31D8 + 16 * i) - d.W(0x32C0 + 8 * i);
      const u32 height = d.W(0x31DC + 16 * i) - d.W(0x32C4 + 8 * i);
      if (!(tile_w > width))
        tile_w = width;
      if (!(tile_h > height))
        tile_h = height;
    }
    f.Put(f.sp + 0x58, tile_w);
    f.Put(f.sp + 0x5C, tile_h);
    f.Put(f.sp + 0x50, 0);
    f.Put(f.sp + 0x54, 0);
    std::memcpy(f.G(f.sp + 0x70), f.G(dev + 0x3168), 0x1C);
    for (u32 i = 0; i < 4; ++i)
      f.Put(f.sp + 0x60 + 4 * i, d.W(0x3184 + 4 * i));
    f.Call(sub_82222EA0, {dev, kViewportDefault});
    f.Call(sub_82223178, {dev, kScissorDefault});
    CallWithFloat(f, sub_822811A0, {dev, 0x3F, f.sp + 0x50, color, 0, 0}, clear_depth);
    RestoreViewport(f, dev, f.sp + 0x70);
    f.Call(sub_82223178, {dev, f.sp + 0x60});
  }

  if (color) {
    std::memcpy(d.bytes + 0x3390, f.G(color), 16);
  } else {
    d.SetQ(0x3390, 0);
    d.SetQ(0x3398, 0);
  }
  d.SetW(0x33A0, clear_depth_bits);
  d.SetW(0x33A4, 0);
  const u32 first = d.W(0x3098);
  const u32 surface = first ? first : d.W(0x30A8);
  d.SetB(0x2ABC, d.B(0x2ABC) | 0x20);
  d.SetW(0x31B4, surface);
  for (u32 i = 0; i < 5; ++i)
    d.SetW(0x31B8 + 4 * i, d.W(0x3098 + 4 * i));
  d.SetW(0x31B0, 2);
  d.SetW(0x31AC, 0);
  const u8 mode = d.B(0x2ABC);
  const u32 tiled = (!(mode & 0x08) && !(mode & 0x04) && d.B(0x2F9B) == 0) ? 1u : 0u;
  d.SetW(0x31A8, 0);
  d.SetW(0x31A4, 0xFFFFFFFFu);
  d.SetW(0x2AB4, 0);
  d.SetW(0x2AB8, 0);
  SetTiledBit(d, tiled);
  f.Call(sub_8220F610, {dev});

  // (The original also writes a packet into the ring and reserves room for the per-tile
  // packets; the ring is a stub nothing reads.)
  f.Call(sub_82222EA0, {dev, kViewportDefault});
  f.Call(sub_82287D38, {dev, 1});
  return true;
}

// --- EndTiling --------------------------------------------------------------------

bool HaveEnd() {
  return sub_82287D38;
}

// HRESULT D3DDevice_EndTiling(device, flags, ?, destination texture, clear colour)
//
// Each tile's rect is resolved from the full-size target on the host, so what the original leaves
// for the GPU to replay per tile is left out, as in reNut: the tile list's closing records, the
// indirect ranges and the WorkerDpc callback that replays the list. What stays is the resolves,
// the predication and render state around them, the kick, the device's flags and the fence that
// covers the pass (EndTilingLists).
bool EndTiling(PPCContext& ctx, uint8_t* base) {
  if (!HaveEnd())
    return false;
  GuestFrame f(ctx, base, 0xD0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 dev = d.va, texture = ctx.r6.u32, color = ctx.r7.u32;
  const float default_depth = std::bit_cast<float>(f.Get(0x82000D0C));

  if (texture && d.W(0x31CC) > 0) {
    for (u32 i = 0; i < d.W(0x31CC); ++i) {
      const u32 tile = dev + 0x31D0 + 16 * i;
      const u32 format = f.Get(d.W(0x31B4) + 0x24);
      const u32 rect = f.sp + 0x70;
      for (u32 k = 0; k < 4; ++k)
        f.Put(rect + 4 * k, f.Get(tile + 4 * k));
      if (f.Get(rect + 8) == (format >> 18) + 1)
        f.Put(rect + 8, (f.Get(rect + 8) + 7) & ~7u);
      if (f.Get(rect + 12) == ((format >> 3) & 0x7FFF) + 1)
        f.Put(rect + 12, (f.Get(rect + 12) + 7) & ~7u);
      f.Call(sub_8222AA18, {dev, 3u << (2 * i)});
      f.Call(sub_822899D0, {dev, ((~u32(d.B(0x2ABF))) >> 4) & 2});
      const bool last = !(i < d.W(0x31CC) - 1);
      if (!last) {
        f.Put(f.sp + 0x64, 0);
        f.Put(f.sp + 0x5C, d.W(0x33A4));
        CallWithFloat(f, sub_8222D6C8, {dev, 0x300, rect, texture, tile, 0, 0, dev + 0x3390},
                      d.F(0x33A0));
      } else {
        f.Put(f.sp + 0x64, 0);
        f.Put(f.sp + 0x5C, 0);
        CallWithFloat(f, sub_8222D6C8, {dev, 0x300, rect, texture, tile, 0, 0, color}, default_depth);
      }
    }
  }
  f.Call(sub_8222AA18, {dev, 0});
  f.Call(sub_822899D0, {dev, 2});

  if (d.W(0x31CC) != 1) {
    // Around the WorkerDpc callback.
    f.Call(sub_8222AA18, {dev, 0x80000000u});
    f.Call(sub_8222AA18, {dev, 0});
  }
  Kick(f, dev);

  d.SetW(0x31B4, 0);
  d.SetW(0x31AC, 0);
  d.SetB(0x2ABC, d.B(0x2ABC) & 0xDF);
  d.SetB(0x2ABF, d.B(0x2ABF) & 0xCF);
  const u32 tiled = TiledBit(d);
  d.SetW(0x31A8, 0);
  d.SetW(0x31A4, 0xFFFFFFFFu);
  d.SetW(0x2AB4, 0);
  d.SetW(0x2AB8, 0);
  SetTiledBit(d, tiled);

  // sub_82210138: the pass is over and its fence goes out, unless the device's command memory
  // ran out (EndCommandBuffer marks a device so until the next Begin).
  const bool failed = f.Call(sub_82210138, {dev, d.W(0x31CC), 0}) != 0;
  f.Call(sub_82222EA0, {dev, kViewportDefault});
  if (failed) {
    f.Call(sub_82287D38, {dev, 0});
    ctx.r3.u64 = kOutOfMemoryHresult;
  } else {
    ctx.r3.u64 = 0;
  }
  return true;
}

// --- RunCommandBuffer -------------------------------------------------------------





// --- AllocateEdramMemory ----------------------------------------------------------

// BOOL D3D::AllocateEdramMemory(tiles, u32* start out): the console's search of its EDRAM bitmap
// for a free run. There is no EDRAM behind the host's targets (each lives in its own image, found by
// its surface), so nothing is searched or marked: the call always succeeds, with a start of 0.
bool AllocateEdramMemory(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.u32) GuestFrame(ctx, base, 0).Put(ctx.r4.u32, 0);
  ctx.r3.u64 = 1;
  return true;
}

}  // namespace

void RegisterTiling() {
  SetHandler(kD3DDevice_BeginTiling, BeginTiling);
  SetHandler(kD3DDevice_EndTiling, EndTiling);
}


void RegisterEdram() {
  SetHandler(kD3D_AllocateEdramMemory, AllocateEdramMemory);
}

}  // namespace renut::d3d::port
