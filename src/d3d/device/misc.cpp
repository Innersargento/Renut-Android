// The smaller device calls, natively (see misc.h).
#include "d3d/device/misc.h"

#include <cstring>

#include "d3d/device/internal.h"
#include "d3d/device/retail.h"

namespace renut::d3d::port {

namespace {

constexpr u32 kRing = 0x30;
constexpr u32 kVerticesRing = 0x3484;       // the ring pointer BeginVertices saved
constexpr u32 kReferenceCount = 0x3C;
constexpr u32 kCapsBytes = 0x130;

u8* Guest(const u8* base, u32 address) {
  return renut::GuestPtr(const_cast<u8*>(base), address);
}

}  // namespace

void GetDeviceCaps(uint8_t* base, u32 caps_va) {
  if (!caps_va)
    return;
  std::memcpy(Guest(base, caps_va), Guest(base, retail::kDeviceCaps), kCapsBytes);
}

void GetResourceDevice(uint8_t* base, u32 device_type, u32 out_va) {
  const u32 slot = device_type == 2 ? retail::kVdGlobalDeviceSlotType2 : retail::kVdGlobalDeviceSlot;
  const u32 variable = LoadBE32(Guest(base, slot));
  u32 device_va = variable ? LoadBE32(Guest(base, variable)) : 0;
  if (device_va) {
    u8* device = Guest(base, device_va);
    StoreBE32(device + kReferenceCount, LoadBE32(device + kReferenceCount) + 1);
  }
  if (out_va)
    StoreBE32(Guest(base, out_va), device_va);
}

void FinishVerticesUP(Device device, u32 destination, u32 data, u32 bytes) {
  if (destination && data && bytes)
    std::memcpy(Guest(device.base, destination), Guest(device.base, data), bytes);
  device.SetW(kRing, device.W(kVerticesRing));
}

}  // namespace renut::d3d::port
