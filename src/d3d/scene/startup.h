#pragma once
// The device's start, natively: D3D::InitializeHardwareDevice (0x82659F30, which Direct3D_CreateDevice calls) and
// the InitializePresentationParameters it calls (0x82659AA8), without the kernel's video code. The originals start
// the GPU's engines (VdInitializeEngines, with a callback that writes the hardware's registers), register the
// game's graphics interrupt handler (VdSetGraphicsInterruptCallback, sub_826573F0, which reads and writes the
// display registers), ask whether the memory link trained (VdIsHSIOTrainingSucceeded) and query the video mode
// (VdQueryVideoMode). Here the GPU backend is started instead (PlumeGpuStart), the video mode is the host's
// (host::GetVideoMode), and no interrupt handler is registered: the vertical blank is the GPU host's
// (gpu/core/host.cpp). The rest are the same calls in the same order, read off the generated code.
namespace renut::d3d::port {

// Registers the handler.
void RegisterStartup();

}  // namespace renut::d3d::port
