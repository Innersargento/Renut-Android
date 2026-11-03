// D3DDevice_BeginCommandBuffer, EndCommandBuffer and RunCommandBuffer, natively (see
// command_buffers.h).
//
// Translated from this executable's own code (sub_82657CB0, sub_826582A0 and sub_82222888, read off
// the recompiled code), not from reNut's; the offsets are this release's. The native front end has opened the
// buffer's host list (Begin) or closes it (End) before these run: the draws a recording device makes
// are recorded on the host, and the buffer's memory never holds what the GPU would run.
//
// What is left out of the originals is what builds that memory:
//  * Begin does not reset the buffer's allocator, does not start the first segment (the kick that
//    would allocate it) and does not set up the ring allocation list (the GPU's own, 0x1000);
//  * End does not chain the segment into the buffer's call list, does not finish the allocation list
//    and does not flush the dirty state as packets: the front end has recorded that state, and the
//    pending masks are cleared as the packets' writer clears them.
// The ring of a device recording is the sink, as for every device (sync.cpp), so the sizes End
// reads of it (the segment start, +14928) are the ones the sink gives. The recompiled Run walks the
// buffer's call list (+116) in both of its modes without a null test, so End still names one empty
// node there (EmptyCallList) for a Run that is not this one: without it the first Run read guest
// address 4 over and over and the game froze at the first 3D. Run itself keeps the device's
// bookkeeping and leaves the packets out (see its comment).
//
// All three leave the original to run when the cvar is off, when the stub of the recording ring (the
// stub_command_buffers cvar of sync.cpp) is off, or for what they do not cover.
#include "d3d/scene/command_buffers.h"

#include "d3d/frontend/native_d3d.h"
#include "d3d/hooks/layer.h"
#include "d3d/device/guest_frame.h"
#include "d3d/device/internal.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, renut_stub_command_buffers);
REXCVAR_DEFINE_INT32(renut_command_buffer_lists_ahead, 32, "reNut",
                     "The brake of BeginCommandBuffer: a thread that starts recording a buffer waits (at most "
                     "50 ms) while more than this many published lists are waiting for the GPU backend. The "
                     "console made it wait for the buffer's own previous run; the buffers are copies on the host, "
                     "so this bounds what piles up instead. 0: no brake")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(renut_native_command_buffers, true, "reNut",
                    "BeginCommandBuffer and EndCommandBuffer natively: the buffer's memory (segments, "
                    "allocator, allocation list) is not built, the draws are recorded on the host. Off: "
                    "the recompiled functions. Needs renut_stub_command_buffers");

extern "C" {
REX_FUNC(sub_82223270);  // D3D::SetSurfaceClip (hooked: the front end records the clip)
__attribute__((weak)) void sub_82658818(PPCContext& __restrict, uint8_t*);  // the one-pass Z pass set-up
__attribute__((weak)) void sub_8264E1A8(PPCContext& __restrict, uint8_t*);  // unbinds the targets, shaders, streams
}

