// Windows counterparts of the small platform seams the Android build provides
// in android_vulkan_loader.cpp, vulkan_entry.cpp and the thread-priority call
// in plume/gpu/host.cpp.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>


// Opened once, kept for the life of the process: the loader every Vulkan user in
// the process (Plume, and the SDK's own UI code when it is linked) shares.
void* renutXenosVulkanSymbol(const char* name) {
  static HMODULE loader = LoadLibraryA("vulkan-1.dll");
  return loader ? reinterpret_cast<void*>(GetProcAddress(loader, name)) : nullptr;
}

// Unix niceness onto Windows thread priorities: 0 is the default, 10 the most
// background.
void renutSetThreadNice(int nice) {
  int priority = THREAD_PRIORITY_NORMAL;
  if (nice >= 10) priority = THREAD_PRIORITY_LOWEST;
  else if (nice > 0) priority = THREAD_PRIORITY_BELOW_NORMAL;
  SetThreadPriority(GetCurrentThread(), priority);
}

// Installed physical memory, which low-memory profile selection reads.
uint64_t renutPhysicalMemoryBytes() {
  MEMORYSTATUSEX status{sizeof(status)};
  return GlobalMemoryStatusEx(&status) ? uint64_t(status.ullTotalPhys) : 0;
}

// MSYS2's winpthreads (and libstdc++) are built with stack protectors, and the
// SDK starts threads from static initializers (rex::thread::TimerQueue). libssp
// draws the canary value in its own constructor, which can run after such a
// thread is already inside a protected function: the value then changes under
// it and the function aborts at return with "stack smashing detected". A
// constant canary keeps libssp's constructor out of the link and the value
// stable for the life of the process.
extern "C" uintptr_t __stack_chk_guard = static_cast<uintptr_t>(0x595e9fbd94fda766ull);

// libssp's __stack_chk_fail only says "terminated". Say where, too: the return
// addresses (as offsets from the image base) go to stderr and the runtime log.
extern "C" __attribute__((noreturn)) void __stack_chk_fail() {
  void* frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  char text[48 * 24 + 64];
  int length = std::snprintf(text, sizeof(text), "stack smashing detected; frames (image offsets):");
  for (USHORT i = 0; i < count && length < int(sizeof(text)) - 24; ++i)
    length += std::snprintf(text + length, sizeof(text) - size_t(length), " %zx",
                            reinterpret_cast<uintptr_t>(frames[i]) - base);
  text[length++] = '\n';
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, DWORD(length), &written, nullptr);
  TerminateProcess(GetCurrentProcess(), 127);
  __builtin_unreachable();
}
extern "C" __attribute__((noreturn)) void __stack_chk_fail_local() { __stack_chk_fail(); }
