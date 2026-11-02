// The register emitters, tiling lists, state resets and a few device internals, natively (see
// leaves.h). Transcribed from the retail executable's PowerPC with idalib. The register names in
// the comments are the original's.
//
//   0x8220F610  start the tiling lists   0x8229A260  the front buffer's format packet
//   0x82287D38  D3D::ResetAllState       0x826597C8  InitializeApiState
//   0x82207680  subresource layout       0x8265D8C0 / 0x82663208  device-gone / register reset
#include "d3d/device/leaves.h"

#include <algorithm>
#include <cstring>

#include "d3d/frontend/native_d3d.h"
#include "d3d/hooks/layer.h"
#include "d3d/device/guest_frame.h"
#include "d3d/device/internal.h"
#include "d3d/resources/resource.h"

extern "C" {
REX_FUNC(sub_82210AB0);  // CDevice::KickOff
REX_FUNC(sub_8222D240);  // CDevice::BeginRingAlloc
REX_FUNC(sub_8222D610);  // CDevice::BlockOnSecondaryPosition
REX_FUNC(sub_822873E8);  // CDevice::StartNewSegment
REX_FUNC(sub_82223270);  // D3D::SetSurfaceClip
REX_FUNC(sub_8220F408);  // list terminate
REX_FUNC(sub_8220F450);  // list link
REX_FUNC(sub_822105F0);  // queue an indirect range
REX_FUNC(sub_82210408);  // CDevice::AddCommandsToPrimaryBuffer
REX_FUNC(sub_822108B8);  // CDevice::CreateInvalidateBuffer
REX_FUNC(sub_822052C8);  // D3D::InsertCallback
REX_FUNC(sub_82278528);  // CDevice::SetFence
REX_FUNC(sub_821F7C18);  // room in the ring
__attribute__((weak)) void sub_82D081FC(PPCContext& __restrict, uint8_t*);  // kernel: enter
__attribute__((weak)) void sub_82D082DC(PPCContext& __restrict, uint8_t*);  // kernel: lock
__attribute__((weak)) void sub_82D082EC(PPCContext& __restrict, uint8_t*);  // kernel: unlock
__attribute__((weak)) void sub_82D08BBC(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82D083BC(PPCContext& __restrict, uint8_t*);  // kernel: the ring capture
__attribute__((weak)) void sub_82D083CC(PPCContext& __restrict, uint8_t*);  // kernel: leave
__attribute__((weak)) void sub_82D08B3C(PPCContext& __restrict, uint8_t*);  // kernel: display mode change

REX_FUNC(sub_8228F558);  // CDevice::BlockOnFence
REX_FUNC(sub_8227AF40);  // D3DDevice_SetDepthStencilSurface
REX_FUNC(sub_8227AB68);  // D3DDevice_SetRenderTarget
REX_FUNC(sub_82208720);  // D3DResource_Release
REX_FUNC(sub_8222F410);  // D3DDevice_SetTexture
REX_FUNC(sub_82658988);  // D3DDevice_SetScreenExtentQueryMode
REX_FUNC(sub_82220B20);  // D3DDevice_SetShaderGPRAllocation
__attribute__((weak)) void sub_822399D8(PPCContext& __restrict, uint8_t*);  // the game's XMemFree
__attribute__((weak)) void sub_82D0827C(PPCContext& __restrict, uint8_t*);  // the device type
__attribute__((weak)) void sub_82660E00(PPCContext& __restrict, uint8_t*);  // the device's trace flush

__attribute__((weak)) void sub_82D0836C(PPCContext& __restrict, uint8_t*);  // lock, returns the state
__attribute__((weak)) void sub_82D0824C(PPCContext& __restrict, uint8_t*);  // unlock with the state
}

namespace renut::d3d::port {
namespace {

// A tile list is { u32 head; u32* link; u32* cursor; u32 end; CDevice* device; }.
constexpr u32 kListHead = 0, kListLink = 4, kListCursor = 8, kListEnd = 0xC, kListDevice = 0x10;

// sub_8220F610(device): a kick, then the tiling and indirect lists start afresh. The tile lists
// are what the GPU replays per tile, which the host does not, so they hold nothing here: each
// is an empty list whose cursor sits in the ring's scratch block, where anything the recompiled
// code still puts in it goes nowhere. What stays is the device's record of the pass (the ring it
// had, and the position it is waiting for).
bool StartTilingLists(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x70);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  f.Call(sub_82210AB0, {d.va});
  d.SetW(0x3460, d.W(0x30));
  d.SetW(0x3464, d.W(0x34));
  d.SetW(0x3468, d.W(0x3A50));
  d.SetW(0x346C, d.W(0x3A4C));
  d.SetW(0x3470, d.W(0x3A44));
  d.SetW(0x3474, d.W(0x3A48));
  const u32 position = d.W(0x33BC) + 1;
  d.SetW(0x33B0, d.W(0x33B8));
  d.SetW(0x33B4, position);
  const u32 sink = renut::native_d3d::RingSinkAddress();
  for (const u32 list : {0x3438u, 0x33C0u, 0x3424u, 0x33D4u, 0x33E8u, 0x33FCu, 0x3410u}) {
    d.SetW(list + kListDevice, d.va);
    d.SetW(list + kListLink, 0);
    d.SetW(list + kListCursor, sink);
    d.SetW(list + kListEnd, sink ? sink + renut::native_d3d::kRingSinkSize : 0);
    d.SetW(list + kListHead, 0);
  }
  return true;
}



// sub_82657978(fence, list, mask): every resource a command buffer's list names gets the
// fence (and the read fence when the entry says so); a nested buffer's list is walked too.
void StampFences(const GuestFrame& f, u32 fence, u32 list, u32 mask) {
  if (mask == 0)
    mask = 0xFFFFFFFFu;
  for (; list; list = f.Get(list)) {
    u32 count = f.Get(list + 4);
    for (u32 entry = list + 8; count; --count, entry += 8) {
      const u32 resource = f.Get(entry) << 2;
      if (!resource)
        continue;
      if (f.Get(entry + 4) & mask)
        f.Put(resource + 8, fence);
      if (f.Get(entry) & 0x40000000u)
        f.Put(resource + 0xC, fence);
      if ((f.Get(resource) & 0xF) == 9) {
        if (const u32 nested = f.Get(resource + 0x70))
          StampFences(f, fence, nested, mask);
      }
    }
  }
}



// sub_8229A260(device, format): the packet that tells the display the front buffer's
// format. The ring is a stub (nothing reads it), so no packet is written.
bool FrontBufferFormatPacket(PPCContext&, uint8_t*) { return true; }

// D3D::ResetAllState(device, skip predication) [0x82287D38]: the window scissor again and every
// dirty mask set so the next draw writes all state. (The original also writes the predication
// registers and the screen extent mode into the ring.)
bool ResetAllState(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x60);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const auto sext15 = [](u32 value) { return u32(i32(value << 17) >> 17); };
  const u32 top_left = d.W(0x28C4), bottom_right = d.W(0x28C8);
  f.Call(sub_82223270, {d.va, sext15(top_left), sext15(top_left >> 16), sext15(bottom_right),
                        sext15(bottom_right >> 16)});
  for (u32 word = 0; word < 5; ++word)
    d.SetPending(word, ~u64(0));
  return true;
}

// sub_82658818(device, one-pass, count, extent): a command buffer recorded tiled. The
// tile grid's size (in EDRAM tiles, by the surface's multisampling) gives each tile its
// index (0x3338...), the surface size register its pitch in tiles, and the buffer's
// pass its dirty bits. The caller leaves the extent's width in r6 (the original uses
// it for both of the aligned sizes).
bool SetupTileExtent(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 count = ctx.r5.u32, extent = ctx.r6.u32;
  u8 flags = d.B(0x2ABF) | 0x20;
  if (ctx.r4.u32 != 0)
    flags |= 0x10;
  d.SetB(0x2ABF, flags);
  const u32 samples = d.H(0x2880) & 3;
  const u32 tile_height = samples == 0 ? 0x10 : 8;           // r30
  const u32 tile_width = samples >= 2 ? 0x10 : 0x20;         // r10
  const u32 band = samples >= 2 ? 0x28 : 0x50;               // r29
  const u32 pitch = (tile_width + extent - 1) & ~(tile_width - 1);  // r4
  const u32 row_tiles = pitch / tile_width;                         // r31
  for (u32 i = 0; i < count; ++i) {
    const u32 x = d.W(0x32C0 + 8 * i), y = d.W(0x32C4 + 8 * i);
    d.SetW(0x3338 + 4 * i, (y / tile_height) * row_tiles + x / tile_width);
  }
  d.SetW(0x3374, pitch);
  d.SetW(0x2880, Insert(d.W(0x2880), pitch, 18, 14));
  d.SetQ(0x10, d.Q(0x10) | 0x100);
  d.SetW(0x3378, ((band + extent - 1) / band) * band);
  // (the original tests bit 0x20 of the flags it has just set, so only the first branch runs)
  d.SetQ(0x28, d.Q(0x28) | 0x100);
  return true;
}

// sub_82654FD8(device): BeginExport's settle. The original writes the export registers into
// the ring; here only the dirty bits those registers leave remain. Skipped while the
// executable's debug word at 0x82FAD204 is set.
bool ExportBegin(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x70);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  if (f.Get(0x82FAD204) != 0)
    return true;
  d.SetQ(0x10, d.Q(0x10) | 0x80000 | Bit(41) | Bit(40) | Bit(39) | 8);
  return true;
}

