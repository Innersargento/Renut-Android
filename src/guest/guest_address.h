#pragma once
// Where a guest address lives in host memory, for code that reads guest memory
// through `base` as the recompiled game does (REX_PHYS_HOST_OFFSET in the
// generated code).
//
// The SDK maps the 0xE0000000 view of physical memory in place on Android, so
// base + address is the host address.
#include <cstdint>

namespace renut {

inline uint8_t* GuestPtr(uint8_t* base, uint32_t address) { return base + address; }
inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t address) { return base + address; }

}  // namespace renut
