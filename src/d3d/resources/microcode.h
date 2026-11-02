#pragma once
// The vertex shader binding and the microcode patching, natively (see shader.h).
//
// A vertex shader's microcode names its vertex fetch constants in the order the
// compiler met them; binding it to a vertex declaration rewrites those constants
// (format, offset, stride, ordering) to match the declaration's elements and the
// streams' strides, and matching it to a pixel shader fixes up its output
// interpolators. Ported from the retail executable with idalib (2026-10-30):
//   D3D::PatchVertexShaderToMatchVertexDeclaration 0x821E5218
//   D3D::PatchVertexShaderToMatchPixelShader 0x82656DA0 (helpers 0x82656CE8, 0x82656C18)
//   the binding check and rebind 0x822127F8, D3DVertexShader_Bind 0x82657048
// The bit manipulation follows the PowerPC instruction by instruction (rotate and
// insert-under-mask), because that is what the game's own shaders were compiled
// against.
#include "d3d/device/device.h"

namespace renut::d3d::port {

// 0x821E5218 (shader, microcode fetch constants, declaration, per-stream strides in
// units of 4 bytes, program index): rewrites the program's fetch constants in
// `microcode` (guest address) to match the declaration.
void PatchVertexShaderToDeclaration(uint8_t* base, u32 shader, u32 microcode, u32 declaration,
                                    const u8* strides, u32 program);

// 0x82656DA0 (vertex shader's container, microcode copy, the program-control word to
// update, pixel shader's container or 0).
void PatchVertexShaderToPixelShader(uint8_t* base, u32 vertex_container, u32 microcode, u32 program_control,
                                    u32 pixel_container);

// 0x822127F8 (device, shader, declaration, program): whether the program is bound to
// the declaration and the device's strides, rebinding it when it is not and the GPU is
// done with it. False: it is still in use.
bool RebindVertexShader(uint8_t* base, u32 device, u32 shader, u32 declaration, u32 program);

// 0x82657048 D3DVertexShader_Bind(shader, unused, declaration, strides): binds every
// program of the shader to the declaration. `strides` is a guest array of the
// stream strides in bytes (a word each).
void BindVertexShader(uint8_t* base, u32 shader, u32 declaration, u32 strides);

}  // namespace renut::d3d::port
