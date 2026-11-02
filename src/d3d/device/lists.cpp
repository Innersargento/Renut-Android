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

// The address the GPU reads a list word at: D3D's GPU address less the 0x40000000 window.
u32 ListGpu(u32 address) { return GuestGpu(address) - 0x40000000u; }

// A list is { u32 head; u32* link; u32* cursor; u32 end; CDevice* device; }.
constexpr u32 kListHead = 0, kListLink = 4, kListCursor = 8, kListEnd = 0xC, kListDevice = 0x10;
constexpr u32 kListWords = 0x420;  // the allocation a list segment asks BeginRingAlloc for

constexpr u32 kSideBuffer = 0x4158;

// sub_8220F800(list): the segment that the list is full gets linked to a fresh one.
u32 GrowList(const GuestFrame& f, u32 list) {
  if (const u32 link = f.Get(list + kListLink))
    f.Put(link, ListGpu(f.Get(list + kListCursor)));
  const u32 device = f.Get(list + kListDevice);
  const u32 segment = f.Call(sub_8222D240, {device, kListWords, 0x80});
  u32 start;
  if (!segment) {
    start = f.Get(device + kSideBuffer);
    f.Put(list + kListHead, 0);
  } else {
    start = segment;
    f.Put(segment, ListGpu(segment + 4));
    if (const u32 cursor = f.Get(list + kListCursor))
      f.Put(cursor, ListGpu(segment));
    else
      f.Put(list + kListHead, segment);
    // (the original flushes the segment's cache lines here: the host has no separate cache)
  }
  f.Put(list + kListLink, start);
  f.Put(list + kListCursor, start + 4);
  f.Put(list + kListEnd, start + 0x107C);
  return start + 4;
}





// sub_8220F610(device): a kick, then the tiling and indirect lists start afresh and the
// tile rectangles go into the first of them.
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
  for (u32 i = 0; i < 5; ++i)
    d.SetW(0x344C + 4 * i, d.W(0x3438 + 4 * i));
  for (const u32 list : {0x3438u, 0x33C0u, 0x3424u, 0x33D4u, 0x33E8u, 0x33FCu, 0x3410u}) {
    d.SetW(list + kListDevice, d.va);
    d.SetW(list + kListLink, 0);
    d.SetW(list + kListCursor, 0);
    d.SetW(list + kListEnd, 0);
    d.SetW(list + kListHead, 0);
    GrowList(f, d.va + list);
  }
  u32 packet = d.W(0x3440);
  if (packet + 0xFC > d.W(0x3444))
    packet = GrowList(f, d.va + 0x3438);
  f.Put(packet, 0x80000000u);
  f.Put(packet + 4, d.W(0x3384));
  const u32 count = d.W(0x31CC);
  f.Put(packet + 8, count);
  const u32 x0 = d.W(0x32C0), y0 = d.W(0x32C4);
  for (u32 i = 0; i < count; ++i) {
    f.Put(packet + 0xC + 16 * i, d.W(0x31D0 + 16 * i) - x0);
    f.Put(packet + 0x10 + 16 * i, d.W(0x31D4 + 16 * i) - y0);
    f.Put(packet + 0x14 + 16 * i, d.W(0x31D8 + 16 * i) - x0);
    f.Put(packet + 0x18 + 16 * i, d.W(0x31DC + 16 * i) - y0);
  }
  d.SetW(0x3440, packet + 0xFC);
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



// The ring pointer with room for the packet that follows: kicked first when it is past
// its guarantee (the KickOff's result is the new pointer).
u32 RingForPacket(const GuestFrame& f, Device d) {
  u32 ring = d.W(0x30);
  if (ring > d.W(0x38))
    ring = f.Call(sub_82210AB0, {d.va});
  return ring;
}


