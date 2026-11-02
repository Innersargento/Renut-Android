#pragma once
// What the layer-1 sources (state.cpp, bound.cpp) share: the device's private
// fields, at this release's offsets, and the few helpers both use. Not part of
// the port's interface (state.h, bound.h).
#include "d3d/device/device.h"

namespace renut::d3d::port {

// m_Pending words.
constexpr u32 kPendingStates = 2;  // the register packets
constexpr u32 kPendingFetch = 3;   // fetch constants, tessellation, points
constexpr u32 kPendingMisc = 4;    // m_MiscPacket and friends

// Render state storage in the CDevice-private area. The offsets are reNut's (TU3);
// Layout() moves them to this release (device.h).
constexpr u32 kBlendState = Layout(0x2E50);  // D3D blend: src 0-4, op 5-7, dest 8-12, alpha at 16+
constexpr u32 kBlendFlags = Layout(0x2E54);  // 31 blend enable, 30 separate alpha, HiZ fields
constexpr u32 kScissorTestEnable = Layout(0x2E58);
constexpr u32 kColorWriteEnable = Layout(0x2E5C);  // [4]
constexpr u32 kZEnable = Layout(0x2E6C);
constexpr u32 kStencilEnable = Layout(0x2E70);
constexpr u32 kPointSpriteEnable = Layout(0x2E74);
constexpr u32 kPointSize = Layout(0x2E7C);
constexpr u32 kPointSizeMin = Layout(0x2E80);
constexpr u32 kPointSizeMax = Layout(0x2E84);
constexpr u32 kHighPrecisionBlend = Layout(0x2F04);  // [4]
constexpr u32 kPresentInterval = Layout(0x351C);
constexpr u32 kBehaviorFlags2 = Layout(0x5DAC);
// Sampler state storage, a byte per sampler.
constexpr u32 kMaxAnisotropy = Layout(0x2E9C);
constexpr u32 kMaxMipLevel = Layout(0x2EB6);
constexpr u32 kMinMipLevel = Layout(0x2ED0);
constexpr u32 kZFilter = Layout(0x2EEA);  // bit 0 mag, 1 min, 2 separate Z filtering
// SetViewport's arguments, clamped: x, y, width, height, min z, max z, 0.
constexpr u32 kViewport = Layout(0x3170);
constexpr u32 kVportXScale = 0x2908;  // the register image values
constexpr u32 kVportZScale = 0x2918;
// Bound objects and the state SetVertexDeclaration / SetStreamSource keep.
constexpr u32 kStreamDeclStrides = Layout(0x2E40);  // u8[16]
constexpr u32 kVertexDeclaration = Layout(0x2E30);
constexpr u32 kGprAllocation = 0x2AA8;
constexpr u32 kScreenExtentQueryMode = 0x2AA4;
// A byte of the device that UpdateTilePredicated tests (0 outside a command
// buffer's recording).
constexpr u32 kTilePredicatedGuard = Layout(0x2FA3);
// SetDepthStencilSurface's copy of the tiling extent's register image.
constexpr u32 kTilingSurfaceExtent = Layout(0x337C);

constexpr u32 kTextureSamplers = 26;

inline float AsFloat(u32 bits) { return std::bit_cast<float>(bits); }

constexpr u64 Bit(u32 n) { return u64(1) << n; }

// The sampler's fetch constant dirty bit (D3DTag_FetchConstant).
constexpr u64 FetchBit(u32 fetch_constant) { return Bit(31 - fetch_constant); }

constexpr u32 Fetch(u32 sampler, u32 dword) { return off::TextureFetch + sampler * 24 + dword * 4; }

// RB_COLOR*_INFO's format for high precision blending, as HighPrecisionBlendEnable
// and SetRenderTarget apply it (state.cpp).
void ApplyHighPrecisionBlend(Device d, u32 slot, u32 enable);
// D3DDevice_SetScissorRect's work; ScissorTestEnable and SetViewport reapply it
// (state.cpp).
void SetScissorRect(Device device, i32 left, i32 top, i32 right, i32 bottom);

// True when the device is tiling (Flags 0x10) or the targets bound are the ones
// BeginTiling bound (Flags 0x20), or none: the test ClearF, SetSurfaceClip,
// BeginCommandBuffer and the tiling calls all make (bound.cpp).
bool TilingBound(Device device);

// D3D::SetSurfaceClip (0x82223270) is called at the end of SetScissorRect and
// CleanupGprAllocationSetup. The recompiled one writes the window scissor into the
// ring, which nothing reads, but the native front end (d3d/frontend) records the clip
// from its hook on that function; skipping the call leaves the draw lists with a
// stale scissor. The port reports the clip here and hooks.cpp runs the original.
using SurfaceClipSink = void (*)(Device device, i32 x1, i32 y1, i32 x2, i32 y2);
void SetSurfaceClipSink(SurfaceClipSink sink);
void NotifySurfaceClip(Device device, i32 x1, i32 y1, i32 x2, i32 y2);

}  // namespace renut::d3d::port
