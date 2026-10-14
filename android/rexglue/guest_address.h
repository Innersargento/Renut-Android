#pragma once
// Where a guest address lives in host memory, for code that reads guest memory
// through `base` as the recompiled game does (REX_PHYS_HOST_OFFSET in the
// generated code).
//
// On Windows the SDK maps the 0xE0000000 view of physical memory 0x1000 bytes
// above where base + address would put it; on Android it is mapped in place.
// The Direct3D device, among other guest structures, lives in that view, so
// native code that bypasses REX_LOAD_* must apply the same offset.
#include <cstdint>

namespace renut {

constexpr uint32_t GuestHostOffset(uint32_t address) {
#if defined(_WIN32)
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}

inline uint8_t* GuestPtr(uint8_t* base, uint32_t address) {
  return base + address + GuestHostOffset(address);
}
inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t address) {
  return base + address + GuestHostOffset(address);
}

}  // namespace renut
