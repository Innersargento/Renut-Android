#pragma once
// Graphics system for renut_renderer = "plume": the game's command stream is
// still parsed (Nuts & Bolts records its draws as PM4 and never calls
// D3DDevice_Draw*), but every draw, resolve and swap is executed by a native
// Plume renderer with the offline-recompiled shaders (generated/shaders)
// instead of RexGlue's Xenos emulation (EDRAM, shader translation).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::system {
class KernelState;
}
namespace rex::runtime {
class FunctionDispatcher;
}

namespace renut {

// The GPU, hosted by the app: the Runtime is set up with no graphics system
// (the SDK's native-rendering boundary) and the app starts this after
// Runtime::Setup (it needs PlumeRenderer::Start to have succeeded) and stops it
// before Runtime::Shutdown. The Vd exports the SDK would have routed to its
// graphics system (interrupt callback, ring buffer, read pointer write-back)
// are overridden in kernel_overrides.cpp and land in the functions below.
bool StartPlumeGpu(rex::runtime::FunctionDispatcher* dispatcher, rex::system::KernelState* kernel);
void StopPlumeGpu();
bool PlumeGpuHosted();
void PlumeGpuSetInterruptCallback(uint32_t callback, uint32_t user_data);
void PlumeGpuInitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
void PlumeGpuEnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

// Microcode the GPU backend meets without a recompiled shader is saved
// here (first 32), to extend the capture.
void SetPlumeMissDumpDirectory(const std::string& directory);

// The loading-screen hold, UnleashedRecomp's method with the pipelines built
// ahead at start from <files>/plume-recipes.bin (game_fixes.cpp drives it).
// The game asked for a scene: the pipelines drawn from now on are tagged with
// it, and those tagged with it in earlier runs go to the front of the queue and
// are built at full speed while its loading screen is up. Never blocks.
// `asset` and `ref` are for the log.
void PlumeSceneRequested(uint32_t scene, uint32_t asset, uint32_t ref);
// Whether the requested scene's pipelines are all built (or the building has
// stopped moving for a minute): until then the game's loading screen stays.
bool PlumeSceneReady();
// The loading screen let go of the scene; `held` if it waited for pipelines.
void PlumeSceneOpened(bool held);

// A model's shader pairs, read from its render data as it loads (game_fixes.cpp):
// the physical address and length of each shader's microcode, ps_dwords 0 for a
// draw without a pixel shader. Plume builds the program each pair needs, ahead
// of its first draw; while a scene loads its loading screen waits for them, as
// for the recorded ones. Any thread.
struct PlumeShaderPair {
  uint32_t vs_address = 0, vs_dwords = 0, ps_address = 0, ps_dwords = 0;
  bool alpha_test = false;  // drawn in the alpha-to-coverage pass
  // The engine passes (bit p for pass p, 0-7) whose tables reach the record:
  // with the render states each pass is known to draw with (learned from the
  // draws stamped with native_d3d::EnginePass), its pipelines are predicted
  // whole, not only their programs. 0 when no pass table was found.
  uint8_t passes = 0;
};
void PlumePredictPipelines(std::vector<PlumeShaderPair> pairs);

}  // namespace renut
