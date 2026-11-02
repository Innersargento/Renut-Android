#pragma once
// Layer 1a of the native Direct3D port (see device.h): the state setters and
// getters that only read and write the device.
#include "d3d/device/device.h"

namespace renut::d3d::port {

// N&B's render state table has 101 entries and its sampler table 20, in the order
// reNut and the original number them (the function names in hooks/functions.h
// follow it).
constexpr u32 kRenderStateCount = 101;
constexpr u32 kSamplerStateCount = 20;
constexpr u32 kTextureSamplerCount = 26;

// The setters write the shadow state exactly as the recompiled ones do.
void SetRenderState(Device device, u32 state, u32 value);
void SetSamplerState(Device device, u32 sampler, u32 state, u32 value);
// The getters: the value as Direct3D reports it.
u32 GetRenderState(Device device, u32 state);
u32 GetSamplerState(Device device, u32 sampler, u32 state);

// D3DDevice_SetClipPlane, D3DDevice_Set{Vertex,Pixel}ShaderConstant{B,I}.
void SetClipPlane(Device device, u32 index, const u8* plane);
void SetShaderConstantsB(Device device, u32 first_word, u32 start, const u8* data, u32 count);
void SetShaderConstantsI(Device device, u32 first_word, u32 start, const u8* data, u32 count);

// Installs the layer-1a handlers (hooks.cpp). Each takes its call from the
// recompiled Direct3D, through renut::d3d::SetHandler.
void RegisterStateHandlers();

}  // namespace renut::d3d::port
