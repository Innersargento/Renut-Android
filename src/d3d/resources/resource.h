#pragma once
// Layer 2 of the native Direct3D port (see device.h): resources. Headers and their
// memory are created, locked and destroyed here instead of by the recompiled
// Direct3D. Guest addresses are u32 virtual addresses, as everywhere in the port.
#include "d3d/device/device.h"

namespace renut::d3d::port {

// The recompiled game and kernel functions resource memory goes through (hooks.cpp
// runs them on a copy of the caller's context).
struct GuestCalls {
  // MmAllocatePhysicalMemoryEx(0, size, protect, 0, ~0, alignment); 0 on failure.
  u32 (*alloc_physical)(u32 size, u32 protect, u32 alignment) = nullptr;
  void (*free_physical)(u32 address) = nullptr;     // MmFreePhysicalMemory
  void (*xmem_free)(u32 address, u32 attributes) = nullptr;  // the game's XMemFree
  void (*wait_fence)(u32 fence) = nullptr;          // wait for the GPU to be done with a fence
};
void SetGuestCalls(const GuestCalls& calls);

u32 CreateVertexBuffer(u32 length, u32 usage);
u32 CreateIndexBuffer(u32 length, u32 usage, u32 format);
// D3DDevice_CreateTexture(Width, Height, Depth, Levels, Usage, Format, Pool, Type).
u32 CreateTexture(u32 width, u32 height, u32 depth, u32 levels, u32 usage, u32 format, u32 type);
u32 CreateSurface(u32 width, u32 height, u32 format, u32 multisample, u32 parameters_va);

// The address the CPU writes at.
u32 LockVertexBuffer(u32 buffer, u32 flags);
u32 LockIndexBuffer(u32 buffer, u32 flags);
void LockArrayTextureRect(u32 texture, u32 slice, u32 level, u32 locked_rect, u32 rect, u32 flags);
void LockLineTextureRect(u32 texture, u32 level, u32 locked_rect, u32 rect, u32 flags);
void LockSurfaceRect(u32 surface, u32 locked_rect, u32 rect, u32 flags);
void LockVolumeBox(u32 volume, u32 locked_box, u32 box, u32 flags);

void UnlockVertexBuffer(u32 buffer);
void UnlockSurfaceRect(u32 surface);
void UnlockArrayTextureRect(u32 texture);
void UnlockConstantBuffer(u32 buffer);

// D3DResource_Destroy: waits for the resource's fence, frees its data and header.
void DestroyResource(u32 resource);

// D3D's subresource layout (sub_82207680): where one level and array slice of a texture
// lies, and its dimensions.
struct SubresourceInfo {
  u32 address = 0, row_pitch = 0, slice_bytes = 0, level_bytes = 0, tail_offset = 0;
  u32 width = 0, height = 0, depth = 0;
};
SubresourceInfo QuerySubresource(u32 texture, u32 slice, u32 level);

void GetSurfaceDesc(u32 surface, u32 desc);
void GetVolumeDesc(u32 volume, u32 desc);
u32 GetSurfaceLevel(u32 texture, u32 level);
u32 GetCubeMapSurface(u32 texture, u32 face, u32 level);
u32 GetVolumeLevel(u32 texture, u32 level);

}  // namespace renut::d3d::port
