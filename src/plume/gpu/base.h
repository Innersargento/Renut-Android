#pragma once
// Includes, constants and small helpers shared by the whole Plume GPU.
#include "plume_gpu.h"
#include "d3d/d3d_layer.h"
#include "d3d/native_d3d.h"

#include "frame_pacing.h"
#include "plume_renderer.h"
#include <rex/chrono/clock.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "plume_textures.h"
#include "plume_vulkan.h"
#include "shader_cache.h"
#include "xenos_ucode_key.h"
#include "vendor/smolv/smolv.h"
#include "spirv_rewrites.h"
#include "hiz_model.h"

#include <pthread.h>
#include <thread>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Lowers the calling thread's priority by `nice` (Unix niceness: 0 is the
// default, 10 the most background). Windows maps it in windows/platform_windows.cpp.
#if defined(_WIN32)
void renutSetThreadNice(int nice);
uint64_t renutPhysicalMemoryBytes();
static inline void renutMakeDirectory(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directory(path, error);
}
#else
#include <sys/resource.h>
static inline void renutSetThreadNice(int nice) { setpriority(PRIO_PROCESS, uint32_t(gettid()), nice); }
static inline uint64_t renutPhysicalMemoryBytes() {
  return uint64_t(sysconf(_SC_PHYS_PAGES)) * uint64_t(sysconf(_SC_PAGESIZE));
}
static inline void renutMakeDirectory(const std::string& path) { mkdir(path.c_str(), 0755); }
#endif

#include <algorithm>
#include <array>
#include <bitset>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <atomic>
#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/memory/ring_buffer.h>
#include <rex/thread.h>

#include <fmt/format.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <span>

// Frames of silence the audio output played for want of one from the title
// (the SDL driver in rexglue).
extern "C" uint64_t renut_audio_silent_frames();

REXCVAR_DECLARE(bool, renut_plume_async_pipelines);
REXCVAR_DECLARE(int32_t, renut_plume_pipeline_lookup_ms);
REXCVAR_DECLARE(int32_t, renut_plume_bool_spec);
REXCVAR_DECLARE(int32_t, renut_plume_defer_draws);
REXCVAR_DECLARE(int32_t, renut_plume_hiz);
REXCVAR_DECLARE(bool, renut_plume_shader_cache);
REXCVAR_DECLARE(bool, renut_plume_key_reuse);
REXCVAR_DECLARE(int32_t, renut_plume_fetch_spec);
REXCVAR_DECLARE(int32_t, renut_plume_depth_strip);
REXCVAR_DECLARE(int32_t, renut_plume_rel_constants);
REXCVAR_DECLARE(int32_t, renut_plume_adaptive_patches);
REXCVAR_DECLARE(bool, renut_plume_no_shadows);
REXCVAR_DECLARE(bool, renut_plume_no_bloom);
REXCVAR_DECLARE(bool, renut_plume_memexport_readback);
REXCVAR_DECLARE(bool, renut_plume_memexport_fast);
REXCVAR_DECLARE(int32_t, renut_plume_low_memory);
REXCVAR_DECLARE(int32_t, renut_plume_texture_max_dim);
REXCVAR_DECLARE(int32_t, renut_plume_bc_textures);
REXCVAR_DECLARE(int32_t, renut_plume_texture_quality);
REXCVAR_DECLARE(int32_t, renut_plume_shadow_quality);
REXCVAR_DECLARE(int32_t, renut_plume_internal_resolution);
REXCVAR_DECLARE(bool, renut_plume_precompile);

// game_fixes.cpp: how the MSAA-off path treats the scene's band split
// (<files>/plume-keep-split: 0 clear, 1 leave, 2 whole-screen projection).
extern std::atomic<int> g_renut_scene_split_mode;
REXCVAR_DECLARE(bool, renut_scene_msaa);
REXCVAR_DECLARE(bool, vsync);  // the SDK's: off ticks the guest's vblank every millisecond

