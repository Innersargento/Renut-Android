#pragma once
// Interception layer for the game's Direct3D (Xbox 360 XDK) entry points.
//
// Every function listed in hooks/functions.h is overridden by a REX_HOOK_RAW
// (hooks/layer.cpp makes one per entry; the recompiled definitions are weak). The native
// front end or a registered handler may take the call; otherwise the recompiled original
// runs.
#include <cstdint>
#include <string>

#include <rex/ppc/context.h>

namespace renut::d3d {

enum Function : uint32_t {
#define X(symbol, name) k##name,
#include "d3d/hooks/functions.h"
#undef X
  kFunctionCount,
};

// Returns true when it handled the call; false forwards to the original.
using Handler = bool (*)(PPCContext& ctx, uint8_t* base);

void SetHandler(Function function, Handler handler);
void SetShaderDumpDirectory(std::string directory);
// Installs the plume-mode handlers (plume_d3d.cpp).
void RegisterPlumeHandlers();
// Set while Direct3D's predicated tiling is collapsed to one band (the host has
// no EDRAM limit). A predicated draw then runs if it is marked for any band: the
// game culls its objects per band, and a draw marked only for a lower band
// would otherwise never be drawn.
void SetSingleBinTiling(bool single);
bool GetSingleBinTiling();

}  // namespace renut::d3d
