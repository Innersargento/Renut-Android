#pragma once
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/surface_android.h>
#include <atomic>

namespace renut {
// The JNI game thread owns the RexGlue UI loop; Java owns the SurfaceView.
class GameContext final : public rex::ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {} // Pumped every 8 ms.
  void PlatformQuitFromUIThread() override {}
};
class GameWindow final : public rex::ui::Window {
 public:
  GameWindow(GameContext& context, ANativeWindow* window)
      : Window(context,"Renut",1280,720), window_(window) { ANativeWindow_acquire(window_); }
  ~GameWindow() override { EnterDestructor(); ANativeWindow_release(window_); }
  void Pump() {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(ANativeWindow_getWidth(window_),ANativeWindow_getHeight(window_),receiver);
    if(paint_.exchange(false)) OnPaint();
  }
 protected:
  bool OpenImpl() override {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(ANativeWindow_getWidth(window_),ANativeWindow_getHeight(window_),receiver);
    OnFocusUpdate(true,receiver);
    return true;
  }
  void RequestCloseImpl() override {
    WindowDestructionReceiver receiver(this);
    OnBeforeClose(receiver); OnAfterClose();
  }
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags allowed) override {
    if(!(allowed & rex::ui::Surface::kTypeFlag_AndroidNativeWindow)) return {};
    return std::make_unique<rex::ui::AndroidNativeWindowSurface>(window_);
  }
  void RequestPaintImpl() override { paint_=true; }
 private:
  ANativeWindow* window_;
  std::atomic<bool> paint_{true};
};
}
