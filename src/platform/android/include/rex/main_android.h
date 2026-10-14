#pragma once
#include <android/api-level.h>
namespace rex {
inline int GetAndroidApiLevel() { return android_get_device_api_level(); }
}