namespace renut::d3d::port {
namespace {

// D3DCommandBuffer fields.
constexpr u32 kBufferFence = 8;
constexpr u32 kBufferTags = 24;        // the tags it inherits (5 qwords)
constexpr u32 kBufferSetTags = 64;     // its copy
constexpr u32 kBufferFlags = 108;      // low nibble: state (2 recording, 1 done, 7 out of memory)
constexpr u32 kBufferCallList = 116;  // the chain of call nodes Run walks
constexpr u32 kBufferAllocationList = 120;
constexpr u32 kBufferEnd = 136, kBufferLimit = 140, kBufferEndGpu = 144;
constexpr u32 kBufferAllocatorSize = 168;

// Device fields, this release's.
constexpr u32 kRingWrite = 48, kRingLimit = 52;
constexpr u32 kTagMask = 10912;          // the tags a buffer's state is written for (CommandBufferTagMask)
constexpr u32 kPredicationSelect = 10932;  // TilePredication
constexpr u32 kRunPredicationSelect = 10936;
constexpr u32 kTileCount = 12748;
constexpr u32 kTileRects = 12752;        // 16 bytes each
constexpr u32 kTileOrigins = 12992;      // 8 bytes each
constexpr u32 kDepthControl = 10548;
constexpr u32 kSurfaceInfo = 10368;
constexpr u32 kWindowTopLeft = 10436, kWindowBottomRight = 10440;
constexpr u32 kTextureFetch = 1152, kTextureFetchBytes = 768;
constexpr u32 kAllocationList = 13248;   // the ring allocation list head (the GPU's, with 0x1000)
constexpr u32 kAllocatorSizeCopy = 14912;
constexpr u32 kMaxTiles = 16;
constexpr u32 kOutOfMemoryHresult = 0x8007000E;

// The call list RunCommandBuffer (sub_82222888) walks: the buffer's +116 points at a chain of nodes
// {next, count, count x {size, address}}, in either of its two modes, and it reads the first node
// without a null test. The segments the original builds there are not built, so every ended buffer
// names this one node, with one empty call: Run writes an empty indirect call for it into the ring
// (the sink) and finds the end of the chain. Shared, as nothing writes to it.
u32 EmptyCallList() {
  static std::once_flag made;
  static u32 node = 0;
  std::call_once(made, [] {
    auto* memory = rex::system::kernel_memory();
    node = memory ? memory->SystemHeapAlloc(64, 16, rex::memory::kSystemHeapPhysical) : 0;
    if (!node) return;
    u8* bytes = renut::GuestPtr(memory->virtual_membase(), node);
    std::memset(bytes, 0, 64);
    StoreBE32(bytes + 4, 1);  // one call, {0, 0}
  });
  return node;
}

bool Enabled() {
  return REXCVAR_GET(renut_native_command_buffers) && REXCVAR_GET(renut_stub_command_buffers) &&
         sub_82658818 && sub_8264E1A8 && EmptyCallList();
}

// sub_82223270's arguments: a signed 16-bit field of the window scissor words.
i32 LowHalf(u32 word) { return i32(word << 17) >> 17; }
i32 HighHalf(u32 word) { return i32(word << 1) >> 17; }

// The window scissor, when the tags (the last word of a buffer's) do not say the buffer carries
// its own: SetSurfaceClip with the device's two scissor words split into signed halves. The front
// end records the clip from its hook on that function.
void SetWindowClip(const GuestFrame& f, Device d, u64 last_tags) {
  if ((last_tags & (u64(7) << 46)) == (u64(7) << 46)) return;
  const u32 top_left = d.W(kWindowTopLeft), bottom_right = d.W(kWindowBottomRight);
  f.Call(sub_82223270, {d.va, u32(LowHalf(top_left)), u32(HighHalf(top_left)), u32(LowHalf(bottom_right)),
                        u32(HighHalf(bottom_right))});
}

// D3DDevice_BeginCommandBuffer(device, buffer, flags, inheritTags, persistTags, rects, count)
// (sub_82657CB0): this release's has the persist tags that reNut's (title update 3) dropped; they
// are not read.
bool BeginCommandBuffer(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return false;
  Device d = DeviceAt(base, ctx.r3.u32);
  const u32 buffer = ctx.r4.u32, flags = ctx.r5.u32, inherit = ctx.r6.u32;
  const u32 rects = ctx.r8.u32, count = ctx.r9.u32;
  if (!d || !buffer || count > kMaxTiles) return false;
  GuestFrame f(ctx, base, 0x40);
  Device g = GuestOnly(base);  // guest addresses as they are
  const u32 dev = d.va;

  g.SetW(buffer + kBufferFlags, Insert(g.W(buffer + kBufferFlags), 2, 0, 4));
  // The brake. The console waited here for the buffer's last run to finish, because the GPU read the
  // buffer's memory; here the buffer is a copy on the host, a Run never marks it busy (below), and what
  // is left to hold a thread that records is the backend: not too many lists ahead of it.
  if (const int ahead = REXCVAR_GET(renut_command_buffer_lists_ahead))
    renut::native_d3d::WaitForBackend(u32(ahead), 50);
  // The buffer's fence is cleared, not waited for: nothing writes one any more (the original Run did,
  // the native one does not), and a stale one from a Run of the original would only make
  // D3DResource_Destroy wait for a buffer that is a copy on the host.
  g.SetW(buffer + kBufferFence, 0);
  // (The original resets the buffer's allocator here when the buffer has no fixed memory.)

  d.SetW(off::CommandBuffer, buffer);
  g.SetW(buffer + kBufferFlags, g.W(buffer + kBufferFlags) & 0xFFFFFF8Fu);
  g.SetW(buffer + 124, 0);
  g.SetW(buffer + 128, 0);
  g.SetW(buffer + 132, 0);
  g.SetW(buffer + 112, 0);
  g.SetW(buffer + 144, 0);
  g.SetW(buffer + 116, 0);
  g.SetW(buffer + kBufferAllocationList, 0);
  d.SetB(off::Flags1, d.B(off::Flags1) & 0xDF);  // not out of memory
  d.SetW(14916, 0);
  d.SetW(kAllocatorSizeCopy, g.W(buffer + kBufferAllocatorSize));
  for (u32 word : {13520u, 13524u, 13528u, 13532u, 13536u, 13540u}) d.SetW(word, 0);
  // The kick that starts the first segment: the ring is the sink.
  renut::native_d3d::RewindRecordingRing(base, dev);

  // The tags the buffer inherits (none unless given) and its copy.
  for (u32 i = 0; i < 5; ++i)
    g.SetQ(buffer + kBufferTags + i * 8, inherit ? g.Q(inherit + i * 8) : 0);
  for (u32 i = 0; i < 5; ++i)
    g.SetQ(buffer + kBufferSetTags + i * 8, g.Q(buffer + kBufferTags + i * 8));

  u32 state = g.W(buffer + kBufferFlags) & ~0x80u & ~0x8000u;
  state |= ((((flags << 3) & 0x100) | ((flags >> 4) & 1)) << 7);
  if (flags & 8) {
    d.SetW(kTagMask, 0xFFFFFFFFu);
    state |= 0x400;
  } else {
    d.SetW(kTagMask, 0x400000);
    state &= ~0x400u;
  }
  g.SetW(buffer + kBufferFlags, state);
  g.SetW(buffer + kBufferAllocationList, 0);
  g.SetW(buffer + kBufferFlags, state & ~0x1800u & ~0x20000u);

  d.SetB(off::Flags, d.B(off::Flags) & 0x87);
  d.SetB(off::Flags3, d.B(off::Flags3) & 0xCF);
  d.SetQ(off::PredicatedPending2, 0);

  if (flags & 3) {
    if (flags & 2) {
      g.SetW(buffer + kBufferFlags, g.W(buffer + kBufferFlags) | 0x800);
      d.SetB(off::Flags, d.B(off::Flags) | 8);
    } else {
      g.SetW(buffer + kBufferFlags, g.W(buffer + kBufferFlags) | 0x1000);
      // The allocation list is the GPU's: an empty head, no list (the original starts one).
      d.SetW(kAllocationList + 16, dev);
      d.SetW(kAllocationList + 4, 0);
      d.SetW(kAllocationList + 8, 0);
      d.SetW(kAllocationList + 12, 0);
      d.SetW(kAllocationList, 0);
    }
    d.SetB(off::Flags, d.B(off::Flags) | 0x30);
    d.SetW(kTileCount, count);
    u32 extent_x = 0, extent_y = 0;
    for (u32 i = 0; i < count; ++i) {
      const u32 x1 = g.W(rects + 16 * i), y1 = g.W(rects + 16 * i + 4);
      const u32 x2 = g.W(rects + 16 * i + 8), y2 = g.W(rects + 16 * i + 12);
      if (!(extent_x > x2)) extent_x = x2;
      if (!(extent_y > y2)) extent_y = y2;
      d.SetW(kTileRects + 16 * i, x1);
      d.SetW(kTileRects + 16 * i + 4, y1);
      d.SetW(kTileRects + 16 * i + 8, x2);
      d.SetW(kTileRects + 16 * i + 12, y2);
      d.SetW(kTileOrigins + 8 * i, x1 & ~31u);
      d.SetW(kTileOrigins + 8 * i + 4, y1 & ~31u);
    }
    d.SetW(13180, extent_x);
    d.SetW(13184, extent_y);
    d.SetW(13188, 0);
    d.SetW(12720, 2);
    if (flags & 0x40) {
      f.Call(sub_82658818, {dev, (flags >> 7) & 1, count});
      g.SetW(buffer + kBufferFlags, g.W(buffer + kBufferFlags) | 0x20000);
    }
  }

  if (flags & 4) {
    d.SetB(off::Flags, d.B(off::Flags) | 0x40);
    d.SetW(12704, Extract(d.W(kDepthControl), 4, 3));
    if (d.B(off::Flags3) & 0x20) {
      u64 predicated = d.Q(off::PredicatedPending2) | Bit(37);
      if (d.W(13176) != (d.W(kSurfaceInfo) & 0x3FFF)) predicated |= Bit(57);
      d.SetQ(off::PredicatedPending2, predicated);
    }
  }

  d.SetW(off::PredicationSet, 0);
  RefreshTilePredicated(d);
  d.SetW(off::Predication, 0xFFFFFFFFu);
  d.SetW(off::RunPredicationState, 0);
  d.SetW(kPredicationSelect, 0);
  d.SetW(kRunPredicationSelect, 0);
  std::memset(d.bytes + kTextureFetch, 0, kTextureFetchBytes);

  // A buffer that does not inherit the window scissor sets its own from the device's.
  SetWindowClip(f, d, g.Q(buffer + kBufferTags + 32));
  // Everything the buffer does not inherit is dirty in it.
  for (u32 i = 0; i < 5; ++i) d.SetPending(i, ~g.Q(buffer + kBufferTags + i * 8));
  ctx.r3.u64 = 0;
  return true;
}

// D3DDevice_EndCommandBuffer(device) (sub_826582A0).
bool EndCommandBuffer(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return false;
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d) return false;
  const u32 buffer = d.W(off::CommandBuffer);
  if (!buffer) return false;
  GuestFrame f(ctx, base, 0x40);
  Device g = GuestOnly(base);
  const u32 dev = d.va;

