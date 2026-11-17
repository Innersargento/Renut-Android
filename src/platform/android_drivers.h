// The app's drivers for the SDK's systems on Android, served by one module the app registers in code
// (RenutApp::OnRegisterModules): the window (ui "android"), the audio output (apu "aaudio"), the
// controllers (hid "touch", touch_input.h). There is no graphics driver: the app hosts its GPU backend itself
// (gpu/core/host.cpp).
#pragma once
#include <android/native_window.h>

#include <cstdint>

namespace rex {
class IModule;
}

namespace renut::android {

// The module that serves the drivers.
rex::IModule* DriverModule();

// The audio output's underruns since its stream opened (the device found nothing to play: a click) and its
// buffer in frames, which grows by a burst on each.
struct AudioOutputStats {
  int32_t xruns = 0, buffer_frames = 0;
};
AudioOutputStats AudioOutput();

// The window the game draws in, from the Java surface. The driver holds a reference of its own.
// Null while there is none (the app is in the background).
void SetSurface(ANativeWindow* window);
ANativeWindow* Surface();

// The window's life, as the activity reports it: the size changed, the window gained or lost focus, or the
// player closed it. Any thread; the UI thread hears of it the next time it waits for events.
void NotifyResized();
void NotifyFocus(bool focused);
void NotifyClose();

}  // namespace renut::android
