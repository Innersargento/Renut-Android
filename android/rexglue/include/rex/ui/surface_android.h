#pragma once
#include <android/native_window.h>
#include <rex/ui/surface.h>
namespace rex::ui {
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) { ANativeWindow_acquire(window_); }
  ~AndroidNativeWindowSurface() override { ANativeWindow_release(window_); }
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }
 protected:
  bool GetSizeImpl(uint32_t& width, uint32_t& height) const override {
    int w = ANativeWindow_getWidth(window_), h = ANativeWindow_getHeight(window_);
    width = w > 0 ? uint32_t(w) : 0; height = h > 0 ? uint32_t(h) : 0;
    return width && height;
  }
 private:
  ANativeWindow* window_;
};
}