// sub_82655130(device): EndExport's settle: every state group the export registers touch marked
// dirty and the window clip set again. (The original also writes the registers back to the
// executable's defaults into the ring.)
bool ExportEnd(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xF0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  if (f.Get(0x82FAD208) != 0)
    return true;
  d.SetQ(0x10, d.Q(0x10) | Bit(41) | Bit(40) | Bit(39) | 0x80 | 0x40 | 0x20 | Bit(44) | 0x80000 | 0x100000 |
                   0x800 | 0x100 | 8 | Bit(37));
  d.SetQ(0x18, d.Q(0x18) | Bit(35) | Bit(54));
  d.SetQ(0x20, d.Q(0x20) | 0x80000 | Bit(35));
  const auto sext15 = [](u32 value) { return u32(i32(value << 17) >> 17); };
  const u32 top_left = d.W(0x28C4), bottom_right = d.W(0x28C8);
  f.Call(sub_82223270, {d.va, sext15(top_left), sext15(top_left >> 16), sext15(bottom_right),
                        sext15(bottom_right >> 16)});
  return true;
}

// sub_82661AF0(allocator, first, end, set): the EDRAM allocator's bitmap (at +0x20, a bit
// a tile, the most significant first) with the tiles [first, end) marked used or free.
bool MarkEdramBits(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0);
  const u32 object = ctx.r3.u32, first = ctx.r4.u32, end = ctx.r5.u32;
  const bool set = ctx.r6.u32 != 0;
  const auto shr = [](u32 value, u32 amount) { return value >> (amount & 31); };
  u32 p = object + 0x20 + (first >> 5) * 4;
  const u32 last = object + 0x20 + (end >> 5) * 4;
  const u32 head = shr(~0u, first & 31), tail = shr(~0u, end & 31);
  if (first >> 5 == end >> 5) {
    const u32 mask = head & ~tail;
    f.Put(p, set ? (f.Get(p) | mask) : (f.Get(p) & ~mask));
    return true;
  }
  f.Put(p, set ? (f.Get(p) | head) : (f.Get(p) & ~head));
  for (p += 4; p < last; p += 4)
    f.Put(p, set ? ~0u : 0u);
  f.Put(p, set ? (f.Get(p) | ~tail) : (f.Get(p) & tail));
  return true;
}


