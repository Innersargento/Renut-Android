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
  // presents, in one submission. Returns once the GPU finished, so everything
  // `frame` used may be reused. `source` null presents the clear colour.
  void SubmitAndPresent(plume::RenderCommandList* frame, plume::RenderTexture* source,
                        uint32_t width, uint32_t height);

  ~PlumeRenderer();

 private:
  PlumeRenderer();
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace renut
