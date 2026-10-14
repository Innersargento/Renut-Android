// Force-included into the SDK's XInput driver only. It keeps GetProcAddress's
// result in void* members, which clang accepts for a Microsoft-targeted build
// (-fms-compatibility) but rejects with a GNU driver, where that mode also
// breaks libstdc++.
#pragma once
#include <windows.h>

static inline void* renut_GetProcAddress(HMODULE module, LPCSTR name) {
  return reinterpret_cast<void*>(GetProcAddress(module, name));
}
#define GetProcAddress renut_GetProcAddress
