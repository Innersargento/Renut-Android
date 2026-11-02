#pragma once
// D3D internals the ported device layer used to leave to the recompiled game, now native.
// They are hooked like the rest (hooks/functions.h), so
// every caller of the original, recompiled or ported, reaches these.
//
//   0x8264DC08  D3D::CBlocker::Finish (the end of a wait on the GPU)
//   0x8265F3B0  D3D::FitScalerParameters (a scaler rectangle fitted to the source size)
#include "d3d/device/device.h"

namespace renut::d3d::port {

void RegisterLeaves();
void RegisterCommandLists();  // lists.cpp

}  // namespace renut::d3d::port
