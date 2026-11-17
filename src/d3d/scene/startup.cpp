// The device's start, natively (see startup.h).
//
// Offsets are this release's, read off the generated code of the retail executable (not through Layout()).
#include "d3d/scene/startup.h"

#include <bit>
#include <cstring>

#include "d3d/device/device.h"
#include "d3d/device/guest_frame.h"
#include "d3d/device/retail.h"
#include "d3d/hooks/layer.h"
#include "platform/guest.h"
#include "platform/log.h"
#include "plume/plume_gpu.h"

#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

extern "C" {
// Hooked entry points: the native front end and the other ports run them.
REX_FUNC(sub_82657508);                  // D3DDevice_SetRingBufferParameters
REX_FUNC(sub_82663208);                  // D3D::ResetDeviceRegisters
REX_FUNC(sub_826597C8);                  // D3D::InitializeApiState
REX_FUNC(sub_82210AB0);                  // CDevice::KickOff
REX_FUNC(sub_82289F08);                  // CDevice::CaptureRing
REX_FUNC(sub_82287D38);                  // D3D::ResetAllState
REX_FUNC(Direct3DDevice_CreateTexture);  // D3DDevice_CreateTexture
REX_FUNC(sub_82655FE8);                  // D3DDevice_CreateSurface
REX_FUNC(sub_8227AB68);                  // D3DDevice_SetRenderTarget
REX_FUNC(sub_8227AF40);                  // D3DDevice_SetDepthStencilSurface
REX_FUNC(sub_8265F688);                  // D3DDevice_SetScalerParameters
REX_FUNC(sub_8264D948);                  // D3DDevice_SetGammaRamp
REX_FUNC(sub_8264DA18);                  // D3DDevice_SetPWLGamma
// The game's own, called as the original calls them.
void sub_8264DDA8(PPCContext& __restrict, uint8_t*);  // the device's command-buffer pools
void sub_82663088(PPCContext& __restrict, uint8_t*);  // the device's worker thread
void sub_8264E3D8(PPCContext& __restrict, uint8_t*);  // the device's constant pools
void sub_822399D8(PPCContext& __restrict, uint8_t*);  // XMemFree
void sub_82224858(PPCContext& __restrict, uint8_t*);  // the performance counter's frequency
void sub_8265D7D0(PPCContext& __restrict, uint8_t*);  // the device's timing state
// Kernel exports that are not the video code's.
void __imp__RtlInitializeCriticalSection(PPCContext& __restrict, uint8_t*);
void __imp__KeGetCurrentProcessType(PPCContext& __restrict, uint8_t*);
void __imp__ExRegisterTitleTerminateNotification(PPCContext& __restrict, uint8_t*);
void __imp__ExGetXConfigSetting(PPCContext& __restrict, uint8_t*);
}

