#pragma once
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/surface_android.h>
#include <atomic>
#include <mutex>

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
  ~GameWindow() override { EnterDestructor(); if(window_) ANativeWindow_release(window_); }
  // The surface changes when the app leaves and returns to the screen. Null while
  // there is none; the size is left as it was.
  void SetWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if(window) ANativeWindow_acquire(window);
    if(window_) ANativeWindow_release(window_);
    window_ = window;
  }
  void Pump() {
    WindowDestructionReceiver receiver(this);
    {
      std::lock_guard<std::mutex> lock(window_mutex_);
      if(window_) OnActualSizeUpdate(ANativeWindow_getWidth(window_),ANativeWindow_getHeight(window_),receiver);
    }
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
    std::lock_guard<std::mutex> lock(window_mutex_);
    if(!window_) return {};
    return std::make_unique<rex::ui::AndroidNativeWindowSurface>(window_);
  }
  void RequestPaintImpl() override { paint_=true; }
 private:
  ANativeWindow* window_;
  std::mutex window_mutex_;
  std::atomic<bool> paint_{true};
};
}
