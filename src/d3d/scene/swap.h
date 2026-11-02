#pragma once
// D3DDevice_Swap (0x82287590) natively. The native front end has recorded the frame before it runs; the video hardware and display calls it makes are the recompiled game's, called as the original calls them.
namespace renut::d3d::port {

void RegisterSwap();

}  // namespace renut::d3d::port
