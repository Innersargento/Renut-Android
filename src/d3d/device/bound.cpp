// Layer 1b: the calls that bind objects to the device (textures, vertex and index
// buffers, the vertex declaration, render targets, the viewport, predication) and
// the ones that dirty or reset state around them.
//
// Ported from reNut's d3d_native_state.cpp (masterspike52's reNut), as state.cpp
// is: the same fields, dirty bits and order of writes, without the register
// packets N&B's D3D writes into the ring. The native front end flushes from the
// shadow at draw time and reads the bound objects the way these leave them.
//
// Left to the recompiled Direct3D on purpose:
//  * SetVertexShader / SetPixelShader. reNut keeps only the device slot (its
//    shaders are host records); the front end here follows N&B's own flush, which
//    needs the dirty bits and flags the original sets. UnsetAll calls them through
//    ShaderBinder.
//  * BlockUntilIdle (layer 3, it waits on a fence).
#include "d3d/device/bound.h"

#include "d3d/device/internal.h"

namespace renut::d3d::port {

namespace {

// D3DDevice_SetSurfaceInfo's RB_SURFACE_INFO and MSAA config from the surface.
void SetSurfaceInfo(Device device, u32 surface);

// The fence a resource the device stops using is busy until. A device recording
// a command buffer has no fence tag: N&B's D3D then adds the resource to the
// buffer's list, which fences it when the buffer runs; the buffers run on the
// host, so the list is left out.
void TagResourceFence(Device device, u32 resource, bool read_fence) {
  if (!resource)
    return;
  u8* header = renut::GuestPtr(device.base, resource);
  if (const u32 fence = device.W(off::FenceTag)) {
    StoreBE32(header + 8, fence);  // Fence
    if (read_fence)
      StoreBE32(header + 12, fence);  // ReadFence
  }
}

// True when the bound targets are the ones BeginTiling bound (or none).
bool TilingBoundSurfaces(Device device) {
  const u8 flags = device.B(off::Flags);
  if (flags & 0x10)
    return true;
  if (!(flags & 0x20))
    return false;
  for (u32 i = 0; i < 5; ++i) {
    const u32 bound = device.W(off::RenderTargets + i * 4);  // 4 targets, then depth
    if (bound && bound != device.W(off::TilingSurfaces + i * 4))
      return false;
  }
  return true;
}

// Recomputes Flags bit 0: the draws that follow run in each tile's pass.
void UpdateTilePredicated(Device device) {
  const u8 flags = device.B(off::Flags);
  const bool predicated = !device.W(off::PredicationSet) && !(flags & 0x08) && !(flags & 0x04) &&
                          !device.B(kTilePredicatedGuard) && TilingBoundSurfaces(device);
  device.SetB(off::Flags, u8((flags & 0xFE) | (predicated ? 1 : 0)));
}

// The surface's width and height from its +36 (width - 1 << 18 | height - 1 << 3).
void SurfaceSize(Device device, u32 surface, i32& width, i32& height) {
  const u32 size = LoadBE32(renut::GuestPtr(device.base, surface + 0x24));
  width = i32((size >> 18) + 1);
  height = i32(Extract(size, 3, 15) + 1);
}

// D3D::SetViewport(D3DDevice*, float X, Y, Width, Height, MinZ, MaxZ)
// [0x82222F20]: clamped to the bound surface (or the tiling extent), with the
// PA_CL_VPORT values; nothing without a target or depth surface.
void SetViewportRect(Device device, float x, float y, float width, float height, float min_z,
                     float max_z) {
  const u32 target = device.W(off::RenderTargets);
  const u32 surface = target ? target : device.W(off::DepthStencil);
  if (!surface)
    return;
  i32 surface_width, surface_height;
  if (TilingBoundSurfaces(device)) {
    surface_width = i32(device.W(off::TilingExtent));
    surface_height = i32(device.W(off::TilingExtent + 4));
  } else {
    SurfaceSize(device, surface, surface_width, surface_height);
  }
  // 64-bit sums: the truncations saturate, so adding them in 32 bits could overflow.
  const i64 ix = TruncateToI32(x);
  const i64 iy = TruncateToI32(y);
  i64 right = surface_width;
  if (TruncateToI32(width) + ix < right)
    right = TruncateToI32(width) + ix;
  i64 bottom = TruncateToI32(height) + iy;
  if (bottom >= surface_height)
    bottom = surface_height;
  i64 clamped_width = right - ix;
  i64 clamped_height = bottom - iy;
  if (clamped_width < 0 || clamped_height < 0) {
    clamped_width = 0;
    clamped_height = 0;
  }
  const float fw = float(clamped_width);
  const float fh = float(clamped_height);
  device.SetF(kViewport, x);
  device.SetF(kViewport + 4, y);
  device.SetF(kViewport + 16, min_z);
  device.SetF(kViewport + 20, max_z);
  device.SetF(kViewport + 12, fh);
  device.SetW(kViewport + 24, 0);
  device.SetF(kViewport + 8, fw);
  SetScissorRect(device, i32(device.W(off::ScissorRect)), i32(device.W(off::ScissorRect + 4)),
                 i32(device.W(off::ScissorRect + 8)), i32(device.W(off::ScissorRect + 12)));
  device.SetF(kVportZScale, max_z - min_z);
  device.SetF(kVportZScale + 4, min_z);
  const float half_width = fw * 0.5f;
  const float half_height = fh * 0.5f;
  device.SetF(kVportXScale, half_width);
  device.SetF(kVportXScale + 4, half_width + x);
  device.SetF(kVportXScale + 8, -half_height);
  device.SetF(kVportXScale + 12, half_height + y);
  device.Dirty(kPendingStates, 0x07E00000);
}

// D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]: RB_SURFACE_INFO and
// the MSAA config from the surface; the scissor and viewport reset to it.
void SetSurfaceInfo(Device device, u32 surface) {
  if (!surface)
    return;
  const u8* s = renut::GuestPtr(device.base, surface);
  const u32 info = LoadBE32(s + 24);
  const u32 msaa = Extract(info, 16, 2);
  device.SetW(0x29BC, msaa == 0 ? 0u : msaa == 1 ? 0x8001u : 0xC003u);
  device.SetW(off::SurfaceInfo, info);
  device.Dirty(kPendingStates, Bit(57));
  device.Dirty(kPendingMisc, Bit(36));
  device.SetW(off::ScissorRect, 0);
  device.SetW(off::ScissorRect + 4, 0);
  device.SetW(off::ScissorRect + 8, 0xFFFF);
  device.SetW(off::ScissorRect + 12, 0xFFFF);
  SetViewportRect(device, 0.0f, 0.0f, 65535.0f, 65535.0f, 0.0f, 1.0f);  // unk_820018DC
}

}  // namespace

bool TilingBound(Device device) { return TilingBoundSurfaces(device); }

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]. The fetch constant takes the texture's
// header, keeping the sampler state bits; its mip range is narrowed by the
// sampler's MaxMipLevel / MinMipLevel.
void SetTexture(Device device, u32 sampler, u32 texture) {
  if (!device || sampler >= kTextureSamplers)
    return;
  const u32 previous = device.W(off::Textures + sampler * 4);
  if (texture) {
    const u8* header = renut::GuestPtr(device.base, texture);
    const u32 t0 = LoadBE32(header + 0x1C);  // the header's fetch constant
    const u32 t1 = LoadBE32(header + 0x20);
    const u32 t2 = LoadBE32(header + 0x24);
    const u32 t3 = LoadBE32(header + 0x28);
    const u32 t4 = LoadBE32(header + 0x2C);
    const u32 t5 = LoadBE32(header + 0x30);
    const u32 f0 = device.W(Fetch(sampler, 0));
    const u32 f1 = device.W(Fetch(sampler, 1));
    const u32 f3 = device.W(Fetch(sampler, 3));
    const u32 f4 = device.W(Fetch(sampler, 4));
    const u32 f5 = device.W(Fetch(sampler, 5));
    const u32 mip_base = (t5 & 0x1FFFFE00u) + (((t5 >> 20) + 0x200u) & 0x1000u);
    device.SetW(Fetch(sampler, 2), t2);
    u32 dword4 = (t4 & 0x3FC) | (f4 & 0xFFFFFC03u);
    device.SetW(Fetch(sampler, 0), (t0 & ~0x3FFC00u) | (f0 & 0x3FFC00u));
    device.SetW(Fetch(sampler, 1), (GpuAddress(t1) & ~0x800u) | (f1 & 0x800u));
    device.SetW(Fetch(sampler, 3), (t3 & ~0x7FF80000u) | (f3 & 0x7FF80000u));
    device.SetW(Fetch(sampler, 5), (mip_base & ~0x1FFu) | (f5 & 0x1FFu));
    u32 max_level = device.B(kMaxMipLevel + sampler);
    if (Extract(t4, 2, 4) > max_level)
      max_level = Extract(t4, 2, 4);
    dword4 = Insert(dword4, max_level, 2, 4);
    u32 min_level = device.B(kMinMipLevel + sampler);
    const u32 texture_min = Extract(t4, 6, 4);
    if (texture_min < min_level)
      min_level = texture_min;
    dword4 = Insert(dword4, min_level, 6, 4);
    device.SetW(Fetch(sampler, 4), dword4);
    device.Dirty(kPendingFetch, FetchBit(sampler));
  }
  device.SetW(off::Textures + sampler * 4, texture);
  if (previous)
    TagResourceFence(device, previous, false);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void SetStreamSource(Device device, u32 stream, u32 vertex_buffer, u32 offset, u32 stride) {
  if (!device || stream >= 16)
    return;
  if (vertex_buffer) {
    const u8* buffer = renut::GuestPtr(device.base, vertex_buffer);
    // Stream n is VertexFetch[17 - n], in fetch constant (95 - n) / 3.
    const u32 fetch = off::VertexFetch + (17 - stream) * 8;
    device.SetW(fetch, GpuAddress(LoadBE32(buffer + 24) + offset));
    device.SetW(fetch + 4, LoadBE32(buffer + 28) - offset);
    device.Dirty(kPendingFetch, FetchBit((95 - stream) / 3));
  }
  const u32 previous = device.W(off::StreamSources + stream * 4);
  if (previous)
    TagResourceFence(device, previous, false);
  device.SetW(off::StreamSources + stream * 4, vertex_buffer);
  const u32 dwords = stride >> 2;
  device.SetB(off::StreamStrides + stream, u8(dwords));
  if (dwords && dwords != device.B(kStreamDeclStrides + stream))
    device.Dirty(kPendingStates, 0x80000);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void SetIndices(Device device, u32 index_buffer) {
  if (!device)
    return;
  if (const u32 previous = device.W(off::IndexBuffer))
    TagResourceFence(device, previous, false);
  device.SetW(off::IndexBuffer, index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void SetVertexDeclaration(Device device, u32 declaration) {
  if (!device)
    return;
  device.SetW(kVertexDeclaration, declaration);
  device.Dirty(kPendingStates, 0x80000);
}

void SetViewportFloats(Device device, float x, float y, float width, float height, float min_z, float max_z) {
  if (device)
    SetViewportRect(device, x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void SetViewport(Device device, u32 viewport_va) {
  if (!device || !viewport_va)
    return;
  const u8* v = renut::GuestPtr(device.base, viewport_va);
  SetViewportRect(device, float(double(LoadBE32(v))), float(double(LoadBE32(v + 4))),
                  float(double(LoadBE32(v + 8))), float(double(LoadBE32(v + 12))),
                  AsFloat(LoadBE32(v + 16)), AsFloat(LoadBE32(v + 20)));
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void SetScissorRectCall(Device device, u32 rect_va) {
  if (!device || !rect_va)
    return;
  const u8* r = renut::GuestPtr(device.base, rect_va);
  SetScissorRect(device, i32(LoadBE32(r)), i32(LoadBE32(r + 4)), i32(LoadBE32(r + 8)),
                 i32(LoadBE32(r + 12)));
}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void SetRenderTarget(Device device, u32 index, u32 surface) {
  if (!device || index >= 4)
    return;
  device.SetW(off::RenderTargets + index * 4, surface);
  const u8* s = surface ? renut::GuestPtr(device.base, surface) : nullptr;
  if (s) {
    const u32 slot = index ? index + 1 : 0;
    device.SetW(off::Color0Info + slot * 4, LoadBE32(s + 28));
    device.Dirty(kPendingStates, Bit(56 - slot));
    ApplyHighPrecisionBlend(device, slot, device.W(kHighPrecisionBlend + index * 4));
  }
  u32 enable = device.W(kColorWriteEnable + index * 4);
  if (index == 0) {
    u8 flags3 = device.B(off::Flags3) & 0xF7;
    device.SetB(off::Flags3, flags3);
    if (s) {
      const u32 format = (LoadBE32(s + 28) >> 16) & 0xF;
      if (format == 5 || format == 7 || format == 15) {
        device.SetB(off::Flags3, flags3 | 8);
        device.Dirty(kPendingStates, 0x100000);
      }
      SetSurfaceInfo(device, surface);
    } else if (const u32 depth = device.W(off::DepthStencil)) {
      SetSurfaceInfo(device, depth);
    }
    UpdateTilePredicated(device);
  }
  if (!device.W(off::RenderTargets + index * 4))
    enable = 0;
  device.SetW(off::ColorMask, Insert(device.W(off::ColorMask), enable, index * 4, 4));
  device.Dirty(kPendingStates, Bit(37));
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void SetDepthStencilSurface(Device device, u32 surface) {
  if (!device)
    return;
  device.SetW(off::DepthStencil, surface);
  if (surface) {
    const u8* s = renut::GuestPtr(device.base, surface);
    if (!device.W(off::RenderTargets))
      SetSurfaceInfo(device, surface);
    device.SetW(off::DepthInfo, LoadBE32(s + 28));
    const u32 hi_control = LoadBE32(s + 32);
    device.SetW(off::HiControl, hi_control);
    device.SetB(off::Flags2, u8(Insert(device.B(off::Flags2), hi_control, 5, 1)));
    if ((device.B(off::Flags3) & 0x20) && TilingBoundSurfaces(device))
      device.SetW(off::SurfaceInfo,
                  Insert(device.W(off::SurfaceInfo), device.W(kTilingSurfaceExtent), 18, 14));
    device.Dirty(kPendingStates, Bit(55) | 0x100 | 0x20000);
  } else {
    device.SetW(off::HiControl, device.W(off::HiControl) & ~0xFu);
    device.SetB(off::Flags2, device.B(off::Flags2) & 0xDF);
    device.Dirty(kPendingStates, 0x100);
  }
  UpdateTilePredicated(device);
  const bool depth = device.W(off::DepthStencil) != 0;
  const u32 z_enable = depth ? device.W(kZEnable) : 0;
  device.SetW(off::DepthControl, Insert(device.W(off::DepthControl), z_enable, 1, 1));
  device.Dirty(kPendingStates, 0x800 | 0x20000);
  const u32 stencil_enable = depth ? device.W(kStencilEnable) : 0;
  device.SetW(off::DepthControl, (device.W(off::DepthControl) & ~1u) | (stencil_enable & 1));
  device.Dirty(kPendingStates, 0x800 | 0x20000);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void SetPredication(Device device, u32 mask) {
  if (!device)
    return;
  if (!mask) {
    device.SetW(off::PredicationSet, 0);
    UpdateTilePredicated(device);
    device.SetW(off::Predication, ~0u);
  } else {
    device.SetW(off::Predication, mask);
    device.SetB(off::Flags, device.B(off::Flags) & 0xFE);
    device.SetW(off::PredicationSet, 1);
  }
  device.SetW(off::RunPredicationState, 0);
  device.SetW(off::TilePredication, mask);
  device.SetW(off::RunPredication, 0);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void SetCommandBufferPredication(Device device, u32 tile_predication, u32 run_predication) {
  if (!device)
    return;
  if (const u32 command_buffer = device.W(off::CommandBuffer)) {
    u8* word = renut::GuestPtr(device.base, command_buffer) + 0x6C;
    StoreBE32(word, LoadBE32(word) | 0x40);
  }
  if (!tile_predication && !run_predication) {
    device.SetW(off::PredicationSet, 0);
    UpdateTilePredicated(device);
    device.SetW(off::RunPredicationState, 0);
    device.SetW(off::Predication, ~0u);
  } else {
    device.SetW(off::Predication, tile_predication);
    device.SetB(off::Flags, device.B(off::Flags) & 0xFE);
    device.SetW(off::PredicationSet, 1);
    device.SetW(off::RunPredicationState, run_predication);
  }
  device.SetW(off::TilePredication, tile_predication);
  device.SetW(off::RunPredication, run_predication);
}

// D3D::CleanupGprAllocationSetup (0x8264E2C0): the state a GPR allocation
// change invalidates.
void CleanupGprAllocationSetup(Device device) {
  if (!device)
    return;
  device.Dirty(kPendingStates, Bit(41) | Bit(40) | Bit(39));
  device.Dirty(kPendingFetch, Bit(35));
  device.Dirty(kPendingStates, 0x80 | 0x40 | 0x20);
  device.Dirty(kPendingMisc, 0x80000);
  device.Dirty(kPendingStates, Bit(44) | 0x80000 | 0x100000 | 0x800 | 0x100 | 8 | Bit(37));
  device.Dirty(kPendingFetch, Bit(54));
  device.Dirty(kPendingMisc, Bit(35));
  device.Dirty(kPendingFetch, 0x80000000u);
  // Then SetSurfaceClip with the device's window scissor.
  const u32 tl = device.W(off::WindowScissorTL), br = device.W(off::WindowScissorBR);
  NotifySurfaceClip(device, i32(Extract(tl, 0, 15)), i32(Extract(tl, 16, 15)), i32(Extract(br, 0, 15)),
                    i32(Extract(br, 16, 15)));
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void SetShaderGPRAllocation(Device device, u32 flags, u32 vertex_count, u32 pixel_count) {
  if (!device)
    return;
  if (!vertex_count && !pixel_count) {
    vertex_count = 64;
    pixel_count = 64;
  }
  u32 allocation = 0;
  if (!(flags & 1)) {
    pixel_count = Insert(pixel_count, vertex_count, 8, 7);
    allocation = device.W(kGprAllocation);
    allocation = Insert(allocation, pixel_count, 4, 7);
    allocation = (allocation & ~0x7F000u) | ((pixel_count << 4) & 0x7F000u);
  }
  device.SetW(kGprAllocation, allocation);
  // The register writes go into the ring.
  CleanupGprAllocationSetup(device);
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void SetScreenExtentQueryMode(Device device, u32 mode) {
  if (!device)
    return;
  device.SetW(kScreenExtentQueryMode, Insert(device.W(kScreenExtentQueryMode), mode, 12, 2));
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]: everything but the device's
// own (InitializePresentationParameters') surfaces.
void UnsetAll(Device device, const ShaderBinder& shaders) {
  if (!device)
    return;
  for (u32 i = 0; i < 4; ++i) {
    if (device.W(off::RenderTargets + i * 4) != device.W(off::AutoBackBuffer))
      SetRenderTarget(device, i, 0);
  }
  if (device.W(off::DepthStencil) != device.W(off::AutoDepthStencil))
    SetDepthStencilSurface(device, 0);
  if (shaders.bind) {
    shaders.bind(shaders.user, true);
    shaders.bind(shaders.user, false);
  }
  device.SetW(kVertexDeclaration, 0);
  device.Dirty(kPendingStates, 0x80000);
  SetIndices(device, 0);
  for (u32 stream = 0; stream < 16; ++stream)
    SetStreamSource(device, stream, 0, 0, 4);
  for (u32 sampler = 0; sampler < kTextureSamplers; ++sampler)
    SetTexture(device, sampler, 0);
}

// D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]: besides the packets that
// restore the predication and the screen extent mode, every dirty mask set.
void ResetAllState(Device device) {
  if (!device)
    return;
  for (u32 word = 0; word < 5; ++word)
    device.SetPending(word, ~u64(0));
}

// UpdateTilePredicated for the command buffer calls (command_buffers.cpp).
void RefreshTilePredicated(Device device) { UpdateTilePredicated(device); }

// D3D::InvalidateConstants (0x8264E128, D3DDevice_FlushHiZStencil here): an
// invalidate packet, after which the constants are not known to be loaded.
void InvalidateConstants(Device device) {
  if (!device)
    return;
  device.SetB(off::Flags3, device.B(off::Flags3) & 0xFE);
  device.SetB(off::Flags1, device.B(off::Flags1) & 0xFB);
}

}  // namespace renut::d3d::port
