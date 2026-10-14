#pragma once
// Small services of the host process that the SDK does not provide: the
// player's pause, and the counters the renderer and the frame counter share.
// They live here, in reNut's own code, so that the SDK is used exactly as
// released.
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

}  // namespace renut
