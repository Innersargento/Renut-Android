#pragma once
// A stack frame for a ported call: the locals and outgoing stack arguments the original
// kept below its caller's stack pointer, in guest memory, and calls out of it.
#include <array>
#include <cstring>

#include "d3d/device/device.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace renut::d3d::port {

using GuestFn = void (*)(PPCContext&, uint8_t*);

struct GuestFrame {
  PPCContext* ctx;
  uint8_t* base;
  u32 sp;

  GuestFrame(PPCContext& c, uint8_t* b, u32 size) : ctx(&c), base(b), sp(c.r1.u32 - size) {}

  u8* G(u32 address) const { return renut::GuestPtr(base, address); }
  u32 Get(u32 address) const { return LoadBE32(G(address)); }
  void Put(u32 address, u32 value) const { StoreBE32(G(address), value); }

  // Calls `function` with r3..r10 and returns its r3.
  u32 Call(GuestFn function, std::array<u32, 8> args = {}) const {
    PPCContext call = *ctx;
    call.r1.u32 = sp;
    call.r3.u32 = args[0];
    call.r4.u32 = args[1];
    call.r5.u32 = args[2];
    call.r6.u32 = args[3];
    call.r7.u32 = args[4];
    call.r8.u32 = args[5];
    call.r9.u32 = args[6];
    call.r10.u32 = args[7];
    function(call, base);
    return call.r3.u32;
  }

  // The same with 64-bit registers (the dirty-state masks are 64 bits wide).
  u64 CallWide(GuestFn function, std::array<u64, 6> args = {}) const {
    PPCContext call = *ctx;
    call.r1.u32 = sp;
    call.r3.u64 = args[0];
    call.r4.u64 = args[1];
    call.r5.u64 = args[2];
    call.r6.u64 = args[3];
    call.r7.u64 = args[4];
    call.r8.u64 = args[5];
    function(call, base);
    return call.r3.u64;
  }

  // A call through a guest function address (a vtable slot or a stored callback).
  u32 CallAddress(u32 target, std::array<u32, 4> args = {}) const {
    PPCContext call = *ctx;
    call.r1.u32 = sp;
    call.r3.u32 = args[0];
    call.r4.u32 = args[1];
    call.r5.u32 = args[2];
    call.r6.u32 = args[3];
    call.last_indirect_target = target;
    rex::runtime::ResolveIndirectFunction(target)(call, base);
    return call.r3.u32;
  }
};

// D3D's GPU address of a CPU address.
inline u32 GuestGpu(u32 address) {
  return (((address >> 20) + 0x200) & 0x1000) + (address & 0x1FFFFFFF);
}

}  // namespace renut::d3d::port
