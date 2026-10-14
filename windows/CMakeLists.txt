cmake_minimum_required(VERSION 3.25)
project(renut_windows LANGUAGES C CXX)

# For this project's own targets; the SDK sets the same inside its directory.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(RENUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(REXGLUE_ROOT "${RENUT_ROOT}/thirdparty/rexglue-sdk")

# The SDK is the git submodule thirdparty/rexglue-sdk (tag v0.10.0), exactly as
# released: nothing under thirdparty/ is edited, patched or copied.
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "" FORCE)
set(REXGLUE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The SDK derives its version from `git describe` in CMAKE_SOURCE_DIR, which here
# is this repository (and its own tags). Hiding git makes it use its floor.
set(GIT_EXECUTABLE "git-not-used")

# Headers MinGW lacks and the SDK's third-party code expects.
include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/compat")
add_subdirectory("${REXGLUE_ROOT}" rexglue-sdk EXCLUDE_FROM_ALL)

# The XInput driver stores GetProcAddress results in void*, which clang accepts
# only in Microsoft-compatibility mode; a force-included wrapper does the cast.
set_source_files_properties("${REXGLUE_ROOT}/src/input/xinput/xinput_input_driver.cpp"
    TARGET_DIRECTORY rexinput PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/compat/getprocaddress_void.h"
    SKIP_PRECOMPILE_HEADERS ON)

# The SDK builds rexruntime and rexgpu-xenos as DLLs. The recompiled game
# references the kernel's exports by names that start with "__imp_", which the
# COFF linker reads as dllimport thunks, so a DLL boundary there cannot resolve
# them (and MinGW's export-all leaves those names out). Everything goes into one
# executable instead: these static copies are built from the very same sources,
# options and dependencies.
function(renut_static_copy out original)
    get_target_property(source_dir ${original} SOURCE_DIR)
    get_target_property(listed ${original} SOURCES)
    set(sources "")
    foreach(entry IN LISTS listed)
        if(IS_ABSOLUTE "${entry}" OR entry MATCHES "^\$<")
            list(APPEND sources "${entry}")
        else()
            list(APPEND sources "${source_dir}/${entry}")
        endif()
    endforeach()
    add_library(${out} STATIC ${sources})
    foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
            INTERFACE_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS)
        get_target_property(value ${original} ${property})
        if(value)
            set_property(TARGET ${out} PROPERTY ${property} ${value})
        endif()
    endforeach()
    get_target_property(libraries ${original} LINK_LIBRARIES)
    if(libraries)
        list(TRANSFORM libraries REPLACE "^rexruntime$" "renut_rexruntime")
        target_link_libraries(${out} PUBLIC ${libraries})
    endif()
    # What the SDK's root directory applies to everything under it, and a target
    # does not report back as its own property.
    target_compile_definitions(${out} PRIVATE REX_PLATFORM_WINDOWS=1 REXGLUE_BUILD_CONFIG="Release")
    target_compile_options(${out} PRIVATE -fno-strict-aliasing -ffp-model=strict -fno-char8_t -O2 -w)
    target_link_libraries(${out} PUBLIC winpthread)
endfunction()
# Plume carries its own VulkanMemoryAllocator implementation; the SDK's copy
# (only its Vulkan UI layer uses it, and that layer is not created here) would
# define every symbol twice.
get_target_property(RENUT_UI_SOURCES rexui SOURCES)
list(FILTER RENUT_UI_SOURCES EXCLUDE REGEX "vulkan_mem_alloc\.cpp$")
set_property(TARGET rexui PROPERTY SOURCES ${RENUT_UI_SOURCES})

renut_static_copy(renut_rexruntime rexruntime)
renut_static_copy(renut_rexgpu rexgpu-xenos)

