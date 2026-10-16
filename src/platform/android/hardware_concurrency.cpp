// std::thread::hardware_concurrency() on Android is sysconf(_SC_NPROCESSORS_ONLN), and bionic
// answers that by opening and parsing /sys/devices/system/cpu/online on every call.
//
// The SDK's timer queue (the guest's waitable timers) waits for its next timer with
// disruptorplus' spin_wait_strategy, and disruptorplus asks for the processor count each time
// it resets a spin_wait - which the timed wait does on every round, about a thousand times a
// second. A profile of Showdown Town (15/10) had that one thread at 2.4% of all the game's CPU
// cycles, 43% of it inside hardware_concurrency (fopen, getdelim, openat, close) and another
// quarter in sched_yield; the same thread on the PC, where the count is a cached call, takes
// 0.2% of a core.
//
// librexruntime reaches hardware_concurrency through its PLT, and this library is the root of
// the load group, so a definition here answers for it (the same way as query_protect.cpp). The
// count is read once: the cores of a phone are not unplugged while a game runs.
#include <unistd.h>

#include <thread>

_LIBCPP_BEGIN_NAMESPACE_STD

unsigned thread::hardware_concurrency() _NOEXCEPT {
  static const unsigned count = [] {
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? static_cast<unsigned>(online) : 1u;
  }();
  return count;
}

_LIBCPP_END_NAMESPACE_STD
