#pragma once
// Native renderer for renut_renderer = "plume". Owns the Android window's
// swapchain through Plume and is fed by the Direct3D interception layer.
// Stage 1: presents the colour of the game's last clear, which proves the
// path from the game's Direct3D calls to the screen.
#include <android/native_window.h>

#include <memory>
#include <string>

namespace plume {
struct RenderDevice;
struct RenderCommandQueue;
struct RenderCommandList;
struct RenderTexture;
}  // namespace plume

namespace renut {

// Frames the renderer keeps in flight. The command processor records frame N
// while the GPU runs N-1, so a frame costs the larger of the two instead of
// their sum; it sizes the per-frame resources on both sides (command lists,
// fences, acquire semaphores and the slices of the upload ring).
inline constexpr uint32_t kPlumeFramesInFlight = 2;

// Where the driver's compiled pipelines are kept between runs. Call it before
// the renderer starts: entering a scene for the first time spends most of its
// frames inside vkCreateGraphicsPipelines, and a warm cache skips that.
void SetPlumePipelineCachePath(const std::string& path);
// Writes it out. Worth calling whenever new pipelines were built, since the
// process is usually killed rather than shut down.
void SavePlumePipelineCache();

class PlumeRenderer {
 public:
  // Uses the Vulkan loader already opened for the session.
  static bool Start(ANativeWindow* window, std::string* error);
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
  bool AttachWindow(ANativeWindow* window);
  void ReleaseForShutdown();

  ~PlumeRenderer();

 private:
  PlumeRenderer();
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace renut