# On Windows, git checks the SDK's symlinks out as small text files, so
# thirdparty/libmspack/cabextract/mspack/* is not C. The real sources are the
# link targets.
set(RENUT_MSPACK_DIR "${REXGLUE_ROOT}/thirdparty/libmspack/libmspack/mspack")
set_property(TARGET mspack PROPERTY SOURCES "${RENUT_MSPACK_DIR}/lzxd.c")
set_property(TARGET mspack PROPERTY INCLUDE_DIRECTORIES "${RENUT_MSPACK_DIR}")
set_property(TARGET mspack PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${RENUT_MSPACK_DIR}")

#=============================================================================
# Plume (renderbag/plume), the git submodule thirdparty/plume, as released.
# Its Vulkan backend is a single translation unit that also carries the volk and
# VulkanMemoryAllocator implementations.
#=============================================================================
set(PLUME_ROOT "${RENUT_ROOT}/thirdparty/plume")
if(NOT EXISTS "${PLUME_ROOT}/plume_vulkan.cpp")
    message(FATAL_ERROR "thirdparty/plume ausente: git submodule update --init --recursive")
endif()
add_library(renut_plume STATIC "${PLUME_ROOT}/plume_vulkan.cpp")
target_include_directories(renut_plume PUBLIC "${PLUME_ROOT}"
    "${PLUME_ROOT}/contrib/volk" "${PLUME_ROOT}/contrib/Vulkan-Headers/include"
    "${PLUME_ROOT}/contrib/VulkanMemoryAllocator/include")
target_compile_definitions(renut_plume PRIVATE NDEBUG NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(renut_plume PRIVATE -w)

#=============================================================================
# The recompiled game (generated/android is plain C++; nothing in it is ARM64).
#=============================================================================
# src/ is shared with the Android build (android/native/CMakeLists.txt).
set(RENUT_SRC "${RENUT_ROOT}/src")
include("${RENUT_ROOT}/generated/android/sources.cmake")
set(RENUT_COMMON_DEFINITIONS REX_PLATFORM_WINDOWS=1 REXGLUE_BUILD_CONFIG="Release" NOMINMAX WIN32_LEAN_AND_MEAN)
set(RENUT_COMMON_OPTIONS -fno-strict-aliasing -ffp-model=strict -fno-char8_t -O2)

add_library(renut_guest OBJECT ${GENERATED_SOURCES})
target_include_directories(renut_guest PRIVATE "${RENUT_ROOT}/generated/android" "${RENUT_SRC}")
target_link_libraries(renut_guest PRIVATE renut_rexruntime)
target_compile_definitions(renut_guest PRIVATE ${RENUT_COMMON_DEFINITIONS})
target_compile_options(renut_guest PRIVATE ${RENUT_COMMON_OPTIONS} -g0 -w)
target_precompile_headers(renut_guest PRIVATE "${RENUT_SRC}/guest/renut_guest_pch.h")
include("${RENUT_SRC}/guest/kernel_overrides.cmake")
renut_kernel_overrides(renut_guest)

#=============================================================================
# Offline-recompiled shaders: the cache table plus the smol-v blob.
#=============================================================================
set(RENUT_SHADERS "${RENUT_ROOT}/generated/shaders")
set_source_files_properties("${RENUT_SRC}/plume/shader_cache_blob.S" PROPERTIES
    COMPILE_OPTIONS "-DRENUT_SHADER_BLOB=\"${RENUT_SHADERS}/shader_spirv.smolv\""
    OBJECT_DEPENDS "${RENUT_SHADERS}/shader_spirv.smolv")

#=============================================================================
# renut_plume.exe
#=============================================================================
enable_language(ASM)
add_executable(renut_plume_exe
    "${RENUT_SRC}/platform/windows/main_windows.cpp" "${RENUT_SRC}/platform/windows/platform_windows.cpp"
    "${RENUT_SRC}/guest/game_fixes.cpp" "${RENUT_SRC}/guest/cheats.cpp"
    "${RENUT_SRC}/guest/kernel_overrides.cpp" "${RENUT_SRC}/guest/host_support.cpp"
    "${RENUT_SRC}/d3d/d3d_layer.cpp" "${RENUT_SRC}/d3d/native_d3d.cpp"
    "${RENUT_SRC}/d3d/plume_d3d.cpp"
    "${RENUT_SRC}/plume/plume_renderer.cpp" "${RENUT_SRC}/plume/plume_gpu.cpp"
    "${RENUT_SRC}/plume/plume_textures.cpp" "${RENUT_SRC}/plume/shader_cache_blob.S"
    "${RENUT_SHADERS}/shader_cache.cpp"
    "${RENUT_SRC}/vendor/smolv/smolv.cpp"
    $<TARGET_OBJECTS:renut_guest>)
set_target_properties(renut_plume_exe PROPERTIES OUTPUT_NAME renut_plume)
target_include_directories(renut_plume_exe PRIVATE "${RENUT_ROOT}/generated/android" "${RENUT_SHADERS}"
    "${RENUT_SRC}" "${RENUT_SRC}/plume")
target_compile_definitions(renut_plume_exe PRIVATE ${RENUT_COMMON_DEFINITIONS})
target_compile_options(renut_plume_exe PRIVATE ${RENUT_COMMON_OPTIONS} -ferror-limit=0)
target_link_libraries(renut_plume_exe PRIVATE renut_rexruntime renut_rexgpu renut_plume xxhash)
