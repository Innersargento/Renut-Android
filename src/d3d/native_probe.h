#pragma once
// Native renderer reconnaissance (native_probe.cpp).
#include <cstdint>
#include <string>

#include <rex/ppc/context.h>

// The engine functions read in the recompiled PowerPC, overridden by
// native_probe.cpp to be counted. X(symbol)
#define RENUT_PROBE_ENGINE(X) \
  X(sub_823AA1A0)             \
  X(sub_823A9A58)             \
  X(sub_823A9908)             \
  X(sub_823A9970)             \
  X(sub_823B08C0)             \
  X(sub_823B0CB0)             \
  X(sub_823B1260)             \
  X(sub_823B13C8)             \
  X(sub_823B1658)             \
  X(sub_823B1DE8)             \
  X(sub_823B21E0)             \
  X(sub_823B2478)             \
  X(sub_823B2E98)             \
  X(sub_823B3A10)             \
  X(sub_823B40A8)             \
  X(sub_823B4468)             \
  X(sub_823DBEB8)             \
  X(sub_823DBFB8)             \
  X(sub_823DC040)             \
  X(sub_823DC0B0)
// The ones overridden elsewhere, whose overrides call EngineEnter: in
// game_fixes.cpp sub_823B18C8 the scene view job, sub_823B3598/sub_823B3CE0 the
// dynamic and cached shadow-map jobs, sub_823B0698 the object draw; in
// native_engine.cpp sub_823A9C38 the engine's world draw.
#define RENUT_PROBE_ENGINE_EXTERNAL(X) \
  X(sub_823A9C38)                      \
  X(sub_823B18C8)                      \
  X(sub_823B3598)                      \
  X(sub_823B3CE0)                      \
  X(sub_823B0698)

namespace renut::native_probe {

enum Engine : uint32_t {
#define X(symbol) kEngine_##symbol,
  RENUT_PROBE_ENGINE(X) RENUT_PROBE_ENGINE_EXTERNAL(X)
#undef X
  kEngineCount,
};

// Where the reports go (the runtime's files directory).
void SetFilesDirectory(std::string directory);
// Called by the D3D layer on entry to every intercepted function.
void D3DEnter(uint32_t function, PPCContext& ctx, uint8_t* base);
// Called on entry to an engine function of the lists above.
void EngineEnter(uint32_t which, PPCContext& ctx, uint8_t* base);

}  // namespace renut::native_probe
