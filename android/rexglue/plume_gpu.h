#pragma once
// Graphics system for renut_renderer = "plume": the game's command stream is
// still parsed (Nuts & Bolts records its draws as PM4 and never calls
// D3DDevice_Draw*), but every draw, resolve and swap is executed by a native
// Plume renderer with the offline-recompiled shaders (generated/shaders)
// instead of RexGlue's Xenos emulation (EDRAM, shader translation).
#include <string>

namespace rex::system {
class IGraphicsSystem;
}

namespace renut {

// Requires PlumeRenderer::Start to have succeeded.
rex::system::IGraphicsSystem* CreatePlumeGraphicsSystem();

// Microcode the command processor meets without a recompiled shader is saved
// here (first 32), to extend the capture.
void SetPlumeMissDumpDirectory(const std::string& directory);

// Debug: with <root>/plume-memtrace present, Plume logs every memory block, buffer and
// image it makes or frees (with device addresses) and writes it to <root>/plume-mem.txt
// when the device is lost. Call before PlumeRenderer::Start so the first blocks are seen.
void ArmPlumeMemoryTrace(const std::string& root);

}  // namespace renut
