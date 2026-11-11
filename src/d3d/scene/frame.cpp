// The frame-level device calls, natively (see frame.h).
//
// The offsets are this release's: they are read off the retail disassembly, so they
// are written as they are and do not go through Layout().
#include "d3d/scene/frame.h"

#include <array>
#include <cmath>
#include <cstring>

#include "d3d/hooks/layer.h"
#include "d3d/device/device.h"
#include "d3d/device/guest_frame.h"
#include "d3d/device/internal.h"

#include <rex/ppc/context.h>

extern "C" {
// Hooked entry points: the native front end and the other ports run them.
REX_FUNC(sub_82210AB0);  // CDevice::KickOff
REX_FUNC(sub_8227AB68);  // D3DDevice_SetRenderTarget
REX_FUNC(sub_8228A748);  // D3D::SynchronizeToPresentationInterval
REX_FUNC(sub_8222D6C8);  // D3DDevice_Resolve
REX_FUNC(sub_82223178);  // D3DDevice_SetScissorRect
REX_FUNC(sub_82287590);  // D3DDevice_Swap
REX_FUNC(sub_82656208);  // D3DDevice_BeginIndexedVertices
REX_FUNC(sub_82223560);  // D3D::ClearF
REX_FUNC(sub_822811A0);  // D3DDevice_ClearF
REX_FUNC(sub_822052C8);  // D3D::InsertCallback
// Recompiled leaves; weak, and a handler that needs one declines when it is missing.
__attribute__((weak)) void sub_82222F20(PPCContext& __restrict, uint8_t*);  // the viewport from floats
__attribute__((weak)) void sub_826593F8(PPCContext& __restrict, uint8_t*);  // gamma ramp -> table
__attribute__((weak)) void sub_82659490(PPCContext& __restrict, uint8_t*);  // PWL gamma -> table
__attribute__((weak)) void __imp__KeGetCurrentProcessType(PPCContext& __restrict, uint8_t*);  // the device type
__attribute__((weak)) void sub_8265F3B0(PPCContext& __restrict, uint8_t*);  // scaler fit
__attribute__((weak)) void __imp__VdCallGraphicsNotificationRoutines(PPCContext& __restrict, uint8_t*);  // the video scaler call
__attribute__((weak)) void sub_82654FD8(PPCContext& __restrict, uint8_t*);  // export fence packet
__attribute__((weak)) void sub_82655130(PPCContext& __restrict, uint8_t*);  // export fence packet, end
}

namespace renut::d3d::port {
namespace {

using Fn = GuestFn;

// The frame the original would have had: `size` bytes below the caller's stack pointer,
// whose locals (above 0x50) and outgoing stack arguments (0x54, 0x5C, 0x64...) live in
// guest memory (guest_frame.h).
struct Frame : GuestFrame {
  using GuestFrame::GuestFrame;

