#pragma once
// Native renderer for renut_renderer = "plume". Owns the Android window's
// swapchain through Plume and is fed by the Direct3D interception layer.
// Stage 1: presents the colour of the game's last clear, which proves the
// path from the game's Direct3D calls to the screen.
#include <android/native_window.h>

#include <memory>
#include <string>

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

  ~PlumeRenderer();

 private:
  PlumeRenderer();
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace renut