// sub_82210138(device, ?, callback type): the end of a tiled pass. The original hands the pass's
// lists (the tile list, the indirect ranges and the fence) to the GPU through the primary buffer,
// behind an InsertCallback that tells the CPU; the host resolves each tile as it goes, so what is
// left is the pass being over and its fence. A device that ran out of memory gets its ring back
// instead and returns 1.
bool EndTilingLists(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x70);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  d.SetW(0x33B0, 0);
  d.SetW(0x33B4, 0);
  if ((d.B(0x2ABD) >> 5) & 1) {
    d.SetW(0x30, d.W(0x3460));
    d.SetW(0x34, d.W(0x3464));
    d.SetW(0x3A50, d.W(0x3468));
    d.SetW(0x3A4C, d.W(0x346C));
    d.SetW(0x3A44, d.W(0x3470));
    d.SetW(0x3A48, d.W(0x3474));
    d.SetB(0x2ABD, d.B(0x2ABD) & 0xDF);
    d.SetW(0x38, d.W(0x3464) - 0xA0);
    ctx.r3.u64 = 1;
    return true;
  }
  f.Call(sub_82278528, {d.va, d.W(0x30)});
  ctx.r3.u64 = 0;
  return true;
}

// --- Register packets ---------------------------------------------------------
//
// The emitters below write the dirty registers as packets into the ring in the original. The ring
// is a stub nothing reads (the front end flushes from the device's shadow), so they write nothing:
// the shadow is already up to date and the caller clears the dirty bits.

