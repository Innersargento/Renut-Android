#include "platform/android_drivers.h"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include <rex/apu/audio_driver.h>
#include <rex/driver_context.h>
#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/hid/input_driver.h>
#include <rex/module.h>
#include <rex/ui/ui_driver.h>

#include "platform/log.h"
#include "platform/touch_input.h"

namespace renut::android {
namespace {

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

std::mutex g_surface_mutex;
ANativeWindow* g_surface = nullptr;

class WindowEvents {
 public:
  enum class Kind { kResize, kFocus, kClose };
  struct Event {
    Kind kind;
    bool focused;
  };

  void Post(Kind kind, bool focused = false) {
    {
      std::lock_guard lock(mutex_);
      events_.push_back({kind, focused});
    }
    condition_.notify_all();
  }
  void Wake() {
    {
      std::lock_guard lock(mutex_);
      woken_ = true;
    }
    condition_.notify_all();
  }
  // Waits for an event or a wake-up, and returns what is queued.
  std::deque<Event> Wait(uint32_t timeout_ms) {
    std::unique_lock lock(mutex_);
    if (events_.empty() && !woken_ && timeout_ms) {
      condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !events_.empty() || woken_; });
    }
    woken_ = false;
    std::deque<Event> taken;
    taken.swap(events_);
    return taken;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Event> events_;
  bool woken_ = false;
};

WindowEvents g_events;

class WindowDriver final : public rex::ui::IUIDriver {
 public:
  std::string_view GetName() const override { return "android"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return rex::Ok(); }
  void Shutdown() override { opened_ = false; }

  bool HasDisplay() const override { return Surface() != nullptr; }

  // The window is the Java surface, already there: opening it is reading its size.
  rex::Result<void> OpenWindow(const rex::ui::WindowConfig&) override {
    if (!Surface()) return rex::Err(rex::ErrorCategory::kPlatform, "there is no surface to draw in");
    opened_ = true;
    return rex::Ok();
  }
  void CloseUIWindow() override { opened_ = false; }

  void SetTitle(std::string_view) override {}
  void SetSize(rex::u32, rex::u32) override {}
  void SetLimits(rex::u32, rex::u32, rex::ui::AspectRatio) override {}
  void SetFullscreen(bool, bool, rex::i32, rex::u32, rex::u32) override {}
  void RaiseWindow() override {}
  void SetCursorShape(bool, rex::ui::CursorShape) override {}
  void SetPointerMode(rex::PointerMode) override {}
  void CaptureMouse(bool) override {}
  rex::f32 GetContentScale() const override { return 1.0f; }

  rex::ui::WindowSize GetWindowSize() const override {
    std::lock_guard lock(g_surface_mutex);
    if (!g_surface) return {};
    const int width = ANativeWindow_getWidth(g_surface), height = ANativeWindow_getHeight(g_surface);
    return {width > 0 ? uint32_t(width) : 0u, height > 0 ? uint32_t(height) : 0u};
  }
  bool HasFocus() const override { return focused_.load(); }
  void SetTextInputActive(bool) override {}

  // Plume is handed the surface by the app (PlumeRenderer::Start), not through the SDK's native window
  // description, which has no Android window.
  rex::Result<rex::NativeWindow> GetNativeWindow() const override {
    return rex::Err<rex::NativeWindow>(rex::ErrorCategory::kNotFound, "the window is an Android surface");
  }

  bool WaitEvents(rex::ui::IUIEventListener& listener, rex::u32 timeout_ms) override {
    for (const auto& event : g_events.Wait(timeout_ms)) {
      switch (event.kind) {
        case WindowEvents::Kind::kResize: {
          rex::ui::UISetupEvent resized(rex::ui::UIEvent::Type::kResize);
          listener.OnUIEvent(resized);
          break;
        }
        case WindowEvents::Kind::kFocus: {
          focused_.store(event.focused);
          rex::ui::UISetupEvent focus(rex::ui::UIEvent::Type::kFocus);
          listener.OnUIEvent(focus);
          break;
        }
        case WindowEvents::Kind::kClose: {
          rex::ui::UIEvent close(rex::ui::UIEvent::Type::kClose);
          listener.OnUIEvent(close);
          quit_ = true;
          break;
        }
      }
    }
    return !quit_;
  }
  void WakeUp() override { g_events.Wake(); }

