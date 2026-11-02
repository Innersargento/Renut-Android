#pragma once
// The smaller device calls, natively (see device.h), ported from the retail
// executable with idalib (2026-10-30):
//   D3DDevice_GetDeviceCaps 0x82658948      D3DResource_GetDevice 0x82654D50
//   D3DDevice_DrawVerticesUP 0x8222E520
#include "d3d/device/device.h"

namespace renut::d3d::port {

// void D3DDevice_GetDeviceCaps(D3DDevice*, D3DCAPS9*): N&B's constant caps.
void GetDeviceCaps(uint8_t* base, u32 caps_va);

// void D3DResource_GetDevice(D3DResource*, D3DDevice**): the device D3D finds through
// the kernel's VdGlobalDevice (which slot depends on the device type), referenced.
void GetResourceDevice(uint8_t* base, u32 device_type, u32 out_va);

// The end of D3DDevice_DrawVerticesUP once the vertices have a place: they are copied
// in, and the ring pointer goes back to the one BeginVertices saved.
void FinishVerticesUP(Device device, u32 destination, u32 data, u32 bytes);

}  // namespace renut::d3d::port