// sub_8222CB90 / sub_8222D0E8 / sub_8220E2B0 / sub_8222AB88(device, mask, ...): single words,
// constant vectors, clip planes and ALU constant blocks.
bool EmitNothing(PPCContext&, uint8_t*) { return true; }

// sub_8226AAB8(device, mask, hi control): the depth/stencil and render-target state the mask
// names. Returns the mask less what it would have written: the depth/stencil control, the
// surface info and the color mask, whose bits the caller then clears.
bool EmitSurfaceState(PPCContext& ctx, uint8_t* base) {
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  u64 mask = ctx.r4.u64 & ~u64(0x100);
  const u64 pending = d.Q(0x28) & mask;
  mask &= ~(pending & (Bit(57) | Bit(37)));
  ctx.r3.u64 = mask;
  return true;
}

// --- Device set-up and tear-down helpers ---------------------------------------

// sub_82659E18(node): a list of side buffers (each { ?, ?, data, next }) freed from the tail.
void FreeSideBuffers(const GuestFrame& f, u32 node) {
  if (!node)
    return;
  FreeSideBuffers(f, f.Get(node + 0xC));
  f.Put(node + 0xC, 0);
  f.Call(sub_822399D8, {f.Get(node + 8), 0xB1800000u});
  f.Call(sub_822399D8, {node, 0x24800000u});
}




// sub_82663208(device): the register shadow back to its power-on values. (The original also
// writes the same into the GPU's registers through the ring.)
bool ResetDeviceRegisters(PPCContext& ctx, uint8_t* base) {
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  d.SetQ(0x2E30, 0xFFFFFFFFull);
  for (u32 i = 0; i < 26; ++i)
    d.SetW(0x480 + 0x18 * i, d.W(0x480 + 0x18 * i) & ~3u);
  for (u32 i = 0; i < 18; ++i)
    d.SetW(0x6F0 + 8 * i, (d.W(0x6F0 + 8 * i) & ~3u) | 1);
  const u32 mode = d.W(0x2944), control = d.W(0x2948);
  d.SetW(0x28BC, 0x20002000u);
  d.SetW(0x29D4, 0x000FF000u);
  d.SetW(0x2A14, 0x10);
  d.SetW(0x2944, mode | 0x80000);
  d.SetW(0x2984, 0xE);
  d.SetW(0x2A10, 0xE);
  d.SetW(0x296C, 8);
  d.SetW(0x29D8, 0x000FF100u);
  d.SetW(0x2954, 4);
  d.SetW(0x2948, control | 0x10000);
  d.SetW(0x28CC, 0x00FFFFFFu);
  d.SetW(0x29C0, 4);
  d.SetW(0x2A48, 2);
  for (u32 word = 0; word < 5; ++word)
    d.SetPending(word, ~u64(0));
  d.SetW(0x2AA4, 0xE);
  return true;
}