  rex::Result<rex::u32> ShowMessageBox(rex::MessageType, std::string_view title, std::string_view body,
                                       std::span<const std::string>) override {
    REXLOG_CAT_WARN(rex::log::renut, "{}: {}", title, body);
    return rex::Err<rex::u32>(rex::ErrorCategory::kPlatform, "the app shows no message boxes");
  }
  rex::Result<std::vector<std::filesystem::path>> ShowFileDialog(const rex::ui::FileDialogConfig&) override {
    return rex::Err<std::vector<std::filesystem::path>>(rex::ErrorCategory::kPlatform, "the app has no file dialog");
  }
  std::vector<rex::ui::Monitor> GetDisplays() override {
    const rex::ui::WindowSize size = GetWindowSize();
    rex::ui::Monitor monitor;
    monitor.name = "Android";
    monitor.width = size.width_px;
    monitor.height = size.height_px;
    return {monitor};
  }
  std::vector<rex::ui::HostLocale> GetPreferredLocales() override { return {{"en", "US"}}; }
  std::string GetClipboardText() override { return {}; }
  void SetClipboardText(std::string_view) override {}

 private:
  bool opened_ = false;
  bool quit_ = false;
  std::atomic<bool> focused_{true};
};

// ---------------------------------------------------------------------------
// The audio output
// ---------------------------------------------------------------------------

// The device's own thread asks for frames: the SDK's mixer fills them (the fill function), and the
// stream is stereo, which is what makes the mixer fold its 5.1 down.
// The output's underruns (the device found nothing to play) and its buffer, for the underrun log.
std::atomic<int32_t> g_output_xruns{0}, g_output_buffer{0};

class AAudioOutput final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return "aaudio"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return rex::Ok(); }
  void Shutdown() override { CloseDevice(); }

  rex::Result<rex::u32> OpenDevice(rex::u32, rex::apu::AudioFill fill) override {
    CloseDevice();
    fill_ = std::move(fill);
    disconnected_ = false;
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
      return rex::Err<rex::u32>(rex::ErrorCategory::kPlatform, "AAudio_createStreamBuilder failed");
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, kChannels);
    AAudioStreamBuilder_setSampleRate(builder, int32_t(rex::apu::kAudioSampleRate));
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(builder, &AAudioOutput::DataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, &AAudioOutput::ErrorCallback, this);
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || !stream_) {
      stream_ = nullptr;
      return rex::Err<rex::u32>(rex::ErrorCategory::kPlatform,
                                std::string("AAudio openStream failed: ") + AAudio_convertResultToText(opened));
    }
    burst_ = std::max<int32_t>(AAudioStream_getFramesPerBurst(stream_), 1);
    capacity_ = AAudioStream_getBufferCapacityInFrames(stream_);
    // 16 ms of bursts (8 at a burst of 96): small next to the 85 ms of mixed frames the title keeps queued, and
    // room for the callback, which mixes under the SDK's locks and wakes its audio worker, to run late now and
    // then without the device reading nothing (a click). Four bursts (8 ms) clicked on the S26 with the stream in
    // MMAP. Grown by a burst on each underrun.
    const int32_t wanted = std::max<int32_t>(4, (int32_t(rex::apu::kAudioSampleRate) * 16 / 1000 + burst_ - 1) / burst_);
    buffer_frames_ = std::min<int32_t>(capacity_, burst_ * wanted);
    if (const int32_t granted = AAudioStream_setBufferSizeInFrames(stream_, buffer_frames_); granted > 0) {
      buffer_frames_ = granted;
    }
    last_xruns_ = 0;
    g_output_xruns.store(0, std::memory_order_relaxed);
    g_output_buffer.store(buffer_frames_, std::memory_order_relaxed);
    REXLOG_CAT_INFO(rex::log::renut, "aaudio: {} Hz, {} ch, burst {} frames, buffer {}/{} frames, performance mode {}",
                    AAudioStream_getSampleRate(stream_), AAudioStream_getChannelCount(stream_), burst_,
                    buffer_frames_, capacity_, int(AAudioStream_getPerformanceMode(stream_)));
    const aaudio_result_t started = AAudioStream_requestStart(stream_);
    if (started != AAUDIO_OK) {
      CloseDevice();
      return rex::Err<rex::u32>(rex::ErrorCategory::kPlatform,
                                std::string("AAudio requestStart failed: ") + AAudio_convertResultToText(started));
    }
    return rex::u32(kChannels);
  }

  void CloseDevice() override {
    if (AAudioStream* stream = std::exchange(stream_, nullptr)) {
      AAudioStream_requestStop(stream);
      AAudioStream_close(stream);  // returns once the callback thread is gone
    }
    fill_ = nullptr;
  }

  bool IsDeviceLost() const override { return disconnected_.load(std::memory_order_acquire); }

 private:
  static constexpr int32_t kChannels = 2;

  static aaudio_data_callback_result_t DataCallback(AAudioStream* stream, void* user, void* audio, int32_t frames) {
    auto* self = static_cast<AAudioOutput*>(user);
    float* out = static_cast<float*>(audio);
    const size_t samples = size_t(frames) * kChannels;
    if (self->fill_) {
      self->fill_(std::span<float>(out, samples), rex::u32(kChannels));
    } else {
      std::fill(out, out + samples, 0.0f);
    }
    self->Tune(stream);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  // The device went away (headphones, a route change): AAudio stops the stream and reports it from its own
  // thread; the audio system sees the loss and opens a new device.
  static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    auto* self = static_cast<AAudioOutput*>(user);
    REXLOG_CAT_WARN(rex::log::renut, "aaudio: stream error {}", AAudio_convertResultToText(error));
    self->disconnected_.store(true, std::memory_order_release);
  }

  // Lets the buffer grow after an underrun, never shrink. On the callback's thread, with the stream the callback
  // was given: CloseDevice clears stream_ on another thread before AAudio_close waits for this one to end.
  void Tune(AAudioStream* stream) {
    if (!stream) return;
    const int32_t xruns = AAudioStream_getXRunCount(stream);
    if (xruns <= last_xruns_) return;
    last_xruns_ = xruns;
    if (buffer_frames_ < capacity_) {
      const int32_t granted = AAudioStream_setBufferSizeInFrames(stream, std::min(buffer_frames_ + burst_, capacity_));
      if (granted > 0) buffer_frames_ = granted;
    }
    // Read by the underrun log (audio_threads.cpp): nothing is logged from the device's real-time thread.
    g_output_xruns.store(xruns, std::memory_order_relaxed);
    g_output_buffer.store(buffer_frames_, std::memory_order_relaxed);
  }

  AAudioStream* stream_ = nullptr;
  rex::apu::AudioFill fill_;
  std::atomic<bool> disconnected_{false};
  int32_t burst_ = 0, capacity_ = 0, buffer_frames_ = 0, last_xruns_ = 0;
};

