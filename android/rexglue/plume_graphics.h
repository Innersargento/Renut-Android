#pragma once
#include <rex/system/interfaces/graphics.h>
#include <android/native_window.h>
#include <atomic>
#include <memory>
#include <string>

namespace renut {
class PlumeGraphics final : public rex::system::IGraphicsSystem {
 public:
  PlumeGraphics(ANativeWindow* window, std::string hooks, std::string directory, std::string library,
                const std::atomic<bool>& stopped);
  ~PlumeGraphics() override;
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher*, rex::system::KernelState*) override;
  bool has_presentation() const override;
  bool PresentFrontBuffer(const uint32_t*, uint32_t width, uint32_t height);
  void InitializeRingBuffer(uint32_t, uint32_t) override;
  void EnableReadPointerWriteBack(uint32_t, uint32_t) override;
  void SetInterruptCallback(uint32_t, uint32_t) override;
  void Shutdown() override;
  std::string report() const;
 private:
  struct State;
  std::unique_ptr<State> state_;
};
}
