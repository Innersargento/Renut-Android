// The threads that make the game's sound, kept on time.
//
// RexGlue 0.11 runs every guest thread at the host's normal priority (runtime.ignore_thread_priorities is on by
// default) and the app's own audio system of 0.10 (renut_audio.cpp, which raised them) is gone. The title's mixer
// costs a large share of a core for every 5.33 ms frame it makes; at normal priority, next to the pipeline
// compiles and the GPU backend, it fell behind the device, the SDK's queue of mixed frames ran dry and the device
// played the gaps as silence: the crackling after the bump.
//
// So the threads that the next frame waits on are raised to nice -10 (Android's own audio threads run at -16 and
// -19) and kept on the fast cores, as before the bump: the game's mixer threads (leader sub_82664508 and workers
// sub_826645D0, hooked at their entry), the SDK's audio worker (which runs the title's render callback) and its
// XMA decoder. And the underruns of the SDK's audio system are logged, ten seconds at a time, when there are any.
#pragma once

namespace rex::apu {
class IAudioSystem;
}

namespace renut::audio {

// The SDK's audio system, whose underruns are logged. Set by the app at the title's launch, null after.
void SetSystem(rex::apu::IAudioSystem* system);

// From the title's render callback, on the SDK's audio worker: raises the worker and the XMA decoder the first
// time, and keeps the underrun log.
void OnRenderCallback();

}  // namespace renut::audio
