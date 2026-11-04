#pragma once
// The vertex shader binding, natively (see shader.h).
//
// A vertex shader's microcode names its vertex fetch constants in the order the
// compiler met them; binding it to a vertex declaration works out the constants
// (format, offset, stride, ordering) that match the declaration's elements and the
// streams' strides. The original writes them into the microcode; here the microcode
// is never written: the binding keeps them (BoundFetches) and the GPU backend is
// handed them with each load. Matching a vertex shader to a pixel shader only
// updates the program control word. Ported from the retail executable with idalib
// (2026-10-30):
//   D3D::PatchVertexShaderToMatchVertexDeclaration 0x821E5218
//   D3D::PatchVertexShaderToMatchPixelShader 0x82656DA0 (helpers 0x82656CE8, 0x82656C18)
//   the binding check and rebind 0x822127F8, D3DVertexShader_Bind 0x82657048
// The bit manipulation follows the PowerPC instruction by instruction (rotate and
// insert-under-mask), because that is what the game's own shaders were compiled
// against.
#include "d3d/device/device.h"

namespace renut::d3d::port {

// One vertex fetch of a program, as 0x821E5218 leaves it: the instruction slot of the fetch in the
// microcode and the three words of its fetch constant (format, swizzle, stride, offset).
struct VertexFetch {
  u32 slot, w0, w1, w2;
};
constexpr u32 kMaxVertexFetches = 64;

// 0x821E5218's computation (shader, declaration, per-stream strides in units of 4 bytes, program
// index): the fetches of the program matched to the declaration, ordered and merged by stream.
// Returns how many (0: the program has none); the guest's memory is not written.
u32 ComputeVertexFetches(uint8_t* base, u32 shader, u32 declaration, const u8* strides, u32 program,
                         VertexFetch (&fetches)[kMaxVertexFetches]);

// What a program of a shader is bound with: its fetches, or 0 when it never was. The front end
// sends them with the load of the program.
u32 BoundFetches(u32 shader, u32 program, VertexFetch (&fetches)[kMaxVertexFetches]);

// A shader object made anew at this address has no binding.
void ForgetBoundFetches(u32 shader);

// Binds one program: its fetches are worked out and kept.
void BindProgram(uint8_t* base, u32 shader, u32 declaration, const u8* strides, u32 program);

// What 0x82656DA0 (vertex shader's container, the program-control word to update, pixel shader's
// container or 0) does besides rewriting the microcode: the pixel shader's reserved interpolator
// count goes into the program-control word, with the same early outs.
void MatchProgramControl(uint8_t* base, u32 vertex_container, u32 program_control, u32 pixel_container);

// 0x822127F8 (device, shader, declaration, program): whether the program is bound to
// the declaration and the device's strides, rebinding it when it is not and the GPU is
// done with it. False: it is still in use.
bool RebindVertexShader(uint8_t* base, u32 device, u32 shader, u32 declaration, u32 program);

// 0x82657048 D3DVertexShader_Bind(shader, unused, declaration, strides): binds every
// program of the shader to the declaration. `strides` is a guest array of the
// stream strides in bytes (a word each).
void BindVertexShader(uint8_t* base, u32 shader, u32 declaration, u32 strides);

}  // namespace renut::d3d::port
