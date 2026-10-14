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
