// The title's memory and kernel as the app's host code reaches them: through the process runtime of the
// SDK (rex::kernel::TitleRuntime), from Bind (at the title's launch) until Unbind.
//
// The app runs without a graphics driver (GraphicsFamily::kNone), so what the SDK hands a driver at its
// attach is taken here from the runtime's public interfaces instead: the physical write watch (the
// address space of the title's executable module), host threads of the kernel, and the video mode.
#pragma once
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string_view>

#include <rex/guest.h>
#include <rex/result.h>
#include <rex/title.h>
#include <rex/utility.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/title_runtime.h>

namespace rex::gpu {
class IGraphicsSystem;
}

namespace renut::host {

// Set by the app when the title is loaded (after its executable is, before it runs) and cleared when it
// unloads. Bind also takes the write watch.
void Bind(rex::kernel::TitleRuntime& title);
void Unbind();
rex::kernel::TitleRuntime* Title();

// Starts a host thread of the title's kernel (a KernelThread without a guest entry point), as the SDK does for
// a graphics driver: it may call into the title (RunGuest) and the kernel's exports. The guard asks it to stop
// (the token) and waits for it; it must be destroyed before the title unloads. Fails before Bind.
rex::Result<rex::ScopeGuard> StartHostThread(std::string_view name, rex::MoveOnlyFunction<void(std::stop_token)> fn);

// The base of the guest's virtual space: a guest address is a host address at base + address, but for the
// physical views (see Physical).
uint8_t* VirtualBase();
template <typename T = uint8_t*>
T Virtual(uint32_t address) {
  return reinterpret_cast<T>(VirtualBase() + address);
}
// The host address of a physical address (0 to 0x1FFFFFFF): the 0xA0000000 view of the same pages.
template <typename T = uint8_t*>
T Physical(uint32_t address) {
  return reinterpret_cast<T>(VirtualBase() + (0xA0000000u | (address & 0x1FFFFFFFu)));
}
// A guest address in the physical views (0xA0000000 and up) as a physical address.
inline uint32_t PhysicalAddress(uint32_t virtual_address) { return virtual_address & 0x1FFFFFFF; }

// System memory the host code hands the guest (scratch lists, fence words); zeroed.
uint32_t SystemAlloc(uint32_t size, uint32_t alignment, bool physical = false);
void SystemFree(uint32_t address);

// Runs a guest function on the calling thread, which must be a kernel thread (the guest's own or one of
// StartHostThread's): the function's address and its arguments (r3, r4...), on a copy of the thread's registers.
uint64_t RunGuest(uint32_t address, const uint64_t* arguments, size_t count);

// The video mode the title runs in (width, height, refresh rate): the UI system's, which the SDK hands the
// kernel when the title launches. Set by the app before Bind.
void SetVideoMode(const rex::VideoMode& mode);
rex::VideoMode GetVideoMode();
// The same as the kernel's VdQueryVideoMode answers it, into a guest X_VIDEO_MODE (48 bytes).
void FillGuestVideoMode(uint32_t address);
// VdQueryVideoFlags: 1 widescreen, 2 at least 1024 wide, 4 at least 1920 wide.
uint32_t VideoFlags();

// The SDK's graphics system, which counts the frames (GetFrameStats) from the reports of a program that presents
// with a renderer of its own: one ReportFrame per present. Set by the app; null before.
void SetGraphics(rex::gpu::IGraphicsSystem* graphics);
void ReportFrame();

// Watches for guest writes to physical memory, from Bind until Unbind; null outside.
rex::IPhysicalWriteWatch* WriteWatch();
// Watches a range of physical memory for guest writes: the GPU backend's listener hears of the first one.
inline void WatchWrites(uint32_t address, uint32_t length) {
  if (rex::IPhysicalWriteWatch* watch = WriteWatch()) watch->WatchPhysicalWrites({address, length});
}
// Whether every page of a range of physical memory is committed and writable in one of the views the guest
// maps it through (0xA0000000, 0xC0000000, 0xE0000000). The watch skips pages that are not, and committing one
// later notifies nobody, so only a range this answers true for can be watched. Asked through the kernel's
// MmQueryAddressProtect, so only on a kernel thread (the guest's own or StartHostThread's); false elsewhere.
bool PhysicalRangeWritable(uint32_t address, uint32_t length);

}  // namespace renut::host
