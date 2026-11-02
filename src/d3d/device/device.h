#pragma once
// Layer 1 of the native Direct3D port: the device's shadow state.
//
// The setters in this directory are ported from reNut's PC build (masterspike52's
// reNut, src/renut_engine/gpu/d3d/d3d_native.h and d3d_native_state.cpp), which in
// turn follow N&B's own Direct3D disassembly. They write the same bytes the
// recompiled setters write, so the native front end (d3d/frontend/) reads the same
// device at draw time. Credit for the port belongs to the reNut authors; keep the
// credit when the code moves.
//
// Two things differ from reNut and are handled here:
//
//  * Guest memory. reNut resolves a guest address through the kernel memory
//    object; here the handler is given `base` (as every REX_HOOK_RAW is) and the
//    address goes through renut::GuestPtr(base, address).
//
//  * The device layout. reNut targets title update 3, whose CDevice grew by 4
//    bytes from 0x2AC0 and by 8 from 0x2E30 (reNut's d3d.h says so). This app runs
//    the original release, so every offset reNut gives in the CDevice-private
//    area is translated by Layout() below. The register packets (below 0x2AC0)
//    and the fields from 0x3390 on are where they were.
//    Cross-checked against the offsets d3d/frontend/internal.h already uses on this
//    release: declaration 11820 = 0x2E2C, index buffer 12436 = 0x3094, render
//    targets 12440 = 0x3098, pixel/vertex shader 12692/12696 = 0x3194/0x3198,
//    command buffer 13516 = 0x34CC; those are reNut's 0x2E30, 0x309C, 0x30A0,
//    0x319C/0x31A0 and 0x34CC less 4, 8, 8, 8 and 0.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "guest/guest_address.h"