// sub_8229A260(device, format): the packet that tells the display the front buffer's
// format (0x28280136 being the one that changes two of its registers).
bool FrontBufferFormatPacket(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x70);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  u32 ring = RingForPacket(f, d);
  const bool special = ctx.r4.u32 == 0x28280136u;
  for (const u32 word : {0xC0022100u, 0x1841u, 0xFFFFF8FFu, special ? 0x100u : 0u, 0x1930u, special ? 3u : 0u}) {
    ring += 4;
    f.Put(ring, word);
  }
  d.SetW(0x30, ring);
  return true;
}

// D3D::ResetAllState(device, skip predication) [0x82287D38]: the predication registers
// back to their device values (unless skipped), the window scissor again, the screen
// extent mode, and every dirty mask set so the next draw writes all state.
bool ResetAllState(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x60);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  if (ctx.r4.u32 == 0) {
    u32 ring = RingForPacket(f, d);
    for (const u32 word : {0xC0006000u, d.W(0x31A4), 0xC0006200u, 0xFFFFFFFFu, 0xC0006100u, d.W(0x31A8),
                           0xC0006300u, 0u}) {
      ring += 4;
      f.Put(ring, word);
    }
    d.SetW(0x30, ring);
  }
  const auto sext15 = [](u32 value) { return u32(i32(value << 17) >> 17); };
  const u32 top_left = d.W(0x28C4), bottom_right = d.W(0x28C8);
  f.Call(sub_82223270, {d.va, sext15(top_left), sext15(top_left >> 16), sext15(bottom_right),
                        sext15(bottom_right >> 16)});
  const u32 mode = d.W(0x2AA4);
  u32 ring = RingForPacket(f, d);
  d.SetW(0x2AA4, mode);
  for (const u32 word : {0x5C8u, 0x20000u, 0xF01u, d.W(0x2AA4)}) {
    ring += 4;
    f.Put(ring, word);
  }
  d.SetW(0x30, ring);
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

// A ring that grows by whole words: `put` writes the next one after the pointer.
struct RingWriter {
  const GuestFrame& f;
  u32 ring;
  void put(u32 word) {
    ring += 4;
    f.Put(ring, word);
  }
  // A block of the executable's own data copied into the ring.
  void copy(u32 source, u32 bytes) {
    std::memcpy(f.G(ring + 4), f.G(source), bytes);
    ring += bytes;
  }
};

// sub_82654FD8(device): the packets that start an export (BeginExport's settle): the
// shared-memory export registers, a block of the executable's register writes, a fence
// event, and the dirty bits those registers leave. Skipped while the executable's debug
// word at 0x82FAD204 is set.
bool ExportBegin(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x70);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  if (f.Get(0x82FAD204) != 0)
    return true;
  RingWriter w{f, RingForPacket(f, d)};
  for (const u32 word : {0x00022100u, 0xFu, 0xEu, 0x10u, 0xC0003B00u, 0x100u, 0xC0102B00u, 0u, 0xFu})
    w.put(word);
  w.copy(0x82121B34, 0x3C);
  for (const u32 word : {0x00012180u, 0x07000000u, 0u, 0x2208u, 4u, 0xC0003600u, 0x00010081u})
    w.put(word);
  d.SetQ(0x10, d.Q(0x10) | 0x80000 | Bit(41) | Bit(40) | Bit(39) | 8);
  d.SetW(0x30, w.ring);
  return true;
}

