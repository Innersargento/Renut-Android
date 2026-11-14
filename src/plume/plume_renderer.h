#pragma once
// Native renderer for renut_renderer = "plume". Owns the Android window's
// swapchain through Plume and is fed by the Direct3D interception layer.
// Stage 1: presents the colour of the game's last clear, which proves the
// path from the game's Direct3D calls to the screen.
#include <android/native_window.h>

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

// The platform's window: the Android surface.
using PlumeWindow = ANativeWindow;

// Frames the renderer keeps in flight. The GPU backend records frame N
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
// Whether the device takes the *_USCALED / *_SSCALED vertex formats (false on Qualcomm's own Adreno driver,
// where the layer retypes the vertex modules instead). Known once the renderer has started.
bool PlumeScaledVertexFormats();
// Whether the device runs Qualcomm's own Adreno driver (VK_DRIVER_ID_QUALCOMM_PROPRIETARY), which gets the few
// workarounds Turnip does not need. Known once the renderer has started.
bool PlumeQualcommDriver();
// Whether the device runs ARM's own Mali driver (VK_DRIVER_ID_ARM_PROPRIETARY). Known once the renderer has
// started.
bool PlumeArmDriver();
// The GPU, driver and Vulkan version, one per line, for the HUD (empty before the device exists).
std::string PlumeDriverText();
// Whether the device's limits leave out the bindless texture heaps (the Mali-G52: four bound sets, 256 textures
// a stage): each draw then binds a set of its own textures, and the layer rewrites the recompiled modules for it
// (spirv_descriptor_sets.h). Known once the renderer has started.
bool PlumePerDrawTextures();
// Images and views of the BC formats are made in the ETC2/EAC format of the same block size, for a Mali that
// samples ETC2 but not BC (the GPU backend transcodes the blocks: etc2_transcode.h). Set before any is made.
void PlumeSetBcAsEtc2(bool on);
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

class PlumeRenderer {
 public:
  // Whether this machine can run the renderer at all (a Vulkan loader to open), asked
  // before Start so the app can say so instead of failing half-way through setup.
  static bool IsAvailable();
  // Uses the Vulkan loader already opened for the session.
  static bool Start(PlumeWindow* window, std::string* error);
  static void Stop();
  static PlumeRenderer* Get();

  void SetClearColor(float r, float g, float b, float a);
  void Present();
  std::string report() const;

  plume::RenderDevice* device() const;
  plume::RenderCommandQueue* queue() const;

  // Executes `frame` (already ended), then scales `source` (width x height of it
  // is shown; in SHADER_READ layout after `frame` when PresentSamples(), in
  // COPY_SOURCE otherwise) onto the swapchain and presents, in one submission on
  // `slot`'s resources. `source` null presents the clear colour.
  //
  // Returns as soon as the work is queued: what `frame` used stays in use until
  // WaitForSlot(slot) returns, and the caller owns that wait.
  void SubmitAndPresent(plume::RenderCommandList* frame, plume::RenderTexture* source,
                        uint32_t width, uint32_t height, uint32_t slot);
  // Whether the frame is sampled by a render pass on the swapchain image (renut_plume_present_pass) instead
  // of blitted: the layout SubmitAndPresent's `source` is to be left in. Fixed once the renderer has started.
  bool PresentSamples() const;
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

  // The size Java's SurfaceView reports for `window` (surfaceChanged), callable before
  // the renderer exists. The swapchain follows it at the next present: Android does not
  // fail the acquire when a surface turns, it scales the old images onto the new shape (a
  // game opened in portrait and turned to landscape was drawn stretched for good).
  static void SetSurfaceSize(PlumeWindow* window, uint32_t width, uint32_t height);

  ~PlumeRenderer();

 private:
  PlumeRenderer();
  // Under swap_mutex: recreates the swapchain when the surface's size moved away from it,
  // and logs what the window, the surface and the swapchain say.
  void FollowSurfaceSize();
  // Under swap_mutex: makes the swapchain again (every resize goes through it) as the present options ask.
  bool ResizeSwapchain();
  void LogSurface(const char* when);
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace renut