namespace renut::d3d::port {
namespace {

using Frame = GuestFrame;

// Guest data the start reads: the title terminate registrations of Direct3D, the pointer to the debug monitor's
// state, the float the refresh rate is rounded with (0.5), and the one the timer frequency is scaled by.
constexpr u32 kTerminateRegistrations[] = {0x82E50688, 0x82E50698};
constexpr u32 kDebugMonitorPointer = 0x8200095C;
constexpr u32 kRefreshRounding = 0x82000C74;
constexpr u32 kTimerScale = 0x82000D0C;
// Set to 1 when the memory link failed to train (VdIsHSIOTrainingSucceeded); it trains on a host, so it stays 0.
// (0x82F9D20C, not written here.)

float GetFloat(const Frame& f, u32 address) { return std::bit_cast<float>(f.Get(address)); }
void PutFloat(const Frame& f, u32 address, float value) { f.Put(address, std::bit_cast<u32>(value)); }

bool TitleProcess(const Frame& f) { return f.Call(__imp__KeGetCurrentProcessType) != 2; }

// D3D::InitializePresentationParameters(device, parameters) (0x82659AA8): the video mode, the front buffer, the
// back buffer and depth surface the parameters ask for, the scaler, and a linear gamma. Returns 0 when a surface
// cannot be made.
u32 InitializePresentationParameters(const Frame& caller, u32 dev, u32 params) {
  Frame f = caller;
  f.sp = caller.sp - 0x6E0;  // its own frame, below the caller's
  Device d = DeviceAt(f.base, dev);
  u32 format = f.Get(params + 64);                     // r30
  const u32 width = f.Get(params + 0), height = f.Get(params + 4);  // r28, r27
  const u32 back_format = f.Get(params + 8), multisample = f.Get(params + 16);  // r26, r25
  const u32 depth_format = f.Get(params + 40);         // r22
  // The front buffer's format: none, or the two 32-bit ones in their swapped forms, become the forms
  // the display takes.
  if (format == 0 || format == 0x28280186u)
    format = 0x28280106u;
  else if (format == 0x18280186u)
    format = 0x18280106u;
  else if (format == 0x282801B6u)
    format = 0x28280136u;
  else if (format == 0x182801B6u)
    format = 0x18280136u;
  const u32 texture_format = format & 0x3F;  // r23
  u32 gamma_format = texture_format;         // r24
  if (gamma_format == 50)
    gamma_format = 6;
  else if (gamma_format == 7)
    gamma_format = 54;

  // VdQueryVideoMode: the host's.
  const u32 mode = f.sp + 80;
  renut::host::FillGuestVideoMode(mode);
  const float refresh = float(GetFloat(f, mode + 20) + GetFloat(f, kRefreshRounding));
  d.SetW(21544, f.Get(mode + 0));
  d.SetW(21552, f.Get(mode + 0));
  d.SetW(21556, u32(int64_t(refresh)));
  d.SetW(21548, f.Get(mode + 4));

  if (!f.Get(params + 60)) {
    const u32 front = f.Call(Direct3DDevice_CreateTexture, {width, height, 1, 1, 0, format, 0, 3});
    if (!front) return 0;
    d.SetW(14828, front);
  }
  if (!f.Get(params + 56)) {
    const u32 back = f.Call(sub_82655FE8, {width, height, back_format, multisample, 0});
    if (!back) return 0;
    d.SetW(14832, back);
    f.Call(sub_8227AB68, {dev, 0, back});
  }
  if (f.Get(params + 36)) {
    const u32 depth = f.Call(sub_82655FE8, {width, height, depth_format, multisample, 0});
    if (!depth) return 0;
    d.SetW(14824, depth);
    f.Call(sub_8227AF40, {dev, depth});
  }
  std::memcpy(f.G(dev + 13544), f.G(params), 124);
  if ((format & 0x3FFC0000u) != 0x24900000u) d.SetW(13608, texture_format | 0x28280100u);
  f.Call(sub_8265F688, {dev, params + 96});

  // A linear ramp, three channels of 512 bytes from +128.
  const u32 table = f.sp + 128;
  if (gamma_format != 7 && gamma_format != 54) {
    for (u32 i = 0; i < 256; ++i) {
      const u16 value = u16(((i * 1023) / 255 << 6) & 0xFFC0);
      for (u32 channel = 0; channel < 3; ++channel) StoreBE16(f.G(table + channel * 512 + i * 2), value);
    }
    f.Call(sub_8264D948, {dev, 0, table});
  } else {
    // Piecewise linear: 128 (base, delta) pairs per channel.
    for (u32 i = 0; i < 128; ++i) {
      const u32 at = i * 65535, next = at + 65535;
      const u16 base_value = u16((at / 127) & 0xFFFF);
      const u16 delta = u16((next / 127 - base_value) & 0xFFFF);
      for (u32 channel = 0; channel < 3; ++channel) {
        StoreBE16(f.G(table + channel * 512 + i * 4), base_value);
        StoreBE16(f.G(table + channel * 512 + i * 4 + 2), delta);
      }
    }
    f.Call(sub_8264DA18, {dev, 0, table});
  }
  d.SetW(16708, 0xFFFFFFFFu);
  return 1;
}

// D3D::InitializeHardwareDevice(device, parameters) (0x82659F30): 1 when the device started, 0 when not.
bool InitializeHardwareDevice(PPCContext& ctx, uint8_t* base) {
  Frame f(ctx, base, 0x80);
  const u32 dev = ctx.r3.u32, params = ctx.r4.u32;
  Device d = DeviceAt(base, dev);
  if (!d) return false;
  ctx.r3.u64 = 0;
  // In place of the GPU's engines: the backend that runs what the device records.
  if (!renut::PlumeGpuStart()) {
    REXLOG_CAT_ERROR(rex::log::renut, "Renut: the game's Direct3D device was refused: the GPU backend did not start");
    return true;
  }
  f.Call(__imp__RtlInitializeCriticalSection, {dev + 14944});
  f.Call(__imp__RtlInitializeCriticalSection, {dev + 14972});
  d.SetB(10942, d.B(10942) | 4);
  if (TitleProcess(f)) {
    for (const u32 registration : kTerminateRegistrations)
      f.Call(__imp__ExRegisterTitleTerminateNotification, {registration, 1});
  }
  d.SetW(16712, 0);
  // The kernel's VdGlobalDevice, which the resource code reads.
  const u32 slot = f.Get(TitleProcess(f) ? retail::kVdGlobalDeviceSlot : retail::kVdGlobalDeviceSlotType2);
  f.Put(slot, dev);
  StoreBE16(f.G(f.sp + 80), 0);
  if (int32_t(f.Call(__imp__ExGetXConfigSetting, {3, 10, dev + 16716, 4, f.sp + 80})) < 0) return true;
  if (f.Call(sub_82657508, {dev, params + 72}) != 0) return true;
  if (!f.Call(sub_8264DDA8, {dev})) return true;
  if (!f.Call(sub_82663088, {dev})) return true;
  if (!f.Call(sub_8264E3D8, {dev})) return true;
  d.SetW(21560, 0xFFFFFFFFu);
  d.SetW(21564, 0xFFFFFFFFu);
  f.Call(sub_82663208, {dev});
  if (!InitializePresentationParameters(f, dev, params)) return true;
  f.Call(sub_826597C8, {dev});
  f.Call(sub_822399D8, {0, 0xB1800000u});

  // The debug monitor's state, when one is attached, is pointed at the device's counters.
  u32 monitor = 0;
  if (const u32 state = f.Get(f.Get(kDebugMonitorPointer))) monitor = f.Get(state + 32);
  if (monitor) {
    if (f.Get(monitor) > 8) {
      f.Put(monitor + 4, dev + 16720);
      f.Put(monitor + 8, dev + 16724);
    }
    f.Put(monitor + 24, dev + 16560);
    f.Put(monitor + 28, dev + 21576);
  }

  f.Call(sub_82210AB0, {dev});
  f.Call(sub_82224858, {f.sp + 88});
  const float frequency = float(double(int64_t(LoadBE64(f.G(f.sp + 88)))));
  PutFloat(f, dev + 21588, frequency);
  PutFloat(f, dev + 21592, float(double(GetFloat(f, kTimerScale)) / double(frequency)));
  f.Call(sub_8265D7D0, {dev});
  f.Call(sub_82289F08, {dev, 1, 1});
  f.Call(sub_82287D38, {dev, 0});
  if (TitleProcess(f)) {
    for (u32 i = 0; i < 41; ++i) d.SetW(23380 + 4 * i, 0xFFFFFFFFu);
  }
  ctx.r3.u64 = 1;
  REXLOG_CAT_INFO(rex::log::renut, "Renut: Direct3D device {:08X} started natively ({}x{}), without the kernel's video code", dev,
                  d.W(21544), d.W(21548));
  return true;
}

}  // namespace

void RegisterStartup() { SetHandler(kD3D_InitializeHardwareDevice, InitializeHardwareDevice); }

}  // namespace renut::d3d::port
