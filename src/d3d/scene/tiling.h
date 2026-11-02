#pragma once
// BeginTiling 0x822788A0, EndTiling 0x82212328, RunCommandBuffer 0x82222888 and
// AllocateEdramMemory 0x82661C38, natively; ported from the retail executable with idalib
// (2026-10-30). The helpers they call (the dirty-state writers, the list pages, the EDRAM
// bit marker) stay the recompiled game's, called as the originals call them.
namespace renut::d3d::port {

void RegisterTiling();            // BeginTiling, EndTiling
void RegisterEdram();

}  // namespace renut::d3d::port