// D3D::InitializeApiState(device) [0x826597C8]: the device's tables of render-state and
// sampler-state functions filled from the executable's, every state set to its default
// through them, every texture unset, the default vertex declaration bound, and the
// predication and tiling flags cleared.
bool InitializeApiState(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xB0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 saved_interval = d.W(0x351C), saved_flags = d.W(0x5DAC);
  for (u32 i = 0; i < 101; ++i) {
    const u32 entry = 0x82E506F0 + 12 * i;
    d.SetW(0x40 + 4 * i, f.Get(entry + 4));
    d.SetW(0x224 + 4 * i, f.Get(entry));
    f.CallAddress(d.W(0x40 + 4 * i), {d.va, f.Get(entry + 8)});
  }
  for (u32 sampler = 0; sampler < 26; ++sampler) {
    for (u32 state = 0; state < 20; ++state) {
      const u32 entry = 0x82E50BD0 + 12 * state;
      d.SetW(0x1D4 + 4 * state, f.Get(entry + 4));
      d.SetW(0x3B8 + 4 * state, f.Get(entry));
      f.CallAddress(d.W(0x1D4 + 4 * state), {d.va, sampler, f.Get(entry + 8)});
    }
    f.CallWide(sub_8222F410, {d.va, sampler, 0, Bit(63) >> (sampler + 0x20)});
  }
  d.SetW(0x2FA0, 5);
  d.SetW(0x2FA4, 1);
  f.Call(sub_82658988, {d.va, 2});
  d.SetW(0x3090, 2);
  d.SetW(0x2FD8, 0x002A23B9u);
  d.SetH(0x2FD4, 0);
  d.SetH(0x2FD6, 0);
  d.SetB(0x2FDC, 0);
  d.SetB(0x2FDD, 0);
  d.SetB(0x2FDE, 0);
  d.SetW(0x2FE4, 0xFFFFFFFFu);
  d.SetW(0x2FE8, 0);
  d.SetW(0x2FE0, 0x00FF0000u);
  d.SetW(0x2FB8, 1);
  d.SetW(0x2FBC, 0);
  d.SetW(0x2FD0, 0);
  d.SetW(0x2E2C, d.va + 0x2FA0);
  d.SetQ(0x10, d.Q(0x10) | 0x80000);
  d.SetW(0x351C, saved_interval);
  d.SetW(0x5DAC, (d.W(0x5DAC) & ~1u) | (saved_flags & 1));
  const u8 mode = d.B(0x2ABC);
  d.SetW(0x31AC, 0);
  const bool tiled = !(mode & 8) && !(mode & 4) && d.B(0x2F9B) == 0 && TilingBound(d);
  d.SetW(0x31A4, 0xFFFFFFFFu);
  d.SetW(0x31A8, 0);
  d.SetW(0x2AB4, 0);
  d.SetW(0x2AB8, 0);
  const u8 flags = u8((tiled ? 1 : 0) | (mode & 0xFE));
  d.SetB(0x2ABC, flags);
  if (!(flags & 0x80))
    f.Call(sub_82220B20, {d.va, 0, 0, 0});
  return true;
}

// sub_82207680(texture, slice, level, &row pitch, &slice bytes, &level bytes, &tail offset,
// &width, [stack: &height, &depth, &address]): the layout of one subresource, computed by the
// resource port (resource.cpp), which the lock calls use as well.
bool GetSubresourceLayout(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0);
  const SubresourceInfo info = QuerySubresource(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const u32 sp = ctx.r1.u32;
  const auto store = [&](u32 pointer, u32 value) {
    if (pointer)
      f.Put(pointer, value);
  };
  store(ctx.r6.u32, info.row_pitch);
  store(ctx.r7.u32, info.slice_bytes);
  store(ctx.r8.u32, info.level_bytes);
  store(ctx.r9.u32, info.tail_offset);
  store(ctx.r10.u32, info.width);
  store(f.Get(sp + 0x54), info.height);
  store(f.Get(sp + 0x5C), info.depth);
  store(f.Get(sp + 0x64), info.address);
  return true;
}

