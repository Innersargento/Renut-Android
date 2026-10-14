#pragma once
// reNut's audio output for Android: one AAudio low-latency stream per render
// client, fed from a lock-free ring, with its own worker thread for the guest's
// render callbacks. It replaces the SDK's SDL backend and the SDK's worker loop
// (which waits on every client's semaphore at once through the multi-handle
// wait) without changing the SDK: it is an rex::audio::AudioSystem, handed to
// the runtime through RuntimeConfig::audio_factory, and the XAudio kernel
// exports that talk to clients are routed here by kernel_overrides.cpp.
#include <cstdint>
#include <memory>

#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/audio.h>

namespace renut::audio {

// The runtime's audio_factory.
std::unique_ptr<rex::system::IAudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher);

// XAudioRegisterRenderDriverClient(callback_ptr, driver_ptr) and the other
// XAudio calls the title makes on the render driver, with guest addresses.
// They return the X_STATUS / X_ERROR the export returns.
uint32_t RegisterRenderClient(uint32_t callback_ptr, uint32_t driver_ptr);
uint32_t UnregisterRenderClient(uint32_t driver);
uint32_t SubmitRenderFrame(uint32_t driver, uint32_t samples_ptr);

// Output frames (5.33 ms each) the device played as silence because the title
// had not handed over its next one in time. Read by the frame report.
uint64_t SilentFrames();

}  // namespace renut::audio
