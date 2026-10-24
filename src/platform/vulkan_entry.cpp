// The Vulkan entry point of the session: the loader android_vulkan_loader.cpp opened (Turnip
// or the system driver), handed to Plume's volk.
#include "plume_vulkan.h"

namespace {
PFN_vkGetInstanceProcAddr entry = nullptr;
}

// The native D3D renderer shares the loader opened for the session instead of opening a second one.
void renutSetPlumeVulkanEntry(PFN_vkGetInstanceProcAddr loader_entry) { entry = loader_entry; }

VkResult renutInitializeVulkanLoader() {
  if (!entry) return VK_ERROR_INITIALIZATION_FAILED;
  volkInitializeCustom(entry);
  return vkCreateInstance ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}
