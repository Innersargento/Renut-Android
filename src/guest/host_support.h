#pragma once
// Small services of the host process that the SDK does not provide: the
// player's pause, and the counters the renderer and the frame counter share.
// They live here, in reNut's own code, so that the SDK is used exactly as
// released.
#include <cstddef>
#include <cstdint>

namespace rex::system {
class KernelState;
}

namespace renut {

// The player's pause. While set, every guest thread parks at its next kernel
// wait, delay or yield (the overrides in kernel_overrides.cpp call
// WaitIfTitlePaused first), and a title shutdown releases it. Cooperative on
// purpose: stopping a host thread with a signal can leave it holding the
// allocator or a kernel lock that the threads still running need.
void SetTitlePaused(bool paused);
bool IsTitlePaused();
// Parks the calling guest thread while the title is paused. Host threads and
// threads of the SDK are never parked.
void WaitIfTitlePaused();

// X_KTHREAD::kernel_time (offset 0x58), the thread's kernel time in
// milliseconds, is read by guest code straight out of memory and nothing in the
// SDK writes it. Banjo-Kazooie: Nuts & Bolts drives its allocator's reclaim
// watchdog off it and, with the field stuck at zero, spins at 100% CPU for
// ever. This host thread keeps it moving, as the guest clock, on every guest
// thread. Start it once the title's threads can exist; stop it before the
// kernel state goes away.
void StartKernelTime(rex::system::KernelState* kernel);
void StopKernelTime();

// A probe on the kernel waits of one thread, to find out what it is waiting for
// (the audio worker's title callback: renut_audio.cpp). The marked thread's
// waits, delays and yields are timed by the overrides in kernel_overrides.cpp and
// grouped by what was waited on and who called.
struct WaitProbeEntry {
  uint32_t what = 0;    // kernel export index (see kernel_overrides.cpp)
  uint32_t object = 0;  // guest address or handle in r3
  uint32_t caller = 0;  // guest LR at the call
  uint64_t count = 0;
  uint64_t nanoseconds = 0;
};
void MarkWaitProbeThread();
bool IsWaitProbeThread();
void NoteProbedWait(uint32_t what, uint32_t object, uint32_t caller, uint64_t nanoseconds);
// The entries since the last call, longest total wait first (at most `limit`).
void TakeWaitProbe(WaitProbeEntry* out, size_t limit, size_t* count);

// The audio worker waits for a mixed frame on a couple of kernel objects (see
// the KeWaitForMultipleObjects override); whoever signals one of them is the
// thread that mixes. The wait registers the objects' guest addresses here, the
// KeSetEvent / KeReleaseSemaphore overrides look the signalled address up, and
// the first time a thread signals one the callback gets that thread's id.
// Returns whether the slot held another address before.
bool WatchSignalObject(uint32_t slot, uint32_t guest_address);
void SetSignalerCallback(void (*callback)(int tid));
// True and, once per thread, the callback is told, when `guest_address` is watched.
void NoteSignal(uint32_t guest_address);
// Whether any object is being watched (a cheap test before resolving a handle).
bool AnyWatched();

}  // namespace renut
