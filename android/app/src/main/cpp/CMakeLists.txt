cmake_minimum_required(VERSION 3.22)
project(renut_android LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(RENUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
# Plume (renderbag/plume) is the git submodule thirdparty/plume, as released.
set(PLUME_ROOT "${RENUT_ROOT}/thirdparty/plume")
if(NOT EXISTS "${PLUME_ROOT}/plume_vulkan.cpp")
    message(FATAL_ERROR "thirdparty/plume ausente: git submodule update --init --recursive")
endif()
add_library(renut_plume STATIC "${PLUME_ROOT}/plume_vulkan.cpp")
set_target_properties(renut_plume PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(renut_plume PUBLIC "${PLUME_ROOT}"
    "${PLUME_ROOT}/contrib/volk" "${PLUME_ROOT}/contrib/Vulkan-Headers/include"
    "${PLUME_ROOT}/contrib/VulkanMemoryAllocator/include")
# volk, inside this translation unit, opens "libvulkan.so". The session may use
# its own driver (Turnip through libadrenotools), so that one call goes to
# renut_vulkan_dlopen (android_vulkan_loader.cpp), which answers with the
# session's loader and falls back to dlopen. The file itself is not edited.
target_compile_definitions(renut_plume PRIVATE NDEBUG dlopen=renut_vulkan_dlopen)
add_subdirectory("${RENUT_ROOT}/external/libadrenotools" adrenotools)
add_library(renut_probe SHARED probe.cpp)
target_link_libraries(renut_probe PRIVATE renut_plume adrenotools android log dl)
target_link_options(renut_probe PRIVATE "-Wl,-z,max-page-size=16384")

add_subdirectory("${RENUT_ROOT}/android/native" native)
