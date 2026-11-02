#pragma once
// The frame-level device calls, natively: Present, the gamma and scaler setters,
// DrawIndexedVerticesUP and the memory export pair. Ported from the retail executable
// with idalib (2026-10-30):
//   D3DDevice_Present 0x82654778           D3DDevice_SetGammaRamp 0x8264D948
//   D3DDevice_SetPWLGamma 0x8264DA18       D3DDevice_SetScalerParameters 0x8265F688
//   D3DDevice_DrawIndexedVerticesUP 0x82656728
//   D3DDevice_BeginExport 0x82269618       D3DDevice_EndExport 0x822693B0
// They are sequences of other Direct3D calls and of writes to the device and the ring;
// the calls go to the same entry points the originals call, so the hooked ones (the
// native front end's and the other ports') run as they do for the original.
namespace renut::d3d::port {

// Registers the handlers.
void RegisterFrame();

}  // namespace renut::d3d::port