// sub_821EF4E8(device, ...): the debug notification the title's tooling gets for a presented
// frame. Nothing happens unless the executable's debug flag (0x82FA390C) is set, which is
// the only case left to the recompiled original.
bool NotifyPresented(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0);
  return f.G(0x82FA390C)[0] == 0;
}


// sub_821ED560(device, changes, scan format, front buffer, format, width, height, scaler
// [stack: extra]): what Swap leaves for the title's frame hook: the arguments of the swap
// are kept in the device (+0x54B0...), the kernel told when the display mode changed, and
// the frame's callback put in the ring. The branch that asks the display driver for a
// callback object itself (first swap of a device with no fences yet) stays the original's.
bool SwapNotify(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xC0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 changes = ctx.r4.u32, scan_format = ctx.r5.u32;
  const bool callbacks = (d.W(0x5420) & 0x40000000u) != 0;
  if (callbacks && scan_format == 0 && !(d.B(0x2ABD) & 2))
    return false;  // 0x8265C4B8 builds the callback's context
  if ((changes & 8) && !sub_82D08B3C)
    return false;
  u32 context = ctx.r6.u32;  // r30
  d.SetW(0x54B0, ctx.r7.u32);
  d.SetW(0x54B4, ctx.r8.u32);
  d.SetW(0x54B8, ctx.r9.u32);
  d.SetW(0x54BC, context);
  d.SetW(0x54C0, ctx.r10.u32);
  const u32 extra = f.Get(ctx.r1.u32 + 0x54);
  d.SetW(0x54C4, extra);
  if (changes & 8)
    f.Call(sub_82D08B3C, {d.va + 0x54C8});
  if (!callbacks)
    return true;
  std::memset(f.G(f.sp + 0x54), 0, 0x18);
  f.Put(f.sp + 0x50, 4 + (scan_format != 0 ? 1u : 0u));
  if (const u32 owner = f.Get(f.Get(0x8200095C)))
    f.CallAddress(f.Get(owner + 0x18), {0x5C, f.sp + 0x50});
  context = f.Get(f.sp + 0x68);
  f.Call(sub_822052C8, {d.va, d.W(0x30), 0, 0x8265C418, context});
  return true;
}


}  // namespace

void RegisterCommandLists() {
  SetHandler(kD3D_StartTilingLists, StartTilingLists);
  SetHandler(kD3D_SwapNotify, SwapNotify);
  SetHandler(kD3D_NotifyPresented, NotifyPresented);
  SetHandler(kD3D_GetSubresourceLayout, GetSubresourceLayout);
  SetHandler(kD3D_ResetDeviceRegisters, ResetDeviceRegisters);
  SetHandler(kD3D_InitializeApiState, InitializeApiState);
  SetHandler(kD3D_EmitSurfaceState, EmitSurfaceState);
  SetHandler(kD3D_EmitRegisterWords, EmitNothing);
  SetHandler(kD3D_EmitConstantVectors, EmitNothing);
  SetHandler(kD3D_EmitClipPlanes, EmitNothing);
  SetHandler(kD3D_EmitConstantBlocks, EmitNothing);
  SetHandler(kD3D_FrontBufferFormatPacket, FrontBufferFormatPacket);
  SetHandler(kD3D_ResetAllState, ResetAllState);
  SetHandler(kD3D_SetupTileExtent, SetupTileExtent);
  SetHandler(kD3D_ExportBegin, ExportBegin);
  SetHandler(kD3D_ExportEnd, ExportEnd);
  SetHandler(kD3D_MarkEdramBits, MarkEdramBits);
  SetHandler(kD3D_EndTilingLists, EndTilingLists);
}

}  // namespace renut::d3d::port
