cmake_minimum_required(VERSION 3.25)
project(renut_rexglue_android LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(RENUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# The code reNut itself writes: src/.
set(RENUT_SRC "${RENUT_ROOT}/src")
set(RENUT_ANDROID "${RENUT_SRC}/platform")
# The SDK is the git submodule thirdparty/rexglue-sdk (tag v0.11.0) exactly as
# released: nothing under thirdparty/ is edited, patched or copied. Its modules are shared
# libraries; what the app adds is its own drivers (src/platform) and the title's code.
set(REXGLUE_ROOT "${RENUT_ROOT}/thirdparty/rexglue-sdk")
set(PROJECT_SOURCE_DIR "${REXGLUE_ROOT}")
set(REXGLUE_ENABLE_TRACY OFF)
set(REXGLUE_ENABLE_PERF_COUNTERS ON)
set(CMAKE_INSTALL_INCLUDEDIR include)
add_compile_definitions(REXGLUE_BUILD_CONFIG="Release")
add_compile_options(-fno-strict-aliasing -ffp-model=strict -fno-char8_t)
# The SDK is built with hidden visibility, and rex::ThreadStop is thrown in one shared library (a XAM thunk)
# and caught in another (the kernel thread). libc++ on Android compares the type_info of a hidden type by
# address, so each library's own copy never matches and the unwinding of a stopping thread ends in
# std::terminate. Types (their typeinfo and vtables) keep default visibility; functions stay hidden.
add_compile_options("SHELL:-Xclang -ftype-visibility=default")
# assembleDebug leaves CMAKE_BUILD_TYPE=Debug, so clang defaulted to -O0 for
# every target except the SDK's GPU plugin: the kernel, runtime and shader compiler all
# ran unoptimized even though REXGLUE_BUILD_CONFIG says "Release". Optimize the
# whole tree. NDEBUG is how the SDK is released: its asserts are checks of
# the console's own invariants that the title legitimately breaks (a graphics
# interrupt finding a spinlock its processor already holds, a stray audio
# handle...), and the previous build kept them quiet by patching them out.
add_compile_options(-O3)
add_compile_definitions(NDEBUG)
# ThinLTO inside each library (the guest and the app are one, each SDK module another), parallel and
# cached so an incremental link only redoes what changed.
add_compile_options(-flto=thin)
add_link_options(-flto=thin -Wl,--thinlto-jobs=all
    "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
include("${REXGLUE_ROOT}/cmake/rex_pch.cmake")
include("${REXGLUE_ROOT}/cmake/rex_header_set.cmake")
include("${REXGLUE_ROOT}/cmake/rex_version.cmake")
include("${REXGLUE_ROOT}/cmake/rexglue_target.cmake")
include("${REXGLUE_ROOT}/cmake/rexglue_module.cmake")
include("${REXGLUE_ROOT}/cmake/rexglue_metadata.cmake")
include("${REXGLUE_ROOT}/cmake/rexglue_init_audit.cmake")
include("${REXGLUE_ROOT}/cmake/rexglue_physical_offset.cmake")
rexglue_cpu_flags(REXGLUE_CPU_FLAGS)
add_compile_options(${REXGLUE_CPU_FLAGS})
set(REXGLUE_DERIVED_MAJOR 0)
set(REXGLUE_DERIVED_MINOR 11)
set(REXGLUE_DERIVED_PATCH 0)
set(REXGLUE_DERIVED_TWEAK 0)
set(REXGLUE_FULL_VERSION "0.11.0-renut-android")
set(REXGLUE_BUILD_IDENTITY "${REXGLUE_FULL_VERSION}")
set(REXGLUE_NUMERIC_VERSION "0.11.0")
set(REXGLUE_FLOOR_VERSION "0.11.0")
set(REXGLUE_VERSION_CHANNEL "renut-android")
set(REX_PLATFORM "linux-arm64")
string(TIMESTAMP REXGLUE_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file("${REXGLUE_ROOT}/include/rex/version.h.in" "${PROJECT_BINARY_DIR}/include/rex/version.h" @ONLY)
configure_file("${REXGLUE_ROOT}/src/base/build_stamp.cpp.in" "${PROJECT_BINARY_DIR}/generated/base/build_stamp.cpp" @ONLY)
include_directories("${RENUT_SRC}")

# The SDK's third-party libraries, from the sources the SDK pins. Its own thirdparty/CMakeLists.txt
# needs the Vulkan SDK of a desktop host and builds the shader toolchain, which a phone build has no use
# for, so the ones the modules link are declared here.
set(DEPS "${REXGLUE_ROOT}/thirdparty")
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
set(SPDLOG_DISABLE_DEFAULT_LOGGER ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/spdlog" spdlog)
target_compile_options(spdlog PRIVATE -w)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
set(XXHASH_BUNDLED_MODE ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/xxHash/cmake_unofficial" xxhash)
target_compile_options(xxhash PRIVATE -w)
# SDL3 only because the SDK's apu, hid and ui CMake add their sdl3 driver modules unconditionally and those
# link it; android/app/build.gradle builds only the app's targets, so neither is compiled nor shipped.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/sdl3" sdl3)
foreach(dep tomlplusplus simde renderdoc)
    add_library(${dep} INTERFACE)
endforeach()
target_include_directories(tomlplusplus SYSTEM INTERFACE "${DEPS}/tomlplusplus/include")
target_include_directories(simde INTERFACE "${DEPS}/simde" "${DEPS}/simde/simde")
target_include_directories(renderdoc INTERFACE "${DEPS}/renderdoc")
add_library(mspack STATIC "${DEPS}/libmspack/libmspack/mspack/lzxd.c")
target_include_directories(mspack PUBLIC "${DEPS}/libmspack/libmspack/mspack")
target_compile_options(mspack PRIVATE -w)
set(EXCRYPT_DIR "${DEPS}/excrypt/src")
add_library(excrypt STATIC
    ${EXCRYPT_DIR}/excrypt_aes.c ${EXCRYPT_DIR}/excrypt_bn.c ${EXCRYPT_DIR}/excrypt_bn_key.cpp
    ${EXCRYPT_DIR}/excrypt_bn_mod.cpp ${EXCRYPT_DIR}/excrypt_bn_pkcs1.cpp ${EXCRYPT_DIR}/excrypt_bn_sig.c
    ${EXCRYPT_DIR}/excrypt_des.c ${EXCRYPT_DIR}/excrypt_ecc.c ${EXCRYPT_DIR}/excrypt_md5.c
    ${EXCRYPT_DIR}/excrypt_mem.cpp ${EXCRYPT_DIR}/excrypt_parve.c ${EXCRYPT_DIR}/excrypt_rc4.c
    ${EXCRYPT_DIR}/excrypt_rotsum.c ${EXCRYPT_DIR}/excrypt_sha.c ${EXCRYPT_DIR}/excrypt_sha2.c
    ${EXCRYPT_DIR}/rijndael.c)
target_include_directories(excrypt PUBLIC "${EXCRYPT_DIR}")
set_source_files_properties(${EXCRYPT_DIR}/excrypt_bn_key.cpp ${EXCRYPT_DIR}/excrypt_bn_mod.cpp
    ${EXCRYPT_DIR}/excrypt_bn_pkcs1.cpp ${EXCRYPT_DIR}/excrypt_mem.cpp
    PROPERTIES COMPILE_OPTIONS "-include;cstring;-include;algorithm")
set_source_files_properties(${EXCRYPT_DIR}/excrypt_sha2.c PROPERTIES COMPILE_OPTIONS "-Dmin(a,b)=((a)<(b)?(a):(b))")
target_compile_options(excrypt PRIVATE -w)
add_library(tlsf STATIC "${DEPS}/tlsf/tlsf.c")
target_include_directories(tlsf PUBLIC "${DEPS}/tlsf")
target_compile_options(tlsf PRIVATE -w)
# The SDK's FFmpeg source list (libavutil and libavcodec, for XMA), taken from its thirdparty/CMakeLists.txt
# from the FFmpeg section on, which has no dependency on the rest of that file.
file(READ "${DEPS}/CMakeLists.txt" THIRD_PARTY_CMAKE)
string(FIND "${THIRD_PARTY_CMAKE}" "set(FFMPEG_DIR" FFMPEG_START)
string(SUBSTRING "${THIRD_PARTY_CMAKE}" ${FFMPEG_START} -1 FFMPEG_CMAKE)
string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}" "${DEPS}" FFMPEG_CMAKE "${FFMPEG_CMAKE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg.cmake" "${FFMPEG_CMAKE}")
set(IS_ARM64 ON)
set(IS_X86_64 OFF)
include("${CMAKE_CURRENT_BINARY_DIR}/ffmpeg.cmake")

# Bionic has no ucontext; the SDK's fiber code takes libucontext's.
set(UCONTEXT "${RENUT_ROOT}/thirdparty/libucontext")
add_library(renut_ucontext STATIC "${UCONTEXT}/arch/aarch64/getcontext.S"
    "${UCONTEXT}/arch/aarch64/setcontext.S" "${UCONTEXT}/arch/aarch64/swapcontext.S"
    "${UCONTEXT}/arch/aarch64/makecontext.c" "${UCONTEXT}/arch/aarch64/trampoline.c")
target_include_directories(renut_ucontext PUBLIC "${UCONTEXT}/include" "${UCONTEXT}/arch/common/include"
    PRIVATE "${UCONTEXT}/arch/aarch64" "${UCONTEXT}/arch/common")

# The SDK's modules link pthread and rt, which bionic has no libraries for (they are in libc): targets of
# those names stand in for them.
add_library(pthread INTERFACE)
add_library(rt INTERFACE)

# The SDK's modules, each one a shared library the app loads by name.
add_subdirectory("${REXGLUE_ROOT}/src/base" base)
target_link_libraries(rexbase PRIVATE renut_ucontext)
add_subdirectory("${REXGLUE_ROOT}/src/xenos" xenos)
add_subdirectory("${REXGLUE_ROOT}/src/ppc" ppc)
add_subdirectory("${REXGLUE_ROOT}/src/filesystem" filesystem)
add_subdirectory("${REXGLUE_ROOT}/src/apu" apu)
add_subdirectory("${REXGLUE_ROOT}/src/hid" hid)
add_subdirectory("${REXGLUE_ROOT}/src/ui" ui)
add_subdirectory("${REXGLUE_ROOT}/src/kernel" kernel)
add_subdirectory("${REXGLUE_ROOT}/src/xam" xam)
add_subdirectory("${REXGLUE_ROOT}/src/xbdm" xbdm)
add_subdirectory("${REXGLUE_ROOT}/src/app" app)
# The graphics module is the SDK's own sources; its CMakeLists asks pkg-config for X11 and Wayland, which a
# phone has neither of, so it is declared here with the same sources and settings. The Xenos GPU plugins
# (src/gpu/xenos) are not built: the app is the GPU driver.
add_library(rexgpu SHARED "${REXGLUE_ROOT}/src/gpu/graphics_module.cpp"
    "${REXGLUE_ROOT}/src/gpu/graphics_system.cpp" "${REXGLUE_ROOT}/src/gpu/present_thread.cpp")
add_library(rex::gpu ALIAS rexgpu)
rexglue_link_host_allocator(rexgpu)
set_target_properties(rexgpu PROPERTIES EXPORT_NAME gpu CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_definitions(rexgpu PRIVATE REX_BUILDING_GPU)
rex_target_pch(rexgpu)
target_include_directories(rexgpu PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/thirdparty")
target_link_libraries(rexgpu PUBLIC rexbase)
rexglue_add_version_resource(rexgpu DESCRIPTION "ReXGlue graphics")

# The SDK's Android branches lean on helpers it does not define, and its memory, thread, crash and file
# code on functions bionic only has in later API levels than the app's minimum; platform/shims/android_core.h
# declares what they need, force-included into those four files.
target_include_directories(rexbase PRIVATE "${RENUT_ANDROID}/include")
target_include_directories(rexfilesystem-objects PRIVATE "${RENUT_ANDROID}/include")
set_source_files_properties("${REXGLUE_ROOT}/src/base/threading_posix.cpp" "${REXGLUE_ROOT}/src/base/crash_posix.cpp"
    "${REXGLUE_ROOT}/src/base/memory_posix.cpp" TARGET_DIRECTORY rexbase PROPERTIES
    COMPILE_OPTIONS "-include;${RENUT_ANDROID}/shims/android_core.h")
set_source_files_properties("${REXGLUE_ROOT}/src/filesystem/host_file_posix.cpp"
    TARGET_DIRECTORY rexfilesystem-objects PROPERTIES
    COMPILE_OPTIONS "-include;${RENUT_ANDROID}/shims/android_core.h")
set_source_files_properties("${REXGLUE_ROOT}/src/base/fiber_posix.cpp" TARGET_DIRECTORY rexbase PROPERTIES
    COMPILE_OPTIONS "-include;${RENUT_ANDROID}/shims/android_ucontext.h")

include("${RENUT_ROOT}/generated/android/sources.cmake")
add_library(renut_guest OBJECT ${GENERATED_SOURCES})
target_include_directories(renut_guest PRIVATE "${RENUT_ROOT}/generated/android")
target_link_libraries(renut_guest PRIVATE rexbase rexppc rexkernel)
# The kernel exports reNut takes over (guest/kernel_overrides.cpp): the game's calls to
# them are renamed at compile time, the SDK is not touched.
include("${RENUT_SRC}/guest/kernel_overrides.cmake")
renut_kernel_overrides(renut_guest)
# The recompiled game is the hot path: a profile of the boot stall put ~90% of
# CPU inside two guest functions, so this target's level matters most.
target_compile_options(renut_guest PRIVATE -O3 -g0 -Wno-unused-variable -Wno-unused-label)
target_include_directories(renut_guest PRIVATE "${RENUT_SRC}")
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
    guest/game_fixes.cpp guest/frame_fixes.cpp guest/cheats.cpp guest/kernel_overrides.cpp guest/host_support.cpp guest/widescreen.cpp guest/field_of_view.cpp
    d3d/hooks/layer.cpp d3d/hooks/plume_d3d.cpp d3d/device/state.cpp d3d/device/bound.cpp d3d/device/getters.cpp d3d/sync/fence.cpp d3d/resources/resource.cpp d3d/resources/shader.cpp d3d/resources/microcode.cpp d3d/device/misc.cpp d3d/scene/frame.cpp d3d/scene/swap.cpp d3d/scene/startup.cpp d3d/scene/tiling.cpp d3d/scene/command_buffers.cpp d3d/device/leaves.cpp d3d/device/lists.cpp d3d/hooks/handlers.cpp
    d3d/frontend/lists.cpp d3d/frontend/flush.cpp d3d/frontend/draws.cpp d3d/frontend/commands.cpp d3d/frontend/sync.cpp d3d/frontend/entry.cpp d3d/frontend/report.cpp
    plume/plume_renderer.cpp plume/hiz_model.cpp
    gpu/core/context.cpp plume/shaders/depth_resolve.comp.cpp plume/shaders/depth_resolve_ms.comp.cpp plume/shaders/depth_stencil_pack.comp.cpp plume/shaders/patch_expand.comp.cpp plume/shaders/patch_levels.comp.cpp plume/shaders/point_list.geom.cpp plume/shaders/present.frag.cpp plume/shaders/present.vert.cpp plume/shaders/point_list_position.geom.cpp plume/shaders/rect_list.geom.cpp plume/shaders/rect_list_position.geom.cpp gpu/draw/draw.cpp gpu/draw/draw_util.cpp gpu/xenos/interpreter.cpp gpu/xenos/shader.cpp gpu/draw/hiz.cpp gpu/core/host.cpp
    gpu/exec/native_exec.cpp gpu/core/options.cpp gpu/draw/patches.cpp gpu/draw/pipeline_cache.cpp
    gpu/exec/recorder.cpp gpu/targets/resolve.cpp
    gpu/draw/shaders.cpp gpu/draw/streams.cpp gpu/core/support.cpp gpu/targets/swap.cpp
    gpu/targets/targets.cpp gpu/targets/textures.cpp plume/plume_textures.cpp plume/shader_cache_blob.S
    vendor/smolv/smolv.cpp
    # Android only
    platform/runtime_jni.cpp platform/renut_app.cpp platform/android_drivers.cpp platform/audio_threads.cpp platform/achievements_bridge.cpp
    platform/text_input_bridge.cpp platform/adpf.cpp platform/vulkan_entry.cpp
    platform/xam_users.cpp
    platform/hardware_concurrency.cpp platform/settings.cpp platform/guest.cpp)
list(TRANSFORM RENUT_RUNTIME_SOURCES PREPEND "${RENUT_SRC}/")
add_library(renut_runtime SHARED ${RENUT_RUNTIME_SOURCES}
    "${RENUT_SHADERS}/shader_cache.cpp" $<TARGET_OBJECTS:renut_guest>)
# generated/shaders/shader_cache.cpp includes "shader_cache.h" by bare name.
target_include_directories(renut_runtime PRIVATE "${RENUT_ROOT}/generated/android" "${RENUT_SHADERS}"
    "${RENUT_SRC}" "${RENUT_SRC}/plume")
# The SDK's libraries allocate through its own allocator (rex/memory/host.h) and free blocks of other
# libraries with it, so the app's library takes the same operator new and delete (the SDK's own helper).
rexglue_link_host_allocator(renut_runtime)
target_link_libraries(renut_runtime PRIVATE rexapp rexkernel rexxam rexxbdm rexgpu rexui rexhid rexapu
    rexfilesystem rexppc rexxenos rexbase renut_plume adrenotools xxHash::xxhash android log dl aaudio)
target_sources(renut_runtime PRIVATE "${RENUT_ANDROID}/android_vulkan_loader.cpp")
# Every call between recompiled guest functions, and to the __savegprlr/
# __restgprlr helpers that each prologue and epilogue calls, went through a PLT
# stub: they are weak, default-visibility functions of a shared library, so the
# linker has to assume another library may preempt them. 23.8 thousand stubs,
# up to three indirect jumps per guest call. Nothing outside this library takes
# their place (the strong overrides in game_fixes.cpp are settled at link time),
# so bind them locally and the calls become plain branches.
target_link_options(renut_runtime PRIVATE -Wl,-Bsymbolic-functions)
