// threading_posix.cpp includes this header on Android; the SDK ships none, so it is the app's. It
// carries what the Android branches of memory_posix.cpp and threading_posix.cpp call without
// defining.
#pragma once
#include <android/api-level.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace rex {
inline int GetAndroidAPILevel() { return android_get_device_api_level(); }
inline void copy_truncating(char* destination, std::string_view source, size_t capacity) {
  if (!capacity) return;
  const size_t count = std::min(source.size(), capacity - 1);
  std::memcpy(destination, source.data(), count);
  destination[count] = '\0';
}
}  // namespace rex