// sub_82655130(device): the end of an export (EndExport's settle): the resolve registers
// back to the executable's defaults, written as one long packet block, then a fence
// event, with every state group the registers touch marked dirty and the window clip
// set again.
bool ExportEnd(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xF0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  if (f.Get(0x82FAD208) != 0)
    return true;
  RingWriter w{f, RingForPacket(f, d)};
  for (const u32 word : {0xC0003B00u, 0x300u, 0xC0192B00u, 0u, 0x18u})
    w.put(word);
  w.copy(0x82123E08, 0x60);
  d.SetW(0x30, w.ring);
  w.ring = RingForPacket(f, d);
  for (const u32 word : {0xC00A2B00u, 1u, 9u})
    w.put(word);
  w.copy(0x8211E96C, 0x24);
  d.SetW(0x30, w.ring);
  w.ring = RingForPacket(f, d);
  for (const u32 word : {0x00012180u, 0x10000001u, 0u, 0x00022100u, 0x0000FFFFu, 0u, 0u, 0x2293u, 0u,
                         0x00022204u, 0x00010000u, 0x00010000u, 0x300u, 0x2312u, 0x0000FFFFu, 0x200Du, 0u,
                         0x2200u, 0u, 0x2203u, 0u, 0x2208u, 4u, 0x2104u, 0u, 0x2280u, 0x00080008u, 0x2302u,
                         4u, 0x00022080u, 0u, 0u, 0x00100010u})
    w.put(word);
  d.SetW(0x30, w.ring);
  w.ring = RingForPacket(f, d);
  w.put(0xC0003600u);
  w.put(0x00200081u);
  d.SetW(0x30, w.ring);
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


// sub_82210138(device, ?, callback type): the end of a tiled pass: the lists the pass built
// (the tile list, the indirect ranges and the fence) go to the GPU through the primary
// buffer, behind an InsertCallback that tells the CPU. A device that ran out of memory
// gets its ring back instead and returns 1.
bool EndTilingLists(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xA0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 callback_type = ctx.r5.u32;
  if (!(d.B(0x2ABD) & 0x20))
    f.Call(sub_822108B8, {d.va, f.sp + 0x54, f.sp + 0x50});
  const u32 block_a = f.Call(sub_8222D240, {d.va, 0x94, 0x20});   // r28
  const u32 block_b = f.Call(sub_8222D240, {d.va, 0xA, 0x20});    // r30
  u32 block_c = f.Call(sub_8222D240, {d.va, 0xC, 4});             // r29
  const bool out_of_memory = (d.B(0x2ABD) >> 5) & 1;
  d.SetW(0x33B0, 0);
  d.SetW(0x33B4, 0);
  if (out_of_memory) {
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
  const u32 tile_list = d.va + 0x3438;  // r25
  for (const u32 list : {0x3438u, 0x33C0u, 0x3424u, 0x33D4u, 0x33E8u, 0x33FCu, 0x3410u})
    f.Call(sub_8220F408, {d.va + list});
  u32 context = 0;
  if (const u32 head = d.W(0x3438))
    context = GuestGpu(head) - 0x40000000u;
  const u32 inserted = f.Call(sub_822052C8, {d.va, block_a - 4, callback_type | 1, 0x82662FE0, context}) + 4;  // r27
  const u32 fenced = f.Call(sub_82278528, {d.va, block_b - 4}) + 4;                                              // r26
  const u32 words = f.Get(f.sp + 0x50);
  if (words != 0)
    block_c = f.Call(sub_822105F0, {d.va, block_c, f.Get(f.sp + 0x54), words, 0, d.va + 0x344C});
  if (d.W(0x541C) != 0 || !(d.B(0x2ABD) & 2))
    block_c = f.Call(sub_822105F0, {d.va, block_c, GuestGpu(block_a), u32(i32(inserted - block_a) >> 2), 1,
                                    d.va + 0x344C});
  f.Call(sub_822105F0, {d.va, block_c, GuestGpu(block_b), u32(i32(fenced - block_b) >> 2), 0, tile_list});
  ctx.r3.u64 = 0;
  return true;
}

// --- Register packets ---------------------------------------------------------

// sub_821E2940(device, ring, register, data, count, per): `count` words of register data as
// type-0 packets, in pieces of whole records of `per` words that fit under the ring's end
// (kicking for room when none does). Returns the ring pointer.
u32 AppendRegisters(const GuestFrame& f, Device d, u32 ring, u32 reg, u32 data, u32 count, u32 per) {
  for (;;) {
    const i32 fit = (i32(d.W(0x34) - ring) >> 2) - 1;
    if (per && fit / i32(per) > 0) {
      u32 words = u32(fit / i32(per)) * per;
      if (words >= count)
        words = count;
      count -= words;
      ring += 4;
      f.Put(ring, ((words - 1) << 16) | reg);
      for (u32 i = 0; i < words; ++i) {
        data += 4;
        ring += 4;
        f.Put(ring, f.Get(data));
      }
      reg += words;
    }
    if (count == 0)
      return ring;
    d.SetW(0x30, ring);
    f.Call(sub_82210AB0, {d.va});
    ring = d.W(0x30);
  }
}


// How one of the dirty-register emitters lays its runs out: a bit of the mask is `words`
// words of register data, `stride` registers apart, and a run that fits is written in the
// ring directly after the alignment the emitter wants.
struct RunStyle {
  u32 words;   // words of data (and registers) a mask bit stands for
  u32 stride;  // registers a bit advances
  u32 per;     // the record size AppendRegisters keeps whole
  u32 extra;   // words of room beyond the data a run needs to be written directly
  enum Align { kNone, kVector, kPad8 } align;
};

// The emitters' common body (0x8222CB90, 0x8222D0E8, 0x8222AB88, 0x8220E2B0): the set bits
// of `mask`, most significant first, as runs of registers starting at `reg`, their data
// read from `data`. Returns the ring pointer.
u32 EmitRuns(const GuestFrame& f, Device d, u32 ring, u64 mask, u32 reg, u32 data, const RunStyle& style) {
  u32 p = data - 4;  // the pre-incremented read pointer
  const auto leading = [](u64 value) { return value ? u32(__builtin_clzll(value)) : 64u; };
  while (mask) {
    const u32 skip = leading(mask);
    mask <<= skip;
    const u32 run = leading(~mask);
    p += skip * style.words * 4;
    const u32 this_reg = reg + skip * style.stride;
    const u32 words = run * style.words;
    if (ring + (words + style.extra) * 4 < d.W(0x34)) {
      if (style.align == RunStyle::kVector) {
        const u32 pads = (ring & 0xF) == 4 ? 1 : (ring & 0xF) == 0 ? 2 : (ring & 0xF) == 0xC ? 3 : 0;
        for (u32 i = 0; i < pads; ++i) {
          ring += 4;
          f.Put(ring, 0x80000000u);
        }
      } else if (style.align == RunStyle::kPad8) {
        f.Put(ring + 4, 0x80000000u);
        ring += ring & 7;
      }
      ring += 4;
      f.Put(ring, ((words - 1) << 16) | this_reg);
      for (u32 bit = 0; bit < run; ++bit) {
        if (style.align == RunStyle::kVector) {
          // lvx128 / stvx128 ignore the low four bits of their addresses
          for (u32 piece = 0; piece < 4; ++piece)
            std::memcpy(f.G((ring + 4 + 16 * piece) & ~0xFu), f.G((p + 4 + 16 * piece) & ~0xFu), 16);
        } else {
          std::memmove(f.G(ring + 4), f.G(p + 4), style.words * 4);
        }
        p += style.words * 4;
        ring += style.words * 4;
      }
      mask <<= run;
    } else {
      ring = AppendRegisters(f, d, ring, this_reg, p, words, style.per);
      p += words * 4;
      mask <<= run;
    }
    reg = this_reg + words;
  }
  return ring;
}

// sub_8222CB90(device, mask, register, data): single words.
bool EmitRegisterWords(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x90);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const RunStyle style{1, 1, 1, 0, RunStyle::kNone};
  d.SetW(0x30, EmitRuns(f, d, d.W(0x30), ctx.r4.u64, ctx.r5.u32, ctx.r6.u32, style));
  return true;
}

// sub_8222D0E8(device, mask, register, data): 16-word vectors of constants.
bool EmitConstantVectors(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xB0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const RunStyle style{16, 16, 0x10, 3, RunStyle::kVector};
  d.SetW(0x30, EmitRuns(f, d, d.W(0x30), ctx.r4.u64, ctx.r5.u32, ctx.r6.u32, style));
  return true;
}

// sub_8220E2B0(device, mask): the clip planes the mask names (4 words each at +0x2820), after
// the packets that select them.
bool EmitClipPlanes(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xA0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  u32 ring = RingForPacket(f, d);
  for (const u32 word : {0x2007u, d.W(0x289C), 0xA31u, 0x10000u, 0x10A2Fu, 0u, 0x1000u, 0xC0043C00u, 3u, 0xA31u,
                         0u, 0x80000000u, 8u}) {
    ring += 4;
    f.Put(ring, word);
  }
  d.SetW(0x30, ring);
  const RunStyle style{4, 4, 4, 1, RunStyle::kPad8};
  d.SetW(0x30, EmitRuns(f, d, ring, ctx.r4.u64, 0x2388, d.va + 0x2820, style));
  return true;
}

// sub_8222AB88(device, mask): the ALU constants the mask names (6 words a bit, from +0x480),
// then the packet that ends the block. The original's two exits write slightly different
// trailers; this writes the common one.
bool EmitConstantBlocks(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x90);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const RunStyle style{6, 6, 6, 5, RunStyle::kPad8};
  u32 ring = EmitRuns(f, d, d.W(0x30), ctx.r4.u64, 0x4800, d.va + 0x480, style);
  d.SetW(0x30, ring);
  if (ring > d.W(0x38))
    ring = f.Call(sub_82210AB0, {d.va});
  f.Put(ring + 4, 0x25000u);
  f.Put(ring + 8, 0u);
  f.Put(ring + 12, 0u);
  f.Put(ring + 16, 0u);
  d.SetW(0x30, ring + 16);
  return true;
}