// ---------------------------------------------------------------------------
// The module
// ---------------------------------------------------------------------------

std::unique_ptr<rex::IDriver> CreateWindow() { return std::make_unique<WindowDriver>(); }
std::unique_ptr<rex::IDriver> CreateAudio() { return std::make_unique<AAudioOutput>(); }
std::unique_ptr<rex::IDriver> CreateInput() { return std::make_unique<TouchInput>(); }

// No graphics driver: the app runs without one (GraphicsFamily::kNone) and hosts its GPU backend itself
// (gpu/core/host.cpp).
const std::array<rex::DriverRegistration, 3> kDrivers = {
    rex::DriverRegistration{rex::DriverType::kUI, "android", {}, &CreateWindow},
    rex::DriverRegistration{rex::DriverType::kAudio, "aaudio", {}, &CreateAudio},
    rex::DriverRegistration{rex::DriverType::kInput, "touch", {}, &CreateInput},
};

class Drivers final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override { return "renut-drivers"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return rex::Ok(); }
  void Disconnect() override {}
  rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace

AudioOutputStats AudioOutput() {
  return {g_output_xruns.load(std::memory_order_relaxed), g_output_buffer.load(std::memory_order_relaxed)};
}

rex::IModule* DriverModule() {
  static Drivers module;
  return &module;
}

void SetSurface(ANativeWindow* window) {
  std::lock_guard lock(g_surface_mutex);
  if (window) ANativeWindow_acquire(window);
  if (g_surface) ANativeWindow_release(g_surface);
  g_surface = window;
}

ANativeWindow* Surface() {
  std::lock_guard lock(g_surface_mutex);
  return g_surface;
}

void NotifyResized() { g_events.Post(WindowEvents::Kind::kResize); }
void NotifyFocus(bool focused) { g_events.Post(WindowEvents::Kind::kFocus, focused); }
void NotifyClose() { g_events.Post(WindowEvents::Kind::kClose); }

}  // namespace renut::android
