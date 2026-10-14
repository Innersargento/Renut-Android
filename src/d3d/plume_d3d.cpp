// Direct3D handlers for the native Plume renderer.
//
// Every call still flows to the game's own Direct3D, which records the PM4 the
// Plume command processor (plume_gpu.cpp) executes. These only mirror the
// clear colour, which the renderer shows while no frame has been resolved.
#include "d3d_layer.h"
#include "plume/plume_renderer.h"
#include "guest/guest_address.h"
#include "guest/host_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>

namespace renut::d3d {
namespace {

// Xbox 360 D3DCLEAR_ZBUFFER / D3DCLEAR_STENCIL; any other bit clears colour.
constexpr uint32_t kClearDepthStencil = 0x2 | 0x4;

// Guest memory is big-endian. On Android the recompiled REX_LOAD_* macros add
// no physical-window offset, so the address is the mapping offset. Volatile
// because the fence below is written by the thread running the GPU.
uint32_t GuestU32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(GuestPtr(base, address)));
}

uint8_t GuestU8(const uint8_t* base, uint32_t address) {
  return *reinterpret_cast<const volatile uint8_t*>(GuestPtr(base, address));
}

// How long the fence wait sleeps once it is clear the GPU is not moving. Read
// from <files>/plume-spin so a session can A/B it without a rebuild; 0 restores
// the busy wait. The wait it covers is ~100 ms, so this costs at most a quarter
// of a percent of it in latency.
constexpr uint32_t kDefaultSleepMicroseconds = 250;
constexpr uint32_t kMaxSleepMicroseconds = 20000;
// Passes without GPU progress before the first sleep, at roughly 90 million
// polls a second: a few microseconds of spinning, so a wait short enough to end
// inside it never pays for a sleep.
constexpr uint32_t kSpinBeforeSleep = 256;

std::atomic<bool> in_background{false};
constexpr uint32_t kBackgroundSleepMicroseconds = 20000;
std::mutex files_mutex;
std::string files_directory;
std::atomic<uint32_t> blocker_sleep_us{kDefaultSleepMicroseconds};
std::atomic<uint64_t> blocker_calls{0};
std::atomic<int64_t> next_reread_ns{0};

// Neither the clock nor the file may be touched on every call, so a counter
// gates the clock and the clock gates the file.
uint32_t BlockerSleepMicroseconds() {
  if ((blocker_calls.fetch_add(1, std::memory_order_relaxed) & 0xFFFF) == 0) {
    const int64_t now =
        std::chrono::steady_clock::now().time_since_epoch() / std::chrono::nanoseconds(1);
    int64_t due = next_reread_ns.load(std::memory_order_relaxed);
    if (now >= due &&
        next_reread_ns.compare_exchange_strong(due, now + 2'000'000'000, std::memory_order_relaxed)) {
      uint32_t value = kDefaultSleepMicroseconds;
      std::string path;
      {
        std::lock_guard lock(files_mutex);
        if (!files_directory.empty()) path = files_directory + "/plume-spin";
      }
      if (!path.empty()) {
        if (FILE* file = std::fopen(path.c_str(), "r")) {
          if (std::fscanf(file, "%u", &value) != 1) value = kDefaultSleepMicroseconds;
          std::fclose(file);
        }
      }
      value = std::min(value, kMaxSleepMicroseconds);
      if (value != blocker_sleep_us.exchange(value, std::memory_order_relaxed)) {
        REXLOG_INFO("Renut: Plume fence wait sleeps {} us", value);
      }
    }
  }
  return blocker_sleep_us.load(std::memory_order_relaxed);
}

float LoadBEFloat(const uint8_t* p) {
  uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// D3DDevice_Clear(device, count, rects, flags, D3DCOLOR color, z, stencil)
bool Clear(PPCContext& ctx, uint8_t*) {
  if (auto* renderer = PlumeRenderer::Get(); renderer && (ctx.r6.u32 & ~kClearDepthStencil)) {
    uint32_t argb = ctx.r7.u32;
    renderer->SetClearColor(((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f,
                            (argb & 0xFF) / 255.f, (argb >> 24) / 255.f);
  }
  return false;
}

// D3DDevice_ClearF(device, flags, rect, const D3DVECTOR4* color, z, stencil)
bool ClearF(PPCContext& ctx, uint8_t* base) {
  if (auto* renderer = PlumeRenderer::Get();
      renderer && (ctx.r4.u32 & ~kClearDepthStencil) && ctx.r6.u32) {
    const uint8_t* color = GuestPtr(base, ctx.r6.u32);
    renderer->SetClearColor(LoadBEFloat(color), LoadBEFloat(color + 4), LoadBEFloat(color + 8),
                            LoadBEFloat(color + 12));
  }
  return false;
}

// D3D_CBlocker_Check(blocker), the game's fence watchdog. It returns 1 while the
// GPU has not caught up, and appMainDraw calls it in a loop with nothing in
// between. On the console each pass burns a few hundred cycles in db16cyc
// instructions, which the recompiler drops as comments, so here the loop is a
// bare busy wait: measured 2026-09-20 it was 88% of the cycles of a Cortex-X3
// held at 96% for the ~100 ms a frame spends waiting, at ~13.5 million calls
// per frame.
//
// Yield the CPU while the GPU is not moving. The progress test is the original's
// own: the device's fence value lives at [[device + 10896]] and the blocker
// caches the last one it saw at +8, and the original writes that cache itself.
// The call still runs - this only decides whether to sleep before it - so the
// fence bookkeeping and the hang timeout keep their behaviour.
bool BlockerCheck(PPCContext& ctx, uint8_t* base) {
  // One game thread drives this loop; thread_local keeps a second one honest.
  static thread_local uint32_t idle_checks = 0;
  // The player's pause: this loop calls no kernel wait, so it parks here.
  renut::WaitIfTitlePaused();
  const uint32_t sleep_us = in_background.load(std::memory_order_relaxed)
      ? kBackgroundSleepMicroseconds : BlockerSleepMicroseconds();
  if (!sleep_us) {
    idle_checks = 0;
    return false;
  }
  const uint32_t blocker = ctx.r3.u32;
  if (!blocker) return false;
  const uint32_t device = GuestU32(base, blocker);
  if (!device) return false;
  // Bit 1 of +10941 marks the device hung or done: the original returns 0 at
  // once and the loop ends, so there is nothing to wait for.
  if (GuestU8(base, device + 10941) & 2) {
    idle_checks = 0;
    return false;
  }
  const uint32_t fence_address = GuestU32(base, device + 10896);
  if (!fence_address) return false;
  if (GuestU32(base, fence_address) != GuestU32(base, blocker + 8)) {
    idle_checks = 0;  // The GPU moved; the original records the new value.
    return false;
  }
  // A short wait still resolves at spin speed - kSpinBeforeSleep passes cost a
  // few microseconds - so only a wait that is actually long starts sleeping.
  if (++idle_checks > kSpinBeforeSleep) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
  return false;
}

}  // namespace

void SetBackground(bool background) { in_background.store(background, std::memory_order_relaxed); }

void SetRuntimeFilesDirectory(std::string directory) {
  std::lock_guard lock(files_mutex);
  files_directory = std::move(directory);
}

void RegisterPlumeHandlers() {
  SetHandler(kD3DDevice_Clear, Clear);
  SetHandler(kD3DDevice_ClearF, ClearF);
  SetHandler(kD3D_CBlocker_Check, BlockerCheck);
}

}  // namespace renut::d3d
