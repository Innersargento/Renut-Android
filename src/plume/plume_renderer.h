#pragma once
// Native renderer for renut_renderer = "plume". Owns the Android window's
// swapchain through Plume and is fed by the Direct3D interception layer.
// Stage 1: presents the colour of the game's last clear, which proves the
// path from the game's Direct3D calls to the screen.
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace plume {
struct RenderDevice;
struct RenderCommandQueue;
struct RenderCommandList;
struct RenderTexture;
}  // namespace plume

namespace renut {

// The platform's window: the Android surface, or on Windows the HWND, which the
// callers hand over as an opaque pointer (windows/main_windows.cpp).
#if defined(__ANDROID__)
using PlumeWindow = ANativeWindow;
#else
struct PlumeWindowTag;
using PlumeWindow = PlumeWindowTag;
#endif

// Frames the renderer keeps in flight. The command processor records frame N
// while the GPU runs N-1, so a frame costs the larger of the two instead of
// their sum; it sizes the per-frame resources on both sides (command lists,
// fences, acquire semaphores and the slices of the upload ring).
inline constexpr uint32_t kPlumeFramesInFlight = 2;

// Where the driver's compiled pipelines are kept between runs. Call it before
// the first pipeline: entering a scene for the first time spends most of its
// frames inside vkCreateGraphicsPipelines, and a warm cache skips that. Once the
// renderer has started, the file starts loading in the background.
void SetPlumePipelineCachePath(const std::string& path);
// Writes out what is new, if anything. Worth calling often, since the process is
// usually killed rather than shut down; `now` skips the minimum interval.
void SavePlumePipelineCache(bool now = false);
// Whether the driver can be asked for a pipeline it already has without
// compiling one (VK_EXT_pipeline_creation_cache_control). Known once the
// renderer has started.
bool PlumePipelineLookupSupported();
// While one is alive on a thread, pipeline creation there only looks in the
// cache: a pipeline that would have to be compiled comes back null at once.
class PlumePipelineLookupOnly {
 public:
  PlumePipelineLookupOnly();
  ~PlumePipelineLookupOnly();
  PlumePipelineLookupOnly(const PlumePipelineLookupOnly&) = delete;
  PlumePipelineLookupOnly& operator=(const PlumePipelineLookupOnly&) = delete;
};
// Pipelines since launch that the cache already had, and that were compiled.
void PlumePipelineCacheCounts(uint64_t* found, uint64_t* compiled);
// Whether a queue submission or a fence wait has answered VK_ERROR_DEVICE_LOST: the
// GPU faulted, and every later call says the same.
bool PlumeDeviceLost();
// Whether the device was made with VK_EXT_conditional_rendering (storage buffers
// then also carry the conditional rendering usage). Known once the renderer has started.
bool PlumeConditionalRenderingSupported();

class PlumeRenderer {
 public:
  // Uses the Vulkan loader already opened for the session.
  static bool Start(PlumeWindow* window, std::string* error);
  static void Stop();
  static PlumeRenderer* Get();

  void SetClearColor(float r, float g, float b, float a);
  void Present();
  std::string report() const;

  plume::RenderDevice* device() const;
  plume::RenderCommandQueue* queue() const;

  // Executes `frame` (already ended), then scales `source` (in COPY_SOURCE
  // layout after `frame`; width x height of it is shown) onto the swapchain and
  // presents, in one submission on `slot`'s resources. `source` null presents
  // the clear colour.
  //
  // Returns as soon as the work is queued: what `frame` used stays in use until
  // WaitForSlot(slot) returns, and the caller owns that wait.
  void SubmitAndPresent(plume::RenderCommandList* frame, plume::RenderTexture* source,
                        uint32_t width, uint32_t height, uint32_t slot);
  // Waits for whatever SubmitAndPresent left running on `slot`, if anything.
  void WaitForSlot(uint32_t slot);
  // Waits for every slot. For teardown and the ring's overflow path.
  void WaitForAllSlots();

  // Background support. The Android surface goes away when the app leaves the
  // screen, but the game must stay alive to come back to.
  //
  // PauseForBackground stops the frame loop at its next present (the command
  // thread parks there, so the game stalls behind its own fences) and never
  // touches the dead swapchain again. AttachWindow, once a new surface exists,
  // replaces the swapchain and lets the loop run. ReleaseForShutdown wakes a
  // parked thread so the runtime can stop while paused.
  void PauseForBackground();
  // The player's own pause: the same parking at the next present, but the surface
  // is alive, so the last frame stays on screen and nothing is saved or replaced.
  void SetPaused(bool paused);
  bool AttachWindow(PlumeWindow* window);
  void ReleaseForShutdown();

  ~PlumeRenderer();

 private:
  PlumeRenderer();
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace renut