namespace renut::gpu {

using namespace plume;
namespace gfx = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
// Register indices (XE_GPU_REG_*).
using rex::graphics::Register;
using namespace rex::graphics;


// The push constants: the guest's three block addresses (24 bytes). The range is
// declared wider than that.
constexpr uint32_t kPushBytes = 128;

// Where microcode without a recompiled shader is saved (SetPlumeMissDumpDirectory).
inline std::string g_miss_dump_directory;
inline bool g_dedupe_descriptors = false;  // files/plume-dedupe-desc (see SetTexture2D)
inline bool g_validate_windows = false;    // files/plume-validate (see ValidateRange)

// The game's own VdSwap count, which is what the on-screen counter shows. Plume
// presents more often than the game swaps, so the two rates have to be read
// side by side to know which one the player is actually seeing.
extern "C" uint64_t renut_guest_swap_count();
extern "C" void renut_note_guest_swap();

struct UcodeMapEntry {
  uint64_t ucode;
  uint64_t loose;
  uint64_t container;
};
// The microcode keys of the recompiled shaders (generated/shaders), in support.cpp.
std::span<const UcodeMapEntry> UcodeMap();

constexpr uint32_t kMaxTextures2D = 4096;
constexpr uint32_t kMaxTextures3D = 128;
constexpr uint32_t kMaxTexturesCube = 16;
constexpr uint32_t kMaxSamplers = 256;
// Per frame in flight. Showdown Town fills about 66 MB of it a frame and has
// peaked at 126, so a slice is sized like the old single ring rather than half
// of it: an overflow costs a full drain of the GPU in the middle of a frame,
// which is exactly what the pipelining is there to avoid.
constexpr uint64_t kUploadSize = 128ull << 20;
// Bindings 0..15 are the guest's vertex streams; this one feeds zeros to any
// input the draw has no data for.
constexpr uint32_t kZeroBinding = 16;
// Where the expansion of a patch draw is bound: one float4 per generated
// vertex, xyz the point's weights within its patch and w the patch's index.
constexpr uint32_t kPatchDomainBinding = 15;
// How finely a patch is expanded, in case the guest asks for more than the
// expansion is willing to build.
constexpr uint32_t kMaxPatchLevel = 16;
// Container hash of Direct3D's Clear vertex shader (microcode 1E6883FCCDE1F688,
// wrapped by tools/shaders/wrap_raw_ucode.py): position and colour of a
// rectangle from vertex fetch constant 0.

// Descriptor 0 of the sampler heap, for a fetch whose state cannot be built.
constexpr uint32_t kSamplerDefault = 0;

inline uint32_t BSwap32(uint32_t v) { return __builtin_bswap32(v); }

inline uint64_t ThreadCpuMicroseconds() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return uint64_t(now.tv_sec) * 1000000u + uint64_t(now.tv_nsec) / 1000u;
}

// Copies guest data into host order, as the fetch constant's endian field says.
// Swaps the guest's big-endian data into the upload ring.
//
// Sixteen bytes at a time where the size allows it. That matters twice over:
// the byte reversal is one instruction instead of a load, a bswap and a store,
// and the ring is write-combined memory, where four-byte stores leave most of
// each write-combine buffer unused. Measured on the thread that records the
// command list, this was its single largest cost at ~19% of it.
inline void CopySwapped(uint8_t* dst, const uint8_t* src, size_t size, xenos::Endian endian) {
  size_t i = 0;
  switch (endian) {
    case xenos::Endian::k8in16:
#if defined(__aarch64__)
      for (; i + 16 <= size; i += 16) {
        vst1q_u8(dst + i, vrev16q_u8(vld1q_u8(src + i)));
      }
#endif
      for (; i + 1 < size; i += 2) {
        // Read both before writing either: the tail may overlap in place.
        const uint8_t low = src[i], high = src[i + 1];
        dst[i] = high;
        dst[i + 1] = low;
      }
      break;
    case xenos::Endian::k8in32:
#if defined(__aarch64__)
      for (; i + 16 <= size; i += 16) {
        vst1q_u8(dst + i, vrev32q_u8(vld1q_u8(src + i)));
      }
#endif
      for (; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = BSwap32(v);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
#if defined(__aarch64__)
      for (; i + 16 <= size; i += 16) {
        vst1q_u8(dst + i,
                 vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(vld1q_u8(src + i)))));
      }
#endif
      for (; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    default:
      std::memcpy(dst, src, size);
      break;
  }
}

template <typename T>
void HashCombine(uint64_t& seed, const T& value) {
  seed = XXH3_64bits_withSeed(&value, sizeof(value), seed);
}

// Bytes laid end to end for one hash, instead of one per field.
struct KeyBytes {
  alignas(8) uint8_t data[1024];
  size_t size = 0;
  template <typename T>
  void Put(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (size + sizeof(T) > sizeof(data)) return;
    std::memcpy(data + size, &value, sizeof(T));
    size += sizeof(T);
  }
  uint64_t Hash() const { return XXH3_64bits(data, size); }
};

// A small direct-mapped memo in front of one of the node-based maps the draw
// path asks thousands of times a frame. A libc++ unordered_map lookup walks a
// bucket to a node - two cache misses more often than not - and those lookups
// were ~15% of the command thread; the memo is one contiguous array. It holds
// pointers to the map's nodes, which stay where they are until erased, so
// whatever erases from the map clears its memo.
template <typename T, uint32_t kBits = 10>
struct LookupMemo {
  struct Entry {
    uint64_t key = 0;
    T* value = nullptr;
  };
  std::array<Entry, size_t(1) << kBits> entries{};
  static size_t Slot(uint64_t key) { return size_t((key * 0x9E3779B97F4A7C15ull) >> (64 - kBits)); }
  T* Find(uint64_t key) const {
    const Entry& entry = entries[Slot(key)];
    return entry.value && entry.key == key ? entry.value : nullptr;
  }
  void Put(uint64_t key, T* value) { entries[Slot(key)] = {key, value}; }
  void Clear() { entries.fill({}); }
};

// Input locations a SPIR-V vertex shader declares, and whether each is an
// integer vector (BLENDINDICES is uint4; everything else float4).
std::vector<std::pair<uint32_t, bool>> ReflectInputs(const std::vector<uint32_t>& spirv);

}  // namespace renut::gpu