// sub_8226AAB8(device, mask, hi control): the depth/stencil and render-target state the mask
// names, as register packets (the per-tile predicated ones when the device draws into the
// tiles of a command buffer). Returns the mask less what it wrote.
bool EmitSurfaceState(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0xC0);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  u64 mask = ctx.r4.u64;                 // r24
  u32 hi_control = ctx.r5.u32;           // r28
  u32 ring = RingForPacket(f, d);        // r11
  u32 stencil_mark = ~0u, surface_mark = ~0u, color_mark = ~0u;  // r26, r23, r20
  constexpr u32 kPredicateSet = 0xC0006100u, kPredicateTest = 0xC0006000u, kRegisterBlock = 0xC0012D01u,
                kBlockSize = 0x40203u;
  const auto put = [&](u32 word) {
    ring += 4;
    f.Put(ring, word);
  };
  const auto room = [&] {
    d.SetW(0x30, ring);
    if (ring > d.W(0x38))
      ring = f.Call(sub_82210AB0, {d.va});
  };
  if (mask & 0x100) {
    bool tiled = (d.B(0x2ABF) & 0x20) && TilingBound(d);
    if (tiled) {
      put(kPredicateSet);
      put(0);
      u32 last = f.Get(f.sp + 0x50);  // (uninitialised in the original when there are no tiles)
      const i32 tiles = i32(d.W(0x31CC));
      for (i32 tile = tiles - 1; tile >= 0; --tile) {
        const u32 index = d.W(0x3338 + 4 * u32(tile));
        hi_control = (std::rotl(index, 17) & 0xFFFE0000u) | (hi_control & ~0xFFFE0000u);
        last = hi_control;
        const u32 mode = (d.W(0x2E4C) >> 17) & 7;
        const bool clear_low =
            mode == 0 || (mode == 2 && (tile != 0 || !(d.B(0x2ABF) & 0x10)));
        if (clear_low)
          last = hi_control & ~1u;
        put(kPredicateTest);
        put(tile * 2 < 32 ? 3u << (tile * 2) : 0u);
        put(kRegisterBlock);
        put(kBlockSize);
        put(last);
        room();
      }
      if (last != hi_control && (d.B(0x2ABC) & 0x40)) {
        put(kPredicateTest);
        put(1);
        put(kRegisterBlock);
        put(kBlockSize);
        put(hi_control);
      }
      put(kPredicateTest);
      put(d.W(0x31A4));
      put(kPredicateSet);
      put(d.W(0x31A8));
    } else {
      tiled = false;
    }
    if (!tiled) {
      u32 value = hi_control;
      if ((d.W(0x2E4C) & 0xE0000) == 0)
        value = hi_control & ~1u;
      put(0x2203);
      put(value);
      if (value != hi_control && (d.B(0x2ABC) & 0x40))
        stencil_mark = hi_control;
    }
    mask &= ~u64(0x100);
  }
  const u64 pending = d.Q(0x28) & mask;
  if (pending & Bit(57)) {
    put(0x2000);
    const u32 info = d.W(0x2880);
    put(info);
    mask &= ~Bit(57);
    surface_mark = (d.W(0x3378) & 0x3FFFu) | (info & 0xFFFFC000u);
  }
  if (pending & Bit(37)) {
    put(0x2104);
    color_mark = 0;
    put(d.W(0x28DC));
    mask &= ~Bit(37);
  }
  if ((color_mark & surface_mark & stencil_mark) != ~0u) {
    put(kPredicateSet);
    put(0);
    put(kPredicateTest);
    put(0x15555555u);
    if (stencil_mark != ~0u) {
      put(kRegisterBlock);
      put(kBlockSize);
      put(stencil_mark);
    }
    if (surface_mark != ~0u) {
      put(kRegisterBlock);
      put(0x40000u);
      put(surface_mark);
    }
    if (color_mark != ~0u) {
      put(kRegisterBlock);
      put(0x40104u);
      put(color_mark);
    }
    put(kPredicateTest);
    put(d.W(0x31A4));
    put(kPredicateSet);
    put(d.W(0x31A8));
  }
  d.SetW(0x30, ring);
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