  // The dirty state the buffer would have written at its end (the flag is the buffer's, 0x8000): the
  // front end has recorded it, and the masks are clear when the packets are written. Then the device
  // lets go of what it had bound.
  if (g.W(buffer + kBufferFlags) & 0x8000) {
    for (u32 i = 0; i < 5; ++i) d.SetPending(i, 0);
    f.Call(sub_8264E1A8, {dev});
  }
  // The last kick: the ring starts over in the sink.
  renut::native_d3d::RewindRecordingRing(base, dev);
  if (g.W(buffer + kBufferFlags) & 0x1000) g.SetW(buffer + kBufferAllocationList, 0);
  d.SetW(off::CommandBuffer, 0);
  std::atomic_thread_fence(std::memory_order_seq_cst);  // sync

  const u32 end = d.W(kRingWrite) + 4;
  g.SetW(buffer + kBufferEnd, end);
  g.SetW(buffer + kBufferLimit, d.W(kRingLimit));
  g.SetW(buffer + kBufferEndGpu, GpuAddress(end) - 0x40000000u);
  g.SetW(buffer + kBufferCallList, EmptyCallList());
  const bool out_of_memory = (d.B(off::Flags1) & 0x20) != 0;
  g.SetW(buffer + kBufferFlags, Insert(g.W(buffer + kBufferFlags), out_of_memory ? 7 : 1, 0, 4));
  // CDevice::MarkAsOutOfMemory (sub_82657160), stubbed as reNut's End does: the original points the
  // ring at the device's side buffer so nothing is written until the next Begin; the ring is the sink
  // here (rewound above), so only the flag is left, which Begin clears.
  d.SetB(off::Flags1, d.B(off::Flags1) | 0x20);
  ctx.r3.u64 = out_of_memory ? kOutOfMemoryHresult : 0;
  return true;
}

// D3DDevice_RunCommandBuffer(device, buffer, predication) (sub_82222888). The front end has
// recorded the buffer's list into the device's (and flushed the state the buffer inherits); what the
// original does besides writing packets is the device's bookkeeping, which stays:
//  * the dirty words the buffer inherits are cleared (the original writes that state out first);
//  * the buffer is NOT marked busy: the original writes the fence the device is at into its +8, which
//    the next Begin waits for and the resource code reads (D3DResource_Destroy waits for it before
//    freeing); the buffer is a copy on the host, so nothing is left to wait for (reNut's Run does not
//    write it either, nor does its Begin find one). The brake is Begin's;
//  * the window scissor is set again unless the buffer carries its own, and what the buffer set
//    is dirty on the device (the complement of its set tags).
// Left out are the packets: the indirect calls for the buffer's segments (its call list, +116, whose
// walk it did in two modes), the preamble and predication words, the ring kicks they brought and the
// bookkeeping of the lists a ring replay would use (the device's own allocation lists, 13348 and
// 13248, and a recording device's call list: Run inside a recording is recorded by the front end).
bool RunCommandBuffer(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return false;
  Device d = DeviceAt(base, ctx.r3.u32);
  const u32 buffer = ctx.r4.u32;
  if (!d || !buffer) return false;
  GuestFrame f(ctx, base, 0x40);
  Device g = GuestOnly(base);

  for (u32 word = 0; word < 5; ++word) {
    if (g.Q(buffer + kBufferTags + word * 8) & d.Pending(word)) d.SetPending(word, 0);
  }
  const u64 set_last = g.Q(buffer + kBufferSetTags + 32);
  SetWindowClip(f, d, set_last);
  for (u32 word = 0; word < 4; ++word) d.SetPending(word, ~g.Q(buffer + kBufferSetTags + word * 8));
  d.SetPending(4, ~set_last);
  return true;
}

}  // namespace

bool NativeCommandBuffers() { return Enabled(); }

void RegisterCommandBuffers() {
  SetHandler(kD3DDevice_BeginCommandBuffer, BeginCommandBuffer);
  SetHandler(kD3DDevice_EndCommandBuffer, EndCommandBuffer);
  SetHandler(kD3DDevice_RunCommandBuffer, RunCommandBuffer);
}

}  // namespace renut::d3d::port
