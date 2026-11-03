#pragma once
// D3DDevice_BeginCommandBuffer (0x82657CB0) and D3DDevice_EndCommandBuffer (0x826582A0) natively
// (see command_buffers.cpp).
namespace renut::d3d::port {

void RegisterCommandBuffers();
// Whether Begin, End and Run are the native ones: no segment is chained into a buffer, so the ring
// writes of a recording device are not read by anything.
bool NativeCommandBuffers();

}  // namespace renut::d3d::port
