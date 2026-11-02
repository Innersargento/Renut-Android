#pragma once
// Layer 1b of the native Direct3D port (see device.h): the calls that bind
// objects to the device, and the state resets around them.
#include "d3d/device/device.h"

namespace renut::d3d::port {

void SetTexture(Device device, u32 sampler, u32 texture);
void SetStreamSource(Device device, u32 stream, u32 vertex_buffer, u32 offset, u32 stride);
void SetIndices(Device device, u32 index_buffer);
void SetVertexDeclaration(Device device, u32 declaration);
void SetViewport(Device device, u32 viewport_va);       // D3DVIEWPORT9*
// D3D::SetViewport (0x82222F20): the same with the rectangle and depth range as floats.
void SetViewportFloats(Device device, float x, float y, float width, float height, float min_z, float max_z);
void SetScissorRectCall(Device device, u32 rect_va);    // RECT*
void SetRenderTarget(Device device, u32 index, u32 surface);
void SetDepthStencilSurface(Device device, u32 surface);
void SetPredication(Device device, u32 mask);
void SetCommandBufferPredication(Device device, u32 tile_predication, u32 run_predication);
void CleanupGprAllocationSetup(Device device);
void SetShaderGPRAllocation(Device device, u32 flags, u32 vertex_count, u32 pixel_count);
void SetScreenExtentQueryMode(Device device, u32 mode);
// D3D::InvalidateConstants (D3DDevice_FlushHiZStencil here).
void InvalidateConstants(Device device);
// D3D::ResetAllState: every piece of state dirty. Not intercepted yet (see hooks.cpp).
void ResetAllState(Device device);

// UnsetAll also unbinds the shaders, which stay the recompiled Direct3D's: the
// caller supplies how (hooks.cpp calls the original SetVertexShader/SetPixelShader).
struct ShaderBinder {
  void (*bind)(void* user, bool vertex) = nullptr;  // binds shader 0
  void* user = nullptr;
};
void UnsetAll(Device device, const ShaderBinder& shaders);

}  // namespace renut::d3d::port