namespace renut::d3d::port {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

// reNut's (TU3) offset of a CDevice field -> this release's.
constexpr u32 Layout(u32 tu3_offset) {
  if (tu3_offset < 0x2AC0) return tu3_offset;
  if (tu3_offset <= 0x2E30) return tu3_offset - 4;
  if (tu3_offset <= 0x3390) return tu3_offset - 8;
  return tu3_offset;
}

// Guest memory is big-endian.
inline u16 LoadBE16(const u8* p) {
  u16 v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline u32 LoadBE32(const u8* p) {
  u32 v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline u64 LoadBE64(const u8* p) {
  u64 v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
inline void StoreBE16(u8* p, u16 v) {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void StoreBE32(u8* p, u32 v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void StoreBE64(u8* p, u64 v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}

// A guest D3D device's bytes. `base` is the guest memory base the hook was given.
struct Device {
  u8* base = nullptr;
  u32 va = 0;
  u8* bytes = nullptr;

  explicit operator bool() const { return bytes != nullptr; }

  u32 W(u32 offset) const { return LoadBE32(bytes + offset); }
  void SetW(u32 offset, u32 value) { StoreBE32(bytes + offset, value); }
  u16 H(u32 offset) const { return LoadBE16(bytes + offset); }
  void SetH(u32 offset, u16 value) { StoreBE16(bytes + offset, value); }
  u8 B(u32 offset) const { return bytes[offset]; }
  void SetB(u32 offset, u8 value) { bytes[offset] = value; }
  float F(u32 offset) const { return std::bit_cast<float>(W(offset)); }
  void SetF(u32 offset, float value) { SetW(offset, std::bit_cast<u32>(value)); }
  u64 Q(u32 offset) const { return LoadBE64(bytes + offset); }
  void SetQ(u32 offset, u64 value) { StoreBE64(bytes + offset, value); }

  // m_Pending[word] (D3DTAGCOLLECTION): the dirty masks the draw flush reads.
  u64 Pending(u32 word) const { return Q(word * 8); }
  void SetPending(u32 word, u64 value) { SetQ(word * 8, value); }
  void Dirty(u32 word, u64 bits) { SetPending(word, Pending(word) | bits); }

  // A big-endian word of another guest object (a texture's format dword).
  u32 GuestW(u32 address) const { return LoadBE32(renut::GuestPtr(base, address)); }
};

// The device a hook was called with, or an empty Device for a null pointer.
inline Device DeviceAt(uint8_t* base, u32 va) {
  if (!base || !va) return {};
  return Device{base, va, renut::GuestPtr(base, va)};
}

// A Device that only reaches guest memory (for the calls that take no device, such
// as the resource ones): `bytes` is not a device's.
inline Device GuestOnly(uint8_t* base) {
  if (!base) return {};
  return Device{base, 0, base};
}

// The PowerPC field operations N&B's code uses, with bits counted from the
// least significant: insrwi (Insert) and extrwi (Extract).
constexpr u32 FieldMask(u32 shift, u32 width) {
  return (width >= 32 ? ~0u : ((1u << width) - 1)) << shift;
}
constexpr u32 Insert(u32 word, u32 value, u32 shift, u32 width) {
  const u32 mask = FieldMask(shift, width);
  return (word & ~mask) | ((value << shift) & mask);
}
constexpr u32 Extract(u32 word, u32 shift, u32 width) {
  return (word >> shift) & FieldMask(0, width);
}

// D3D's GPU address for a CPU address (the expression its code inlines).
constexpr u32 GpuAddress(u32 cpu_address) {
  return (cpu_address & 0x1FFFFFFFu) + (((cpu_address >> 20) + 0x200u) & 0x1000u);
}

// fctiwz / fctidz: truncate, saturating, NaN to the most negative value.
inline i32 TruncateToI32(double value) {
  if (std::isnan(value)) return std::numeric_limits<i32>::min();
  if (value >= 2147483647.0) return std::numeric_limits<i32>::max();
  if (value <= -2147483648.0) return std::numeric_limits<i32>::min();
  return static_cast<i32>(value);
}
inline i64 TruncateToI64(double value) {
  if (std::isnan(value)) return std::numeric_limits<i64>::min();
  if (value >= 9223372036854775807.0) return std::numeric_limits<i64>::max();
  if (value <= -9223372036854775808.0) return std::numeric_limits<i64>::min();
  return static_cast<i64>(value);
}

// Device fields the layer-1 setters use, at this release's offsets.
namespace off {
constexpr u32 TextureFetch = 0x480;      // m_Constants.TextureFetch[26], 24 bytes each
constexpr u32 VertexFetch = 0x6F0;       // m_Constants.VertexFetch[18], 8 bytes each
constexpr u32 PredicatedPending2 = 0x28;  // m_Predicated_PendingMask2
constexpr u32 ClipPlanes = 0x2820;       // m_ClipPlanes[6][4]
constexpr u32 SurfaceInfo = 0x2880;      // m_DestinationPacket (RB_SURFACE_INFO)
constexpr u32 Color0Info = 0x2884;       // m_DestinationPacket (RB_COLOR_INFO)
constexpr u32 WindowScissorTL = 0x28C4;  // m_WindowPacket
constexpr u32 WindowScissorBR = 0x28C8;
constexpr u32 DepthInfo = 0x2888;
constexpr u32 ColorMask = 0x28DC;        // m_ValuesPacket
constexpr u32 ColorControl = 0x293C;     // m_ControlPacket
constexpr u32 DepthControl = 0x2934;
constexpr u32 HiControl = 0x2940;
constexpr u32 ClipControl = 0x2944;
constexpr u32 ModeControl = 0x2948;
constexpr u32 BlendControl0 = 0x2938;
constexpr u32 BlendControl1 = 0x2958;
constexpr u32 BlendControl2 = 0x295C;
constexpr u32 BlendControl3 = 0x2960;
constexpr u32 FenceTag = 0x2A9C;         // the current fence
constexpr u32 TilePredication = 0x2AB4;  // SetPredication / SetCommandBufferPredication args
constexpr u32 RunPredication = 0x2AB8;
constexpr u32 Flags = 0x2ABC;            // bytes 0x2ABC-0x2ABF; reNut's d3d_native.h lists the bits
constexpr u32 Flags1 = 0x2ABD;
constexpr u32 Flags2 = 0x2ABE;
constexpr u32 Flags3 = 0x2ABF;
constexpr u32 ScissorRect = Layout(0x318C);   // SetScissorRect's RECT
constexpr u32 IndexBuffer = Layout(0x309C);    // SetIndices
constexpr u32 RenderTargets = Layout(0x30A0); // SetRenderTarget's surfaces, [4]
constexpr u32 DepthStencil = Layout(0x30B0);  // SetDepthStencilSurface's surface
constexpr u32 StreamSources = Layout(0x30B4);  // SetStreamSource's buffers, [16]
constexpr u32 StreamStrides = Layout(0x30F8);  // u8[16], stride / 4
constexpr u32 Textures = Layout(0x3108);      // SetTexture's textures, [26]
constexpr u32 Predication = Layout(0x31AC);   // tile predication of the next draws, ~0 none
constexpr u32 RunPredicationState = Layout(0x31B0);
constexpr u32 PredicationSet = Layout(0x31B4);  // 1 while SetPredication's mask is non-zero
constexpr u32 TilingSurfaces = Layout(0x31C0);  // the 4 targets and depth BeginTiling bound
constexpr u32 TilingExtent = Layout(0x3384);    // widest and tallest tile edge
constexpr u32 CommandBuffer = Layout(0x34CC);   // the D3DCommandBuffer being recorded
constexpr u32 AutoDepthStencil = Layout(0x39E8);  // InitializePresentationParameters' surfaces
constexpr u32 AutoBackBuffer = Layout(0x39F0);
}  // namespace off

}  // namespace renut::d3d::port
