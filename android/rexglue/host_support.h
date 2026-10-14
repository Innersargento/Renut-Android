#pragma once
// Small services of the host process that the SDK does not provide: the
// player's pause, and the counters the renderer and the frame counter share.
// They live here, in reNut's own code, so that the SDK is used exactly as
// released.
#include <cstdint>

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

}  // namespace renut