// sub_82663208(device): the register shadow back to its power-on values, and the packets
// that put the same into the GPU's registers.
bool ResetDeviceRegisters(PPCContext& ctx, uint8_t* base) {
  GuestFrame f(ctx, base, 0x70);
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
  u32 ring = RingForPacket(f, d);
  const u32 g1 = f.Get(0x82FAD21C), g2 = f.Get(0x82FAD220);
  const u32 flush_depth = (d.W(0x5DAC) & 2) ? 0xFu : 3u;
  for (const u32 word : {0xD01u, 0x04000000u, 0xC0022100u, 0x81u, 0xFFFFFFFFu, g1 | 0x80010000u, 0xC0022100u, 0x82u,
                         0xFFFFFFFFu, g2, 0xE42u, 0x1F60u, 0xC85u, flush_depth, 0x57Cu, 0x0BADF00Du, 0x57Bu, 0u}) {
    ring += 4;
    f.Put(ring, word);
  }
  d.SetW(0x30, ring);
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
  u32 ring = RingForPacket(f, d);
  d.SetW(0x30, f.Call(sub_822052C8, {d.va, ring, 0, 0x8265C418, context}));
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
  SetHandler(kD3D_EmitRegisterWords, EmitRegisterWords);
  SetHandler(kD3D_EmitConstantVectors, EmitConstantVectors);
  SetHandler(kD3D_EmitClipPlanes, EmitClipPlanes);
  SetHandler(kD3D_EmitConstantBlocks, EmitConstantBlocks);
  SetHandler(kD3D_FrontBufferFormatPacket, FrontBufferFormatPacket);
  SetHandler(kD3D_ResetAllState, ResetAllState);
  SetHandler(kD3D_SetupTileExtent, SetupTileExtent);
  SetHandler(kD3D_ExportBegin, ExportBegin);
  SetHandler(kD3D_ExportEnd, ExportEnd);
  SetHandler(kD3D_MarkEdramBits, MarkEdramBits);
  SetHandler(kD3D_EndTilingLists, EndTilingLists);
}

}  // namespace renut::d3d::port
