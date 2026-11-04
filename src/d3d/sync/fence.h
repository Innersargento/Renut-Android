#pragma once
// Layer 3 (part): fence queries and waits of the native Direct3D port (see
// device.h). The ring, its segments and KickOff stay the recompiled Direct3D's:
// the native front end (d3d/frontend) publishes the lists from their hooks, and the
// SDK still carries the vertical blank and callbacks.
#include "d3d/device/device.h"

namespace renut::d3d::port {

// What the wait needs from outside the port.
struct FenceHooks {
  void (*kick)(void* user, u32 device_va) = nullptr;  // CDevice::KickOff: issue the current fence
  bool (*executor_idle)() = nullptr;                  // every published list has run
  void (*pause)() = nullptr;                          // the player's pause
  // Sleep until a fence completes instead of polling: epoch() is read before the fence is
  // tested, wait(epoch, timeout_us) returns when it moves (or on the timeout).
  u32 (*epoch)() = nullptr;
  void (*wait)(u32 epoch, u32 timeout_us) = nullptr;
  void* user = nullptr;
};

// IsFencePending's test: the fence has been issued and the GPU has not reached it.
bool FencePending(Device device, u32 fence);

// D3DDevice_IsFencePending, D3DDevice_BlockOnFence and CDevice::BlockOnFence are handlers in
// hooks/handlers.cpp; IsFencePending kicks the current fence first.

// CDevice::BlockOnFence(Fence, Type, NoKick): kick the fence if no KickOff has
// covered it yet, then wait for it.
void BlockOnFence(Device device, u32 fence, bool no_kick, const FenceHooks& hooks);

}  // namespace renut::d3d::port
