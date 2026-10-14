// Force-included into every C++ file of the Android build (CMakeLists.txt).
//
// The SDK's chrono.h specialises std::chrono::clock_time_conversion and the
// rest of the SDK calls clock_cast. libc++ declares both only together with the
// time-zone database, which the NDK's libc++ does not ship, and chrono.h itself
// provides the same two declarations for Apple's libc++. Declaring them first,
// here, keeps the SDK exactly as released.
#pragma once
#if defined(__cplusplus) && __cplusplus >= 202002L && defined(__ANDROID__)
#include <chrono>

namespace std::chrono {
template <class, class>
struct clock_time_conversion {};

template <class DestClock, class SourceClock, class Duration>
auto clock_cast(const std::chrono::time_point<SourceClock, Duration>& t)
    -> decltype(clock_time_conversion<DestClock, SourceClock>{}(t)) {
  return clock_time_conversion<DestClock, SourceClock>{}(t);
}
}  // namespace std::chrono
#endif
