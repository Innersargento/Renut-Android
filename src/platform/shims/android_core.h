// Force-included into the SDK's memory_posix.cpp and threading_posix.cpp on
// Android (CMakeLists.txt). Those files are written for glibc: they call
// rex::GetAndroidApiLevel() without declaring it (memory_posix.cpp) and use robust mutexes, which
// bionic does not have. Declared here, the first as bionic answers it and the
// robust-mutex calls as "not supported", which the SDK's code already treats as
// a plain mutex.
#pragma once
#include <errno.h>
#include <pthread.h>

// rex::GetAndroidApiLevel(), as the repository's include/rex/main_android.h
// answers it (threading_posix.cpp includes the same header itself).
#include <rex/main_android.h>

#ifndef PTHREAD_MUTEX_ROBUST
#define PTHREAD_MUTEX_ROBUST 1
static inline int pthread_mutexattr_setrobust(pthread_mutexattr_t*, int) { return ENOTSUP; }
static inline int pthread_mutex_consistent(pthread_mutex_t*) { return ENOTSUP; }
#endif

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
