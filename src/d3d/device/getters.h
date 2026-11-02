#pragma once
// Layer 1c of the native Direct3D port (see device.h): the getters, resource
// reference counting and the last of the device-state calls. Guest memory only;
// nothing here needs the renderer.
#include "d3d/device/device.h"

namespace renut::d3d::port {

// Common bit of a texture level's surface/volume header (a view of a texture).
constexpr u32 kCommonTextureLevel = 0x40000000u;

// ULONG D3DResource_AddRef: a texture level's first reference is one on its texture.
u32 AddRefResource(Device device, u32 resource);

// D3DResource_Destroy stays the recompiled Direct3D's (it frees the resource's
// memory through the game's allocator); ReleaseResource asks for it through this.
struct ResourceDestroyer {
  void (*destroy)(void* user, u32 resource) = nullptr;
  void* user = nullptr;
};
// ULONG D3DResource_Release: the last release destroys the resource, and a texture
// level's releases its texture first.
u32 ReleaseResource(Device device, u32 resource, const ResourceDestroyer& destroyer);

// D3DRESOURCETYPE D3DResource_GetType: textures by dimension, a volume texture's
// level 0x10, otherwise the Common type.
u32 ResourceType(Device device, u32 resource);
u32 BaseTextureLevelCount(Device device, u32 texture);
u32 ArrayTextureSize(Device device, u32 texture);

u32 GetRenderTarget(Device device, u32 index);
u32 GetDepthStencilSurface(Device device);
// The vertex buffer of a stream; offset and stride in bytes are written to the
// guest pointers when they are not 0.
u32 GetStreamSource(Device device, u32 stream, u32 offset_va, u32 stride_va);
void GetViewport(Device device, u32 viewport_va);
void SetVertexFetchConstant(Device device, u32 reg, u32 vertex_buffer, u32 offset);
void EndIndexedVertices(Device device);
void GetDisplayMode(Device device, u32 mode_va);

}  // namespace renut::d3d::port
