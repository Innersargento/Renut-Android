#pragma once
// Shaders, natively (see device.h): binding a shader to the device, creating the
// shader objects and vertex declarations, and registering a shader object in place.
//
// Ported from the retail executable's own code with idalib (2026-10-30), not from
// reNut: reNut keeps shaders as host records and its SetVertexShader only stores the
// slot, but this app's draw flush follows N&B's own and needs what the original sets:
// the dirty bits, the fence tags and the constants a shader defines for the device.
//   D3DDevice_SetVertexShader 0x8222A0A8      D3DDevice_SetPixelShader 0x82226730
//   XGRegisterVertexShader 0x8264E7B8         XGRegisterPixelShader 0x8264E670
//   D3DDevice_CreateVertexShader 0x8264E8B0   D3DDevice_CreatePixelShader 0x8264E6A8
//   D3DDevice_CreateVertexDeclaration 0x8264EA90 (builder 0x8264E9A0)
//   D3DDevice_Set{Vertex,Pixel}ShaderConstantFN 0x8222EFA8 / 0x8222F0D8
// Still the recompiled Direct3D's: D3DVertexShader_Bind (0x82657048) and the
// microcode patching it and the draw flush use (0x821E5218, 0x82656DA0, 0x822127F8).
#include "d3d/device/device.h"

namespace renut::d3d::port {

// The game's allocator, which owns the memory of the shader objects (the game frees
// them with its own XMemFree).
struct ShaderCalls {
  u32 (*xmem_alloc)(void* user, u32 size, u32 attributes) = nullptr;
  void (*xmem_free)(void* user, u32 address, u32 attributes) = nullptr;
  void* user = nullptr;
};

// Each returns false when the call is left to the recompiled code (nothing has been
// changed then).

// D3DDevice_SetVertexShader(device, shader) / D3DDevice_SetPixelShader(device, shader)
bool SetVertexShader(Device device, u32 shader);
bool SetPixelShader(Device device, u32 shader);

// XGRegisterVertexShader(shader, physical) / XGRegisterPixelShader(shader, physical):
// make a shader object of the zeroed memory at `shader`, over the microcode at `physical`.
void RegisterVertexShader(uint8_t* base, u32 shader, u32 physical);
void RegisterPixelShader(uint8_t* base, u32 shader, u32 physical);

// D3DVertexShader *CreateVertexShader(const DWORD *pFunction) and the pixel one: the
// new object in `result` (0 when out of memory).
bool CreateVertexShader(uint8_t* base, u32 function, const ShaderCalls& calls, u32& result);
bool CreatePixelShader(uint8_t* base, u32 function, const ShaderCalls& calls, u32& result);

// D3DVertexDeclaration *CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pElements)
u32 CreateVertexDeclaration(uint8_t* base, u32 elements, const ShaderCalls& calls);

// SetVertexShaderConstantFN(device, start, data, count, pending mask) and the pixel one.
bool SetVertexShaderConstantsF(Device device, u32 start, u32 data, u32 count, u64 pending_mask);
bool SetPixelShaderConstantsF(Device device, u32 start, u32 data, u32 count, u64 pending_mask);

}  // namespace renut::d3d::port
