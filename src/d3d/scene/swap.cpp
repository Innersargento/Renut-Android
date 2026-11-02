// D3DDevice_Swap natively (see swap.h).
//
// Offsets are this release's, read off the retail disassembly (not through Layout()).
// The native front end has recorded the frame (native_d3d.cpp, Swap) before this runs. The
// packets the original queues in the ring (the history slots' counter stamps, the front buffer's
// address and format, the display mode register) are left out: the ring is a stub nothing reads,
// and the present record (VdSwap, RecordPresent) carries the frame. What stays is the device's
// swap state, the history slots, the scaler and the calls into the kernel's video code.
#include "d3d/scene/swap.h"

#include <array>
#include <cstring>

#include "d3d/frontend/native_d3d.h"
#include "d3d/hooks/layer.h"
#include "d3d/device/device.h"
#include "d3d/device/guest_frame.h"
#include "d3d/device/retail.h"

#include <rex/ppc/context.h>

extern "C" {
REX_FUNC(sub_82210AB0);  // CDevice::KickOff
REX_FUNC(sub_8228A748);  // D3D::SynchronizeToPresentationInterval
REX_FUNC(sub_8265F688);  // D3DDevice_SetScalerParameters
REX_FUNC(sub_822873E8);  // CDevice::StartNewSegment
REX_FUNC(sub_8228F558);  // CDevice::BlockOnFence
REX_FUNC(sub_8264DCD0);  // D3D_CBlocker_Check
__attribute__((weak)) void sub_82D0827C(PPCContext& __restrict, uint8_t*);  // the device type
__attribute__((weak)) void sub_821EF4E8(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82D0820C(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82D0821C(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82D08B2C(PPCContext& __restrict, uint8_t*);  // the video mode
__attribute__((weak)) void sub_82D08B6C(PPCContext& __restrict, uint8_t*);  // query the video mode
__attribute__((weak)) void sub_821ED560(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82D08A6C(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82D088EC(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_82287D38(PPCContext& __restrict, uint8_t*);
__attribute__((weak)) void sub_8264DC08(PPCContext& __restrict, uint8_t*);  // CBlocker cleanup
}

namespace renut::d3d::port {
namespace {

struct Frame : GuestFrame {
  using GuestFrame::GuestFrame;

  u32 Kick(u32 device) const { return Call(sub_82210AB0, {device}); }
};

u32 GpuOf(u32 address) {
  return (((address >> 20) + 0x200) & 0x1000) + (address & 0x1FFFFFFF);
}

// rlwimi rA,rS,sh,mb,me with the mask given.
u32 InsertMasked(u32 target, u32 source, u32 rotate, u32 mask) {
  const u32 rotated = rotate ? (source << rotate) | (source >> (32 - rotate)) : source;
  return (rotated & mask) | (target & ~mask);
}

bool HaveAll() {
  return sub_82D0827C && sub_821EF4E8 && sub_82D0820C && sub_82D0821C &&
         sub_82D08B2C && sub_82D08B6C && sub_821ED560 &&
         sub_82D08A6C && sub_82D088EC && sub_82287D38 &&
         sub_8264DC08;
}

// D3DDevice_Swap(device, front buffer, scaler parameters)
bool Swap(PPCContext& ctx, uint8_t* base) {
  if (!HaveAll())
    return false;
  Frame f(ctx, base, 0x230);
  Device d = DeviceAt(base, ctx.r3.u32);
  if (!d)
    return false;
  const u32 front = ctx.r4.u32, parameters = ctx.r5.u32;
  const u32 dev = d.va;
  const bool hardware = f.Call(sub_82D0827C) != 2;  // r22

  d.SetW(0x40B0, d.W(0x40B0) + 1);
  std::memcpy(f.G(f.sp + 0xA0), f.G(front + 0x1C), 0x18);
  const u32 format = d.W(0x3528);
  u32 word1 = f.Get(f.sp + 0xA4);
  switch (word1 & 0x3F) {
    case 0x32:
      word1 = InsertMasked(word1, 3, 1, 0x3F);
      f.Put(f.sp + 0xA4, word1);
      break;
    case 7:
      word1 = InsertMasked(word1, 0x1B, 1, 0x3F);
      f.Put(f.sp + 0xA4, word1);
      break;
    default:
      break;
  }
  u32 size_word = format >> 17;  // r30
  const u32 word_ac = f.Get(f.sp + 0xAC);
  u32 word0 = f.Get(f.sp + 0xA0);
  word0 = InsertMasked(word0, format, 25, 0x3FC);
  const u32 merged = InsertMasked(size_word, word_ac, 0, 0xFFFFE001u);
  f.Put(f.sp + 0xA0, word0);
  size_word = InsertMasked(size_word, merged, 0, 0xFFFFFFFEu);
  std::memcpy(f.G(dev + 0x39FC), f.G(front), 0x34);
  f.Put(f.sp + 0xAC, size_word);

  const u32 word_a8 = f.Get(f.sp + 0xA8);
  u32 bump = (((size_word << 2) | (size_word >> 30)) & 2) + 1;
  const u32 scan_format = d.W(0x352C);  // r10 / r25
  f.Put(f.sp + 0x68, scan_format);
  f.Put(f.sp + 0x60, (word_a8 & 0x1FFF) + bump);
  f.Put(f.sp + 0x70, word1 & ~0xFFFu);
  f.Put(f.sp + 0x6C, word1 & 0x3F);
  f.Put(f.sp + 0x64, ((word_a8 >> 13) & 0x1FFF) + bump);
  bool slot_written = false;  // r29

  if (d.W(0x5480) != 0 && d.W(0x547C) - d.W(0x5478) < 6) {
    const u32 block = d.W(0x2A90);
    f.Put(block + (((d.W(0x547C) & 7) + 0x10) << 2), 0);
    f.Put(block + ((((d.W(0x547C) + 1) & 7) + 0x10) << 2), 0);
    // (The original also queues a packet that stamps the first slot with the GPU's counter; the
    // present record stamps both slots when the frame is presented, see RecordPresent.)
    slot_written = true;
  }

  const bool scaler_changed =
      parameters != 0 && std::memcmp(f.G(parameters), f.G(dev + 0x3548), 0x1C) != 0;  // r26

  if (hardware) {
    if (scaler_changed)
      d.SetW(0x5D94, d.W(0x5D94) | 0x40000000u);
    f.Call(sub_821EF4E8, {dev, front});
  }

  f.Call(sub_82D0820C, {f.sp + 0xD0, f.sp + 0x74});
  if (d.W(0x541C)) {
    f.Put(d.W(0x2A90) + 8, f.Get(f.sp + 0x74));
  } else {
    // VdSwap takes the buffer its swap packet goes to. Nothing reads the ring, so the packet goes
    // to the scratch block (the present is recorded from VdSwap's arguments, RecordPresent) and
    // the ring is left alone.
    const u32 sink = renut::native_d3d::RingSinkAddress();
    const u32 buffer = sink ? sink : d.W(0x30);
    f.Put(f.sp + 0x54, f.sp + 0x60);
    f.Put(f.sp + 0x5C, f.sp + 0x64);
    f.Call(sub_82D0821C, {buffer + 4, f.sp + 0xA0, d.W(0x2A90) + 8, f.sp + 0xD0, f.Get(f.sp + 0x74),
                          f.sp + 0x70, f.sp + 0x6C, f.sp + 0x68});
  }
  d.SetW(0x5B34, f.Get(f.sp + 0xD4));

  if (slot_written)
    d.SetW(0x547C, d.W(0x547C) + 2);
  if (const u32 value = f.Get(f.sp + 0xD8))
    d.SetW(0x5DA8, value);
  u32 changes = d.W(0x4144);  // r27
  d.SetW(0x4144, 0);

  if (changes & 8) {
    if (hardware) {
      const u32 kind = d.W(0x3528) & 0x3F;
      u32 mode = 0x40000000u | ((d.W(0x352C) & 3) << 28);
      if (kind == 7 || kind == 0x36)
        mode |= 0x08000000;
      if (d.W(0x34E8) == 0x2D0) {
        const u32 height = d.W(0x34EC);
        if (height == 0x1E0 || height == 0x240)
          mode |= 2;
      }
      f.Call(sub_82D08B2C, {mode});
    }
    f.Call(sub_82D08B6C, {f.sp + 0x170});
    d.SetW(0x5428, LoadBE16(f.G(f.sp + 0x1B8)));
    d.SetW(0x542C, LoadBE16(f.G(f.sp + 0x1BA)));
    d.SetW(0x5430, LoadBE16(f.G(f.sp + 0x1C6)));
    f.Kick(dev);
  }

  if (scaler_changed) {
    std::memcpy(f.G(dev + 0x3548), f.G(parameters), 0x1C);
    f.Call(sub_8265F688, {dev, parameters});
  }

  const u32 front_gpu = GpuOf(f.Get(f.sp + 0x70));  // r29
  u32 stored = 0, flags_value = 0;                    // r28, r30 and r26 below
  u32 r26 = 0, r28, r30;
  if (f.Get(f.sp + 0xD8) != 0 && f.Get(f.sp + 0x68) == scan_format) {
    r28 = dev + 0x3564;
    r30 = 0x28280000u | (f.Get(f.sp + 0x6C) == 7 ? 0x136 : 0x106);
    bool take_new = false;
    if (f.Get(f.sp + 0xEC) != 0)
      take_new = std::memcmp(f.G(f.sp + 0xF0), f.G(dev + 0x3564), 0x38) != 0;
    if (take_new) {
      std::memcpy(f.G(dev + 0x3564), f.G(f.sp + 0xF0), 0x38);
      d.SetB(0x2ABE, d.B(0x2ABE) | 0x10);
      changes |= 4;
    } else if ((d.B(0x2ABE) & 0x10) && f.Get(f.sp + 0x160) == 0) {
      r28 = dev + 0x359C;
      changes |= 4;
    }
    u8 flags3 = d.B(0x2ABF);
    if (!(flags3 & 0x40))
      changes |= 3;
    d.SetB(0x2ABF, flags3 | 0x40);
  } else {
    r28 = dev + 0x359C;
    r30 = d.W(0x3528);
    r26 = dev + 0x3A9C;
    if (d.B(0x2ABE) & 0x10) {
      std::memset(f.G(dev + 0x3564), 0, 0x38);
      changes |= 4;
      d.SetB(0x2ABE, d.B(0x2ABE) & 0xEF);
    }
    u8 flags3 = d.B(0x2ABF);
    if (flags3 & 0x40)
      changes |= 3;
    d.SetB(0x2ABF, flags3 & 0xBF);
  }
  (void)stored;
  (void)flags_value;

  if (f.Get(0x82FAD20C) != 0 || (d.B(0x2ABD) & 2))
    changes = 0;

  if (changes != 0) {
    // sub_82654598 writes the display registers, the front buffer's address, the gamma table and
    // the scaler's command buffer into the ring: nothing reads it, and the display is the host's.
    // What it also does is wait for the presentation interval, and kick.
    if (d.W(0x5DAC) & 4)
      f.Call(sub_8228A748, {dev, 0});
    f.Kick(dev);
  } else if (d.W(0x5DAC) & 4) {
    f.Call(sub_8228A748, {dev, front_gpu});
  }
  // (Otherwise the original queues the front buffer's address and format as packets.)

  f.Put(f.sp + 0x54, r26);
  f.Call(sub_821ED560, {dev, changes, f.Get(f.sp + 0x68), front_gpu, r30, f.Get(f.sp + 0x60),
                        f.Get(f.sp + 0x64), r28});

  if ((changes & 8) && hardware) {
    // (The original queues a register write here; the display mode is the kernel's.)
    std::memset(f.G(f.sp + 0xB8), 0, 12);
    if (f.Call(sub_82D08A6C, {f.sp + 0xB8, f.sp + 0x80}))
      f.Call(sub_82D088EC, {1, f.Get(f.sp + 0x80)});
  }

  const u32 fence = d.W(0x2A9C);  // r30
  d.SetW(0x2AB0, fence);
  f.Kick(dev);
  f.Call(sub_822873E8, {dev, d.W(0x3A40) >> 2});
  if (const u32 pending = d.W(0x3A5C))
    f.Call(sub_8228F558, {dev, pending, 3, 0});

  if (d.W(0x5DAC) & 4) {
    const u32 record = f.sp + 0x80;
    const u32 thread = f.Get(f.Get(ctx.r13.u32 + 0x100) + 0x58);
    f.Put(record, dev);
    f.Put(record + 4, 3);
    f.Put(record + 8, f.Get(d.W(0x2A90)));
    f.Put(record + 12, thread);
    f.Put(record + 16, thread);
    f.Put(record + 20, 0);
    while (f.Call(sub_8264DCD0, {record}) && d.W(0x40B0) - d.W(0x40B8) >= 0xF) {
    }
    f.Call(sub_8264DC08, {record});
  } else {
    d.SetW(0x3A5C, fence);
  }
  d.SetB(0x2ABD, d.B(0x2ABD) & 0xEF);
  f.Call(sub_82287D38, {dev, 0});
  return true;
}

}  // namespace

void RegisterSwap() {
  SetHandler(kD3DDevice_Swap, Swap);
}

}  // namespace renut::d3d::port
