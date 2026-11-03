#pragma once
// D3DDevice_BeginCommandBuffer (0x82657CB0) and D3DDevice_EndCommandBuffer (0x826582A0) natively
// (see command_buffers.cpp).
namespace renut::d3d::port {

void RegisterCommandBuffers();

}  // namespace renut::d3d::port
