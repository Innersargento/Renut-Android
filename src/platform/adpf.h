// Android Dynamic Performance Framework (ADPF): a performance hint session for the threads
// that carry the game, fed with how long each frame's work really took against the time
// it has (the frame period). The system then picks the lowest CPU clocks that keep up, so
// the phone neither idles the game below its need nor over-boosts and heats into the
// vendor's thermal caps; the thermal headroom is read alongside to tell which one it is.
#pragma once

#include <cstdint>

namespace renut::adpf {

// Looks the NDK entry points up (API 33 for hint sessions, 30 for thermal) and starts the
// maintenance thread. A device without them leaves everything off. The target is the
// frame period the game is held to: the player's frame-rate target (plume/frame_pacing.h).
void Start();
// The player moved the frame-rate target while playing: the hint session is told the
// new period (0 is unlimited).
void SetTargetFps(int fps);
// Closes the session and stops the thread. Before the runtime is torn down.
void Stop();
// The player's pause (and the app going to the background): no frames come, so no session
// may keep its boost; it is made again at the first frame after.
void SetActive(bool active);

// One frame of the guest has been presented. Called by the kernel at every swap.
void OnFrame();

}  // namespace renut::adpf
