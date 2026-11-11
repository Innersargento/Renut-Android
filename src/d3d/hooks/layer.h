#pragma once
// Interception layer for the game's Direct3D (Xbox 360 XDK) entry points.
//
// Every function listed in hooks/functions.h is overridden by a REX_HOOK_RAW
// (hooks/layer.cpp makes one per entry; the recompiled definitions are weak). The native
// front end or a registered handler may take the call; otherwise the recompiled original
// runs.
#include <atomic>
#include <cstdint>
#include <string>

#include <rex/ppc/context.h>

namespace renut::d3d {

// Whether the per-call and per-draw counters of the native report are kept (cvar renut_d3d_stats, or
// renut_d3d_report_swaps > 0). Off, a counter costs one relaxed load; on, an atomic increment shared
// between threads. Counters the sync logic reads are not Bump()ed.
inline std::atomic<bool> g_stats{false};
inline void Bump(std::atomic<uint64_t>& counter) {
  if (g_stats.load(std::memory_order_relaxed)) counter.fetch_add(1, std::memory_order_relaxed);
}

enum Function : uint32_t {
#define X(symbol, name) k##name,
#include "d3d/hooks/functions.h"
#undef X
  kFunctionCount,
};

// Returns true when it handled the call; false forwards to the original.
using Handler = bool (*)(PPCContext& ctx, uint8_t* base);

void SetHandler(Function function, Handler handler);
// How many calls of a function were taken (by the native front end or a handler) and how many ran
// the recompiled original, since the start. Read by the native report.
void CallCounts(Function function, uint64_t& taken, uint64_t& original);
void SetShaderDumpDirectory(std::string directory);
// Installs the plume-mode handlers (plume_d3d.cpp).
void RegisterPlumeHandlers();
}  // namespace renut::d3d
