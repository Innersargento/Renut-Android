// Force-included into the SDK's memory_posix.cpp, threading_posix.cpp, crash_posix.cpp and
// host_file_posix.cpp on Android (CMakeLists.txt). Those files are written for glibc, with Android
// branches that lean on helpers the SDK does not define, and call functions bionic only has in later
// API levels than the app's minimum. What they need is declared here, so the SDK stays as released.
#pragma once
#include <errno.h>
#include <pthread.h>
#include <rex/main_android.h>

#ifdef __cplusplus
#include <algorithm>
#include <cstring>
#include <string_view>

// threading_posix.cpp names the calling thread's object `current_thread` in a branch that only runs below
// API 26 (the app's minimum is 29) but has to compile: a stand-in that is never true.
struct RenutNoCurrentThread {
  explicit operator bool() const { return false; }
  const RenutNoCurrentThread* operator->() const { return this; }
  const RenutNoCurrentThread& GetCondition() const { return *this; }
  void SetAndroidPreAPI26Name(std::string_view) const {}
};
inline constexpr RenutNoCurrentThread current_thread{};
#endif

#ifndef PTHREAD_MUTEX_ROBUST
// Bionic has no robust mutexes: "not supported", which the SDK's code treats as a plain mutex.
#define PTHREAD_MUTEX_ROBUST 1
static inline int pthread_mutexattr_setrobust(pthread_mutexattr_t*, int) { return ENOTSUP; }
static inline int pthread_mutex_consistent(pthread_mutex_t*) { return ENOTSUP; }
#endif

// renameat2 is in bionic from API 30; the app's minimum is 29.
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
static inline int renameat2(int old_dir, const char* old_path, int new_dir, const char* new_path,
                            unsigned flags) {
  return int(syscall(__NR_renameat2, old_dir, old_path, new_dir, new_path, flags));
}

// backtrace() is in bionic from API 33. Frames come from the unwinder.
#include <unwind.h>
struct RenutBacktraceState {
  void** frames;
  int capacity;
  int count;
};
static inline _Unwind_Reason_Code renut_backtrace_step(_Unwind_Context* context, void* argument) {
  auto* state = static_cast<RenutBacktraceState*>(argument);
  if (state->count >= state->capacity) return _URC_END_OF_STACK;
  const uintptr_t pc = _Unwind_GetIP(context);
  if (pc) state->frames[state->count++] = reinterpret_cast<void*>(pc);
  return _URC_NO_REASON;
}
static inline int backtrace(void** frames, int capacity) {
  RenutBacktraceState state{frames, capacity, 0};
  _Unwind_Backtrace(renut_backtrace_step, &state);
  return state.count;
}

// A kernel older than 4.17 does not know MAP_FIXED_NOREPLACE and takes the address as a hint: over an existing
// mapping it maps somewhere else and succeeds instead of failing with EEXIST. memory_posix.cpp's AllocFixed
// then returned a range that kept its old protection (a commit onto a reserved stack left it PROT_NONE, and the
// write that filled it faulted for ever - Android 13 on a 4.14 kernel, Unisoc T618). Its mmap call is this one,
// which treats a result away from the address asked for as the EEXIST it is; AllocFixed already handles that.
#include <sys/mman.h>
#ifdef MAP_FIXED_NOREPLACE
static inline void* renut_mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset) {
  void* result = mmap(address, length, prot, flags, fd, offset);
  if ((flags & MAP_FIXED_NOREPLACE) && address && result != MAP_FAILED && result != address) {
    munmap(result, length);
    errno = EEXIST;
    return MAP_FAILED;
  }
  return result;
}
#define mmap renut_mmap
#endif
