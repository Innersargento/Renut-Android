// Force-included into the SDK's fiber_posix.cpp on Android (CMakeLists.txt).
//
// Bionic has no getcontext/makecontext/swapcontext. libucontext provides them
// with the same layout as bionic's ucontext_t (runtime_jni.cpp checks the
// offsets its AArch64 assembly relies on), under its own names; the SDK's file
// is used as released and these names are what it calls.
#pragma once
#include <libucontext/libucontext.h>

#define getcontext libucontext_getcontext
#define makecontext libucontext_makecontext
#define swapcontext libucontext_swapcontext
