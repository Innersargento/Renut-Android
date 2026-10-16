cmake_minimum_required(VERSION 3.22)
project(renut_rexglue_android LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(RENUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# The code reNut itself writes: src/, shared with the Windows build (windows/CMakeLists.txt).
set(RENUT_SRC "${RENUT_ROOT}/src")
set(RENUT_ANDROID "${RENUT_SRC}/platform/android")
# The SDK is the git submodule thirdparty/rexglue-sdk (tag v0.10.0) exactly as
# released: nothing under thirdparty/ is edited, patched or copied. What Android
# needs on top of it is under src/ (platform/android/shims/, guest/kernel_overrides.*,
# the audio output) or arrives as compile options below.
set(REXGLUE_ROOT "${RENUT_ROOT}/thirdparty/rexglue-sdk")
set(PROJECT_SOURCE_DIR "${REXGLUE_ROOT}")
set(REXGLUE_USE_D3D12 OFF)
set(REXGLUE_USE_VULKAN ON)
set(REXGLUE_ENABLE_TRACY OFF)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF)
set(CMAKE_INSTALL_INCLUDEDIR include)
add_compile_definitions(REX_PLATFORM_LINUX=1 REXGLUE_BUILD_CONFIG="Release")
add_compile_options(-fno-strict-aliasing -ffp-model=strict -fno-char8_t)
# assembleDebug leaves CMAKE_BUILD_TYPE=Debug, so clang defaulted to -O0 for
# every target except the SDK's GPU plugin: the kernel, runtime and shader compiler all
# ran unoptimized even though REXGLUE_BUILD_CONFIG says "Release". Optimize the
# whole tree. NDEBUG is how the SDK is released: its asserts are checks of
# the console's own invariants that the title legitimately breaks (a graphics
# interrupt finding a spinlock its processor already holds, a stray audio
# handle...), and the previous build kept them quiet by patching them out.
add_compile_options(-O3)
add_compile_definitions(NDEBUG)
# ThinLTO across the whole tree (the guest, the runtime and the SDK libraries are one
# shared object), parallel and cached so an incremental link only redoes what changed.
add_compile_options(-flto=thin)
add_link_options(-flto=thin -Wl,--thinlto-jobs=all
    "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
# Profile-guided optimization of the code that matters (the recompiled guest and the
# runtime), driven by -DRENUT_PGO=gen|use (gradle: -PrenutPgo=gen|use):
#   gen  instruments those two targets; the profile is written on nativeStop and
#        nativeBackground (runtime_jni.cpp) to files/renut-%m.profraw.
#   use  RENUT_PGO_PROFILE is the merged .profdata (tools/pgo.py merge).
set(RENUT_PGO "" CACHE STRING "PGO mode: gen, use or empty")
set(RENUT_PGO_PROFILE "" CACHE FILEPATH "merged .profdata for RENUT_PGO=use")
function(renut_pgo_target target)
    if(RENUT_PGO STREQUAL "gen")
        target_compile_options(${target} PRIVATE -fprofile-generate)
        target_compile_definitions(${target} PRIVATE RENUT_PGO_GEN=1)
    elseif(RENUT_PGO STREQUAL "use")
        if(NOT EXISTS "${RENUT_PGO_PROFILE}")
            message(FATAL_ERROR "RENUT_PGO=use needs RENUT_PGO_PROFILE (merged .profdata): '${RENUT_PGO_PROFILE}'")
        endif()
        target_compile_options(${target} PRIVATE "-fprofile-use=${RENUT_PGO_PROFILE}"
            -Wno-profile-instr-unprofiled -Wno-profile-instr-missing -Wno-backend-plugin)
    endif()
endfunction()
# libc++ in the NDK lacks clock_time_conversion/clock_cast, which the SDK's chrono.h
# builds on (platform/android/shims/android_chrono.h).
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${RENUT_ANDROID}/shims/android_chrono.h>")
include("${REXGLUE_ROOT}/cmake/rex_pch.cmake")
set(REXGLUE_DERIVED_MAJOR 0)
set(REXGLUE_DERIVED_MINOR 10)
set(REXGLUE_DERIVED_PATCH 0)
set(REXGLUE_DERIVED_TWEAK 0)
set(REXGLUE_FULL_VERSION "0.10.0-renut-android")
set(REXGLUE_NUMERIC_VERSION "0.10.0")
set(REXGLUE_FLOOR_VERSION "0.10.0")
set(REXGLUE_VERSION_CHANNEL "renut-android")
set(REX_PLATFORM "android-arm64")
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file("${REXGLUE_ROOT}/include/rex/version.h.in" "${PROJECT_BINARY_DIR}/include/rex/version.h" @ONLY)
include_directories("${REXGLUE_ROOT}/include" "${PROJECT_BINARY_DIR}/include" "${RENUT_ANDROID}/include" "${RENUT_SRC}")
set(DEPS "${REXGLUE_ROOT}/thirdparty")
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/fmt" fmt)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/spdlog" spdlog)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/xxHash/cmake_unofficial" xxhash)
add_subdirectory("${DEPS}/utfcpp" utfcpp)
set(UCONTEXT "${RENUT_ROOT}/external/libucontext")
add_library(renut_ucontext STATIC "${UCONTEXT}/arch/aarch64/getcontext.S"
    "${UCONTEXT}/arch/aarch64/setcontext.S" "${UCONTEXT}/arch/aarch64/swapcontext.S"
    "${UCONTEXT}/arch/aarch64/makecontext.c" "${UCONTEXT}/arch/aarch64/trampoline.c")
target_include_directories(renut_ucontext PUBLIC "${UCONTEXT}/include" "${UCONTEXT}/arch/common/include"
    PRIVATE "${UCONTEXT}/arch/aarch64" "${UCONTEXT}/arch/common")
foreach(dep tomlplusplus simde disruptorplus)
    add_library(${dep} INTERFACE)
endforeach()
target_include_directories(tomlplusplus INTERFACE "${DEPS}/tomlplusplus/include")
target_include_directories(simde INTERFACE "${DEPS}/simde" "${DEPS}/simde/simde")
target_include_directories(disruptorplus INTERFACE "${DEPS}/disruptorplus/include")
add_library(aes128 STATIC "${DEPS}/aes_128/aes.c")
target_include_directories(aes128 PUBLIC "${DEPS}")
add_library(mspack STATIC "${DEPS}/libmspack/libmspack/mspack/lzxd.c")
target_include_directories(mspack PUBLIC "${DEPS}/libmspack/libmspack/mspack")
add_library(o1heap STATIC "${DEPS}/o1heap/o1heap/o1heap.c")
target_include_directories(o1heap PUBLIC "${DEPS}/o1heap/o1heap")
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/sdl3" sdl3)
add_library(imgui STATIC "${DEPS}/imgui/imgui.cpp" "${DEPS}/imgui/imgui_draw.cpp"
    "${DEPS}/imgui/imgui_tables.cpp" "${DEPS}/imgui/imgui_widgets.cpp")
add_library(imgui::imgui ALIAS imgui)
target_include_directories(imgui PUBLIC "${DEPS}/imgui")
add_subdirectory("${REXGLUE_ROOT}/src/core" core)
target_link_libraries(rexcore PUBLIC renut_ucontext)
# Bionic provides these functions in libc, with no separate pthread or rt library.
get_target_property(CORE_LIBRARIES rexcore LINK_LIBRARIES)
list(REMOVE_ITEM CORE_LIBRARIES pthread rt)
set_property(TARGET rexcore PROPERTY LINK_LIBRARIES "${CORE_LIBRARIES}")
get_target_property(CORE_INTERFACE rexcore INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM CORE_INTERFACE pthread rt "$<LINK_ONLY:pthread>" "$<LINK_ONLY:rt>")
set_property(TARGET rexcore PROPERTY INTERFACE_LINK_LIBRARIES "${CORE_INTERFACE}")
# Bionic has no ucontext; the SDK's fiber code reaches libucontext through names
# defined ahead of it, not through an edit.
set_source_files_properties("${REXGLUE_ROOT}/src/core/fiber_posix.cpp"
    TARGET_DIRECTORY rexcore PROPERTIES
    COMPILE_OPTIONS "-include;${RENUT_ANDROID}/shims/android_ucontext.h")
# The same for what the core files assume of glibc (platform/android/shims/android_core.h).
set_source_files_properties("${REXGLUE_ROOT}/src/core/memory_posix.cpp"
    "${REXGLUE_ROOT}/src/core/threading_posix.cpp"
    TARGET_DIRECTORY rexcore PROPERTIES
    COMPILE_OPTIONS "-include;${RENUT_ANDROID}/shims/android_core.h")
# QueryProtect is interposed by src/platform/android/query_protect.cpp, which only works
# while the SDK's own callers reach it through the PLT. ThinLTO imported it into them and
# bound the calls directly (the movies' MMIO fault path went back to parsing
# /proc/self/maps per fault: slow video), so its file stays a plain native object.
set_source_files_properties("${REXGLUE_ROOT}/src/core/memory_posix.cpp"
    TARGET_DIRECTORY rexcore PROPERTIES COMPILE_OPTIONS "-include;${RENUT_ANDROID}/shims/android_core.h;-fno-lto")
add_subdirectory("${REXGLUE_ROOT}/src/filesystem" filesystem)
add_subdirectory("${REXGLUE_ROOT}/src/system" system)
target_link_libraries(rexruntime PRIVATE rexcore rexfilesystem log android)
target_sources(rexruntime PRIVATE "${RENUT_ANDROID}/android_content.cpp" "${RENUT_ANDROID}/android_vulkan_loader.cpp")
target_link_libraries(rexruntime PRIVATE adrenotools)

# Reuse the pinned SDK's FFmpeg source list, without its desktop dependency setup.
file(READ "${DEPS}/CMakeLists.txt" THIRD_PARTY_CMAKE)
string(FIND "${THIRD_PARTY_CMAKE}" "set(FFMPEG_DIR" FFMPEG_START)
string(SUBSTRING "${THIRD_PARTY_CMAKE}" ${FFMPEG_START} -1 FFMPEG_CMAKE)
string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}" "${DEPS}" FFMPEG_CMAKE "${FFMPEG_CMAKE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg.cmake" "${FFMPEG_CMAKE}")
set(IS_ARM64 ON)
set(IS_X86_64 OFF)
include("${CMAKE_CURRENT_BINARY_DIR}/ffmpeg.cmake")

add_library(renderdoc INTERFACE)
target_include_directories(renderdoc INTERFACE "${DEPS}/renderdoc")
include("${CMAKE_CURRENT_SOURCE_DIR}/xenos-dependencies.cmake")
# Android uses SDL's native window, and has no X11 or Wayland dependencies.
file(READ "${REXGLUE_ROOT}/src/ui/CMakeLists.txt" UI_CMAKE)
string(FIND "${UI_CMAKE}" "# Platform-specific dependencies" UI_END)
string(SUBSTRING "${UI_CMAKE}" 0 ${UI_END} UI_CMAKE)
string(REPLACE "surface_gnulinux.cpp" "" UI_CMAKE "${UI_CMAKE}")
# The window is GameWindow (platform/android/android_game_window.h), a rex::ui::Window over the
# Android surface (and GameContext its app context); the SDK's SDL window includes
# Xlib unconditionally.
string(REGEX REPLACE "[ ]+window_sdl[.]cpp" "" UI_CMAKE "${UI_CMAKE}")
string(REGEX REPLACE "[ ]+windowed_app_context_sdl[.]cpp" "" UI_CMAKE "${UI_CMAKE}")
string(REPLACE "add_library(rexui OBJECT \${REXUI_SOURCES})"
    "list(TRANSFORM REXUI_SOURCES PREPEND \"${REXGLUE_ROOT}/src/ui/\")\nadd_library(rexui OBJECT \${REXUI_SOURCES})" UI_CMAKE "${UI_CMAKE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ui.cmake" "${UI_CMAKE}")
include("${CMAKE_CURRENT_BINARY_DIR}/ui.cmake")
add_subdirectory("${REXGLUE_ROOT}/src/input" input)
add_subdirectory("${REXGLUE_ROOT}/src/audio" audio)
add_subdirectory("${REXGLUE_ROOT}/src/kernel" kernel)
# No Xenos GPU plugin (rexgpu-xenos): the app hosts its own GPU. The renderer takes a handful
# of decoding helpers from the SDK's sources (windows/xenos-helpers.cmake lists them).
include("${RENUT_ROOT}/windows/xenos-helpers.cmake")
renut_xenos_helpers(renut_xenos "${REXGLUE_ROOT}" "" rexruntime)
target_link_libraries(renut_xenos PRIVATE renderdoc)
target_compile_definitions(renut_xenos PRIVATE REX_HAS_VULKAN=1 REX_HAS_D3D12=0)
set_target_properties(renut_xenos PROPERTIES CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(rexui PRIVATE REX_HAS_VULKAN=1 REX_HAS_D3D12=0)

include("${RENUT_ROOT}/generated/android/sources.cmake")
add_library(renut_guest OBJECT ${GENERATED_SOURCES})
target_include_directories(renut_guest PRIVATE "${RENUT_ROOT}/generated/android")
target_link_libraries(renut_guest PRIVATE rexruntime)
# The kernel exports reNut takes over (guest/kernel_overrides.cpp): the game's calls to
# them are renamed at compile time, the SDK is not touched.
include("${RENUT_SRC}/guest/kernel_overrides.cmake")
renut_kernel_overrides(renut_guest)
# The recompiled game is the hot path: a profile of the boot stall put ~90% of
# CPU inside two guest functions, so this target's level matters most.
target_compile_options(renut_guest PRIVATE -O3 -g0 -Wno-unused-variable -Wno-unused-label)
renut_pgo_target(renut_guest)
# The vector tables behind every lvx/stvx byte swap, copied verbatim out of the
# SDK as read-only data (renut_simd_fast.h explains why). Taken from the header
# rather than retyped, so an SDK update cannot leave a stale copy behind.
file(READ "${REXGLUE_ROOT}/include/rex/ppc/intrinsics.h" RENUT_PPC_INTRINSICS)
set(RENUT_VECTOR_TABLES "")
foreach(table VectorMaskL VectorMaskR VectorShiftTableL VectorShiftTableR)
    if(NOT RENUT_PPC_INTRINSICS MATCHES "inline uint8_t ${table}\\[\\] = \\{([^}]*)\\};")
        message(FATAL_ERROR "rex/ppc/intrinsics.h no longer defines ${table} the way renut_simd_fast.h expects")
    endif()
    string(APPEND RENUT_VECTOR_TABLES "alignas(16) static const uint8_t renut_${table}[] = {${CMAKE_MATCH_1}};\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/renut_vector_tables.inc" "${RENUT_VECTOR_TABLES}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REXGLUE_ROOT}/include/rex/ppc/intrinsics.h")
target_include_directories(renut_guest PRIVATE "${RENUT_SRC}" "${CMAKE_CURRENT_BINARY_DIR}")
# The generated header, then the AArch64 forms of the SSE intrinsics the game
# uses (clang scalarizes SIMDe's vector compares under -ffp-model=strict).
target_precompile_headers(renut_guest PRIVATE "${RENUT_SRC}/guest/renut_guest_pch.h")
# Native Plume renderer: the offline-recompiled shaders (generated/shaders, from
# tools/shaders/recompile_shaders.sh) are compiled in; their smol-v blob is
# embedded with .incbin.
set(RENUT_SHADERS "${RENUT_ROOT}/generated/shaders")
set_source_files_properties("${RENUT_SRC}/plume/shader_cache_blob.S" PROPERTIES
    COMPILE_OPTIONS "-DRENUT_SHADER_BLOB=\"${RENUT_SHADERS}/shader_spirv.smolv\""
    OBJECT_DEPENDS "${RENUT_SHADERS}/shader_spirv.smolv")
set(RENUT_RUNTIME_SOURCES
    # Shared with the Windows build
    guest/game_fixes.cpp guest/cheats.cpp guest/kernel_overrides.cpp guest/host_support.cpp
    d3d/d3d_layer.cpp d3d/plume_d3d.cpp
    d3d/native/lists.cpp d3d/native/flush.cpp d3d/native/draws.cpp d3d/native/commands.cpp d3d/native/sync.cpp d3d/native/entry.cpp d3d/native/report.cpp
    plume/plume_renderer.cpp plume/hiz_model.cpp
    plume/gpu/context.cpp plume/gpu/draw.cpp plume/gpu/draw_util.cpp plume/gpu/xenos/interpreter.cpp plume/gpu/xenos/register_file.cpp plume/gpu/xenos/registers.cpp plume/gpu/hiz.cpp plume/gpu/host.cpp
    plume/gpu/native_exec.cpp plume/gpu/options.cpp plume/gpu/patches.cpp plume/gpu/pipeline_cache.cpp
    plume/gpu/profiling.cpp plume/gpu/recorder.cpp plume/gpu/report.cpp plume/gpu/resolve.cpp
    plume/gpu/shaders.cpp plume/gpu/streams.cpp plume/gpu/support.cpp plume/gpu/swap.cpp
    plume/gpu/targets.cpp plume/gpu/textures.cpp plume/plume_textures.cpp plume/shader_cache_blob.S
    vendor/smolv/smolv.cpp
    # Android only
    platform/android/runtime_jni.cpp platform/android/achievements_bridge.cpp
    platform/android/text_input_bridge.cpp platform/android/adpf.cpp platform/android/vulkan_entry.cpp
    platform/android/renut_audio.cpp platform/android/xam_users.cpp platform/android/query_protect.cpp
    platform/android/hardware_concurrency.cpp)
list(TRANSFORM RENUT_RUNTIME_SOURCES PREPEND "${RENUT_SRC}/")
add_library(renut_runtime SHARED ${RENUT_RUNTIME_SOURCES}
    "${RENUT_SHADERS}/shader_cache.cpp" $<TARGET_OBJECTS:renut_guest>)
# generated/shaders/shader_cache.cpp includes "shader_cache.h" by bare name.
target_include_directories(renut_runtime PRIVATE "${RENUT_ROOT}/generated/android" "${RENUT_SHADERS}"
    "${RENUT_SRC}" "${RENUT_SRC}/plume")
target_link_libraries(renut_runtime PRIVATE rexruntime renut_xenos renut_plume adrenotools
    xxHash::xxhash android log dl aaudio)
# Every call between recompiled guest functions, and to the __savegprlr/
# __restgprlr helpers that each prologue and epilogue calls, went through a PLT
# stub: they are weak, default-visibility functions of a shared library, so the
# linker has to assume another library may preempt them. 23.8 thousand stubs,
# up to three indirect jumps per guest call. Nothing outside this library takes
# their place (the strong overrides in game_fixes.cpp are settled at link time),
# so bind them locally and the calls become plain branches.
target_link_options(renut_runtime PRIVATE -Wl,-Bsymbolic-functions)
renut_pgo_target(renut_runtime)
if(RENUT_PGO STREQUAL "gen")
    target_link_options(renut_runtime PRIVATE -fprofile-generate)
endif()
