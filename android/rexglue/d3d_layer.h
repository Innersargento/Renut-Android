#pragma once
// Interception layer for the game's Direct3D (Xbox 360 XDK) entry points.
//
// Every function listed in d3d_functions.inc is overridden (the recompiled
// definitions are weak). With renut_renderer = "xenos" each override counts the
// call and forwards to the original, which emits PM4 for RexGlue's emulated
// Xenos. With "plume" a registered handler may take the call instead, so the
// native Plume renderer draws it and the emulated GPU only keeps the control
// flow (fences, vertical blank, swaps) it needs.
#include <cstdint>
#include <string>

#include <rex/ppc/context.h>

namespace renut::d3d {

enum Function : uint32_t {
#define X(symbol, name) k##name,
#include "d3d_functions.inc"
#undef X
  kFunctionCount,
};

// Returns true when it handled the call; false forwards to the original.
using Handler = bool (*)(PPCContext& ctx, uint8_t* base);

bool PlumeActive();
void SetHandler(Function function, Handler handler);
void SetShaderDumpDirectory(std::string directory);
// The runtime's files directory, where plume_d3d.cpp reads plume-spin.
void SetRuntimeFilesDirectory(std::string directory);
// While the app is in the background the game's fence wait polls slowly, so a
// paused game does not keep waking a core (plume_d3d.cpp).
void SetBackground(bool background);
// Installs the plume-mode handlers (plume_d3d.cpp).
void RegisterPlumeHandlers();

}  // namespace renut::d3d
