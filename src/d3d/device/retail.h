#pragma once
// Guest addresses of this app's executable (the European retail default.xex, SHA-256
// d57b15b7...38d3bc) that reNut's code names for its own (title update 3) build.
// reNut's constants are wrong here; these were read from the executable's image
// (tools/xex_basefile.py) on 2026-10-30. Every reNut address in a port must come
// through this file, or be checked the same way, before it is used.
#include <cstdint>

namespace renut::d3d::port::retail {

// N&B's state tables, {getter, setter, default} per state (12 bytes each): 101 render
// states and 20 sampler states. Entry 10 of the render table is {0x8264C178,
// 0x8264C140 (ZEnable), 1}; entry 0 of the sampler table is {0x8264D600, 0x8227D2D0
// (AddressU), 0}. reNut's 0x82D8A948 / 0x82D8AE28 hold unrelated data here.
constexpr uint32_t kRenderStateTable = 0x82E506F0;
constexpr uint32_t kSamplerStateTable = 0x82E50BD0;

// The GPU format table: per D3D format (low 6 bits) a 16-bit entry, bits per pixel in
// the low byte and flags in the high one. reNut's 0x82005200 holds pointers here.
constexpr uint32_t kFormatTable = 0x820052F8;

// Functions the port may call as the game does, by their addresses here.
constexpr uint32_t kXMemFree = 0x822399D8;          // frees a resource's memory
constexpr uint32_t kXMemAlloc = 0x82265B90;         // allocates it
constexpr uint32_t kXPhysicalAlloc = 0x82716060;
constexpr uint32_t kInitializeApiState = 0x826597C8;
constexpr uint32_t kInitializePresentationParameters = 0x82659AA8;
constexpr uint32_t kInitializeHardwareDevice = 0x82659F30;
constexpr uint32_t kDirect3DCreateDevice = 0x8264DF28;
constexpr uint32_t kDeviceRelease = 0x8264E080;

// The kernel's VdGlobalDevice variable, through import slots the game reads (seen in
// D3DResource_Destroy, 0x822084F0, idalib): the slot at 0x82000800 when the function at
// 0x82D0827C returns 2, else the one at 0x820007FC. reNut's 0x820007F8 is not either.
constexpr uint32_t kVdGlobalDeviceSlotType2 = 0x82000800;
constexpr uint32_t kVdGlobalDeviceSlot = 0x820007FC;
constexpr uint32_t kDeviceTypeFunction = 0x82D0827C;

// N&B's constant D3DCAPS9 (0x130 bytes), which D3DDevice_GetDeviceCaps copies out.
constexpr uint32_t kDeviceCaps = 0x82183B00;

// A word D3D tests in KickOff (0x82210AB0): while it is set, a device's first kick
// waits for its fence and the device's fences then complete as they are written.
constexpr uint32_t kKickOffWaitsGlobal = 0x82FAD20C;
// The 8 bytes Direct3D_CreateDevice copies into every device (+0x2A80).
constexpr uint32_t kDeviceGlobalQword = 0x82F39B38;

// NOT verified for this executable, so not to be used as they are in reNut's code:
// the HiZ owner word (reNut 0x82E108A0), the global qword copied into every device
// (0x82DA3478), XPhysicalAlloc's no-large-pages flag (0x82DA3778), the constant
// D3DCAPS9 (0x82161B30, which is text here) and the Fusion flag (0x82DC67D4).

}  // namespace renut::d3d::port::retail