  u32 Kick(u32 device) const { return Call(sub_82210AB0, {device}); }
};

u32 GpuOf(u32 address) {
  return (((address >> 20) + 0x200) & 0x1000) + (address & 0x1FFFFFFF);
}

// --- Present ---------------------------------------------------------------

// D3DDevice_Present(device): the back buffer is resolved to the front buffer and
// swapped, with the viewport and scissor that were set put back afterwards.
bool Present(PPCContext& ctx, uint8_t* base) {
  if (!sub_82222F20)
    return false;
  Frame f(ctx, base, 0xD0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 dev = d.va;
  // Locals: the viewport (0x1C bytes) at +0xA0, the scissor rect at +0x90.
  std::memcpy(f.G(f.sp + 0xA0), f.G(dev + 0x3168), 0x1C);
  for (u32 i = 0; i < 4; ++i)
    f.Put(f.sp + 0x90 + 4 * i, d.W(0x3184 + 4 * i));

  f.Call(sub_8227AB68, {dev, 0, d.W(0x39F0)});
  f.Call(sub_8228A748, {dev, 0});

  // Resolve(device, 0, 0, front buffer, 0, 0, 0, clear colour): no clear flags, so the
  // colour (1.0, 1.0, 0.0, 0.0 from the executable's constants) is never read.
  const u32 constant_a = f.Get(0x82000D18), constant_b = f.Get(0x82000D0C);
  f.Put(f.sp + 0x80, constant_b);
  f.Put(f.sp + 0x84, constant_b);
  f.Put(f.sp + 0x88, constant_a);
  f.Put(f.sp + 0x8C, constant_a);
  f.Put(f.sp + 0x54, 0);
  f.Put(f.sp + 0x5C, 0);
  f.Put(f.sp + 0x64, 0);
  f.Call(sub_8222D6C8, {dev, 0, 0, d.W(0x39EC), 0, 0, 0, f.sp + 0x80});

  f.Kick(dev);

  {
    PPCContext call = ctx;
    call.r1.u32 = f.sp;
    call.r3.u32 = dev;
    call.f1.f64 = std::bit_cast<float>(f.Get(f.sp + 0xA0));
    call.f2.f64 = std::bit_cast<float>(f.Get(f.sp + 0xA4));
    call.f3.f64 = std::bit_cast<float>(f.Get(f.sp + 0xA8));
    call.f4.f64 = std::bit_cast<float>(f.Get(f.sp + 0xAC));
    call.f5.f64 = std::bit_cast<float>(f.Get(f.sp + 0xB0));
    call.f6.f64 = std::bit_cast<float>(f.Get(f.sp + 0xB4));
    call.r10.u32 = f.Get(f.sp + 0xB8);
    sub_82222F20(call, base);
  }
  f.Call(sub_82223178, {dev, f.sp + 0x90});
  f.Call(sub_82287590, {dev, d.W(0x39EC), dev + 0x3548});
  return true;
}

// --- Gamma -----------------------------------------------------------------

// SetGammaRamp and SetPWLGamma are in hooks/handlers.cpp. The port that was here
// named the kernel's process-type thunk (0x82D0827C) as a function, found none, and so always declined.

// --- Scaler ----------------------------------------------------------------

// D3DDevice_SetScalerParameters(device, parameters): the scaler's rectangles are
// fitted to the source size, stored in the device and handed to the video kernel.
bool SetScalerParameters(PPCContext& ctx, uint8_t* base) {
  if (!sub_8265F3B0 || !__imp__VdCallGraphicsNotificationRoutines)
    return false;
  Frame f(ctx, base, 0x100);
  Device d = DeviceAt(base, ctx.r3.u32);
  const u32 params = ctx.r4.u32;
  if (!d || !params)
    return false;
  const u32 local = f.sp + 0x50;
  for (u32 i = 0; i < 6; ++i)
    f.Put(local + 4 * i, f.Get(params + 4 * i));
  const u32 flag = f.Get(params + 0x18) ? 3 : 0;
  f.Put(local + 0x18, flag);
  f.Put(local + 0x28, flag);
  for (u32 offset : {0x1Cu, 0x20u, 0x24u, 0x2Cu, 0x30u, 0x34u})
    f.Put(local + offset, 0);

  u32 width, height;
  if (const u32 source = d.W(0x39EC)) {
    const u32 w24 = f.Get(source + 0x24), w28 = f.Get(source + 0x28);
    const u32 bump = (((w28 << 2) | (w28 >> 30)) & 2) + 1;
    width = (w24 & 0x1FFF) + bump;
    height = ((w24 >> 13) & 0x1FFF) + bump;
  } else {
    width = d.W(0x34E8);
    height = d.W(0x34EC);
  }
  const u32 stored = d.va + 0x359C;
  std::memcpy(f.G(stored), f.G(local), 0x38);
  d.SetB(0x2ABE, d.B(0x2ABE) | 0x10);
  f.Call(sub_8265F3B0, {d.va, width, height, stored, local, 0});

  const u32 packet = f.sp + 0x90;
  StoreBE16(f.G(packet), u16(width));
  StoreBE16(f.G(packet + 2), u16(height));
  StoreBE16(f.G(packet + 4), u16(d.W(0x5428)));
  StoreBE16(f.G(packet + 6), u16(d.W(0x542C)));
  std::memcpy(f.G(packet + 8), f.G(local), 0x38);
  f.Call(__imp__VdCallGraphicsNotificationRoutines, {1, packet});
  return true;
}

// --- DrawIndexedVerticesUP -------------------------------------------------

// D3DDevice_DrawIndexedVerticesUP(device, primitive, start, vertex count, index count,
// indices, index format, vertices, stride [stack]): room for both from
// BeginIndexedVertices, then the data copied in.
bool DrawIndexedVerticesUP(PPCContext& ctx, uint8_t* base) {
  Frame f(ctx, base, 0xB0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 primitive = ctx.r4.u32, start = ctx.r5.u32, vertex_count = ctx.r6.u32,
            index_count = ctx.r7.u32, indices = ctx.r8.u32, format = ctx.r9.u32,
            vertices = ctx.r10.u32;
  const u32 stride = LoadBE32(renut::GuestPtr(base, ctx.r1.u32 + 0x54));
  f.Put(f.sp + 0x54, f.sp + 0x60);
  const u32 result = f.Call(sub_82656208, {d.va, primitive, 0u - start, vertex_count, index_count,
                                           format, stride, f.sp + 0x64});
  ctx.r3.u64 = result;
  if (result != 0)
    return true;
  const u32 vertex_room = f.Get(f.sp + 0x60), index_room = f.Get(f.sp + 0x64);
  std::memcpy(f.G(vertex_room), f.G(vertices + start * stride), vertex_count * stride);
  const u32 index_size = (format & 4) ? 4 : 2;
  std::memcpy(f.G(index_room), f.G(indices), (index_size * index_count + 3) & ~3u);
  d.SetW(0x30, d.W(0x3484));
  return true;
}

// --- Memory export ---------------------------------------------------------

constexpr u32 kExportDepth = 0x34C0, kExportDone = 0x34C4,
              kExportKind = 0x34C8;

// The guest address D3D's export packets name for a resource: its base address (for a
// texture, the first of its levels).
u32 ExportBase(const Frame& f, u32 resource) {
  switch (f.Get(resource) & 0xF) {
    case 1:
      return f.Get(resource + 0x18) & ~3u;
    case 3: {
      const u32 address = f.Get(resource + 0x20) & ~0xFFFu;
      return address ? address : f.Get(resource + 0x30) & ~0xFFFu;
    }
    default:
      return f.Get(resource + 0x18);
  }
}

bool HaveExport() {
  return sub_82654FD8 && sub_82655130;
}

// D3DDevice_BeginExport(device, index, resource, kind). The original also writes the export's
// registers into the ring; the ring is a stub nothing reads, so only the device's state is kept:
// the export base, the dirty bit, and the settle (which marks the registers dirty).
bool BeginExport(PPCContext& ctx, uint8_t* base) {
  if (!HaveExport())
    return false;
  Frame f(ctx, base, 0x80);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 index = ctx.r4.u32, resource = ctx.r5.u32, kind = ctx.r6.u32;
  f.Put(d.va + (index + 0xA26) * 4, GpuOf(ExportBase(f, resource)));
  d.SetQ(0x10, d.Q(0x10) | ((u64(1) << 63) >> ((index + 0xC) & 0x7F)));
  if (d.W(kExportDepth) == 0 && (kind == 0xA || d.W(kExportDone) == 0))
    f.Call(sub_82654FD8, {d.va});
  d.SetW(kExportKind, kind & 3);
  d.SetW(kExportDone, 0);
  d.SetW(kExportDepth, d.W(kExportDepth) + 1);
  return true;
}

// D3DDevice_EndExport(device, index, resource, kind). As BeginExport, without the ring packets:
// the resource gets the current fence (or goes on the list of resources the buffer being
// recorded names), its range is invalidated, and the settle runs.
bool EndExport(PPCContext& ctx, uint8_t* base) {
  if (!HaveExport())
    return false;
  Frame f(ctx, base, 0xD0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 index = ctx.r4.u32, resource = ctx.r5.u32, kind = ctx.r6.u32;
  f.Put(d.va + (index + 0xA26) * 4, 0);
  if (const u32 fence = d.W(0x2A9C)) {
    f.Put(resource + 8, fence);
    f.Put(resource + 12, fence);
  }
  // (A device recording a command buffer used to list the resource for the original Run to stamp
  // with a fence, and the export's range was invalidated with a ring packet. The native Run stamps
  // nothing and InvalidateRange writes nothing, so neither is left.)

  const u32 depth = d.W(kExportDepth) - 1;
  d.SetW(kExportDepth, depth);
  if (depth == 0 && kind != 1) {
    if (kind != 2)
      f.Call(sub_82655130, {d.va});
  } else {
    f.Call(sub_82654FD8, {d.va});
  }
  d.SetW(kExportDone, (d.W(kExportDepth) == 0 && kind == 1) ? 1 : 0);
  return true;
}

// --- ClearF / InsertCallback -------------------------------------------------

// D3DDevice_ClearF(device, flags, rects, colour, depth [f1], r7, stencil): with no
// rectangles, one over the render target (or depth surface), or over the tiling extent
// while the device is tiling with the same targets it was bound with.
bool ClearF(PPCContext& ctx, uint8_t* base) {
  MirrorClearColorF(ctx, base);
  Frame f(ctx, base, 0x70);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  u32 rects = ctx.r5.u32, r7 = ctx.r7.u32;
  if (rects == 0) {
    r7 = d.W(0x3098);
    const u32 surface = r7 ? r7 : d.W(0x30A8);
    const u32 format = f.Get(surface + 0x24);
    const u32 rect = f.sp + 0x50;
    f.Put(rect, 0);
    f.Put(rect + 4, 0);
    f.Put(rect + 8, (format >> 18) + 1);
    f.Put(rect + 12, ((format >> 3) & 0x7FFF) + 1);
    if (TilingBound(d)) {
      f.Put(rect + 8, d.W(0x337C));
      f.Put(rect + 12, d.W(0x3380));
    }
    rects = rect;
  }
  PPCContext call = ctx;
  call.r1.u32 = f.sp;
  call.r5.u32 = rects;
  call.r7.u32 = r7;
  sub_82223560(call, base);
  return true;
}

// D3DDevice_Clear(device, count, rects, flags, colour [D3DCOLOR], depth [f1], r8, stencil):
// ClearF per rectangle with the colour as a vector. XMLoadColor: each byte is read as
// 1 + b / 2^23 and scaled by 2^23 / 255 less its offset (reNut's PC build, d3d/hooks/draw.cpp,
// has the same constants).
bool Clear(PPCContext& ctx, uint8_t* base) {
  MirrorClearColor(ctx);
  Frame f(ctx, base, 0xA0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32, color = ctx.r7.u32,
            stencil = ctx.r9.u32;
  const float scale = std::bit_cast<float>(0x47008081u);
  const auto channel = [scale](u32 byte) {
    return std::fmaf(std::bit_cast<float>(0x3F800000u | (byte & 0xFF)), scale, -scale);
  };
  const u32 vector = f.sp + 0x50;
  const float values[4] = {channel(color >> 16), channel(color >> 8), channel(color), channel(color >> 24)};
  for (u32 i = 0; i < 4; ++i)
    f.Put(vector + 4 * i, std::bit_cast<u32>(values[i]));
  const auto clear = [&](u32 rect) {
    PPCContext call = ctx;
    call.r1.u32 = f.sp;
    call.r3.u32 = d.va;
    call.r4.u32 = flags;
    call.r5.u32 = rect;
    call.r6.u32 = vector;
    call.r8.u32 = stencil;
    sub_822811A0(call, base);
  };
  if (rects == 0) {
    clear(0);
  } else {
    for (u32 i = 0; i < count; ++i)
      clear(rects + 0x10 * i);
  }
  return true;
}

}  // namespace

void RegisterFrame() {
  SetHandler(kD3DDevice_Clear, Clear);
  SetHandler(kD3DDevice_ClearF, ClearF);
  SetHandler(kD3DDevice_Present, Present);
  SetHandler(kD3DDevice_SetScalerParameters, SetScalerParameters);
  SetHandler(kD3DDevice_DrawIndexedVerticesUP, DrawIndexedVerticesUP);
  SetHandler(kD3DDevice_BeginExport, BeginExport);
  SetHandler(kD3DDevice_EndExport, EndExport);
}

}  // namespace renut::d3d::port
