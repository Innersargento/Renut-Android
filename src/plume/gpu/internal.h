#pragma once
// The command processor of the Plume GPU, split by concern across src/plume/gpu/:
//
//   internal.h        this file: types, helpers and the PlumeCommandProcessor class
//   options.cpp       the renut_plume_* cvars
//   support.cpp       free helpers (formats, blend, shader inputs, recipes, the ucode map)
//   pipeline_cache.cpp  pipeline workers, recipes, pipeline prediction
//   context.cpp       Vulkan context set-up and tear-down, the worker loop
//   streams.cpp       upload ring, kept vertex streams, memory exports, constants
//   profiling.cpp     GPU timestamps and the pair profiler
//   targets.cpp       frame start, render targets, framebuffers, resolved images
//   textures.cpp      guest textures and samplers
//   shaders.cpp       register writes, shader lookup, pipeline statistics
//   native_exec.cpp   the native lists the D3D front end records
//   draw.cpp          IssueDraw
//   recorder.cpp      draw recording thread, deferred draws, device-lost report
//   hiz.cpp           Hi-Z culling
//   resolve.cpp       resolves and depth copies
//   patches.cpp       tessellation patch expansion
//   swap.cpp          IssueSwap, the frame's present
//   report.cpp        memory and statistics reports
//   host.cpp          PlumeGpu (the hosted video hardware) and the public API of plume_gpu.h
//
// All of it lives in renut::gpu. Nothing here is for other parts of the app; they use
// plume_gpu.h.
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

// ---------------------------------------------------------------------------
// Recompiled shaders
// ---------------------------------------------------------------------------

// One recompiled shader, shared by every bound variant of it.
struct CompiledShader {
  std::unique_ptr<RenderShader> module;
  // The same module with its live vertex fetch table read from specialization
  // constants instead of the uniform block (spirv_rewrites.h). Only vertex
  // and domain shaders with live fetches have one.
  std::unique_ptr<RenderShader> fetch_spec_module;
  // The vertex shader's two forms again without its interpolant outputs
  // (spirv_rewrites.h), for draws that have no fragment shader.
  std::unique_ptr<RenderShader> depth_module;
  std::unique_ptr<RenderShader> depth_fetch_spec_module;
  std::vector<std::pair<uint32_t, bool>> inputs;  // vertex shaders only
  // Its boolean registers are read from the shared block rather than
  // specialized on (spirv_rewrites.h), so they stay out of the pipeline.
  bool dynamic_bools = false;
  bool failed = false;
};

// Specialization constant ids 64 + slot * 4 + word hold the live fetch table.
constexpr uint32_t kFetchSpecBase = 64;

// A vertex element as the bound microcode fetches it.
struct LiveElement {
  uint32_t location;
  uint32_t slot;
  uint32_t offset, stride;
  uint32_t format;
  bool is_signed, normalized;
  uint32_t usage, usage_index;
  // What the live fetch does to the fetched components that the recompiled
  // one does not, as RENUT_SHARED_REMAP_OFFSET stores it; see FetchRemap.
  uint32_t remap = 0;
};

class HostShader : public gfx::Shader {
 public:
  HostShader(xenos::ShaderType type, uint64_t hash, const uint32_t* ucode, size_t count)
      : Shader(type, hash, ucode, count) {}

  const ShaderCacheEntry* entry = nullptr;
  // What the cache is keyed by, which is not this shader's own hash.
  uint64_t ucode_key = 0;
  uint64_t loose_key = 0;
  // Texture fetch constants this shader reads; ~0 until TextureMask ran.
  uint32_t texture_mask = 0;
  bool texture_mask_decoded = false;
  // Boolean registers the control flow tests, a bit per register in words of
  // 32; the pipeline is specialized on these (RENUT_SPEC_BOOLS_BASE).
  uint32_t bool_mask[8] = {};
  bool bool_mask_decoded = false;
  CompiledShader* compiled = nullptr;
  // Vertex shaders: the input layout decoded from this variant's vfetches.
  std::vector<LiveElement> elements;
  // Fetches the shader indexes with a computed register (vfetchLive): slot k
  // of the shared fetch table, as uint4 {stream | format << 8 | flags << 16,
  // stride, offset, exp scale bits}.
  std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>> live_fetches;
  bool elements_decoded = false;
  // The same microcode recompiled as the domain stage of a tessellated draw.
  // It reads its control points by patch index, so every one of its fetches is
  // a live fetch and the table differs from the vertex form's.
  const ShaderCacheEntry* domain_entry = nullptr;
  CompiledShader* domain_compiled = nullptr;
  std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>> domain_live_fetches;
  bool domain_decoded = false;
  // One hash of each fetch table (vertex form, domain form), made the first
  // time a draw keys a pipeline by it (0: not made yet).
  uint64_t fetch_spec_hash[2] = {0, 0};
  // Hi-Z: the model entry last found for this shader and the (masked) booleans
  // it was found under, so a draw does not hash and look them up again.
  void* hiz_entry = nullptr;
  uint32_t hiz_bools[8] = {};
  // Format-17 elements to repack (offset, stride) by stream, found once the
  // shader is compiled; signature mixes them for the stream memo.
  std::array<std::vector<std::pair<uint32_t, uint32_t>>, 17> repack;
  std::array<uint64_t, 17> repack_signature{};
  bool repack_decoded = false;
};

class ShaderStore {
 public:
  ShaderStore() {
    for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
      by_container_.emplace(g_shaderCacheEntries[i].hash, &g_shaderCacheEntries[i]);
    }
    for (const auto& m : UcodeMap()) {
      by_ucode_.emplace(m.ucode, m.container);
      // The fallback keeps the first container of each loose key, which is the
      // container the store answered with before the swizzle joined the key.
      by_loose_.emplace(m.loose, m.container);
    }
    REXLOG_INFO("Renut: Plume shader cache: {} shaders, {} microcode keys, {} MB smol-v",
                g_shaderCacheEntryCount, by_ucode_.size(),
                size_t(g_spirvCacheEnd - g_spirvCache) >> 20);
  }

  const ShaderCacheEntry* Find(uint64_t ucode_hash, uint64_t loose_hash) const {
    return Find(ucode_hash, loose_hash, 0);
  }

  // The domain stage form of a vertex shader, recompiled a second time under
  // the container's hash xored with the tag. Absent unless the shader is in
  // tools/shaders/domain-shaders.txt.
  const ShaderCacheEntry* FindDomain(uint64_t ucode_hash, uint64_t loose_hash) const {
    return Find(ucode_hash, loose_hash, RENUT_DOMAIN_SHADER_HASH_TAG);
  }

  CompiledShader* Build(RenderDevice* device, const ShaderCacheEntry* entry, bool vertex, bool fetch_spec = false) {
    CompiledShader& compiled = compiled_[entry];
    if (compiled.module || compiled.failed) return compiled.failed ? nullptr : &compiled;
    return BuildModules(device, entry, vertex, fetch_spec, compiled) ? &compiled : nullptr;
  }

  // The entry of a container hash (ShaderCacheEntry::hash), domain forms included.
  const ShaderCacheEntry* FindContainer(uint64_t hash) const {
    auto entry = by_container_.find(hash);
    return entry == by_container_.end() ? nullptr : entry->second;
  }

  // Makes a shader's modules into `compiled`. Also used on their own by the
  // pipeline precompilation, whose threads must not touch the store: the
  // driver keys its cache by the SPIR-V, so the same transformations give
  // pipelines the draws find there.
  static bool BuildModules(RenderDevice* device, const ShaderCacheEntry* entry, bool vertex, bool fetch_spec,
                           CompiledShader& compiled) {
    const uint8_t* data = g_spirvCache + entry->spirvOffset;
    size_t size = smolv::GetDecodedBufferSize(data, entry->spirvSize);
    std::vector<uint32_t> spirv((size + 3) / 4);
    if (!size || !smolv::Decode(data, entry->spirvSize, spirv.data(), size)) {
      compiled.failed = true;
      return false;
    }
    // Debug: <files>/plume-spv/<container hash>.spv stands in for the cached
    // SPIR-V of that shader, to try a changed shader without rebuilding the cache.
    if (!g_miss_dump_directory.empty()) {
      const std::string path = fmt::format("{}/plume-spv/{:016X}.spv",
                                           g_miss_dump_directory.substr(0, g_miss_dump_directory.rfind('/')),
                                           entry->hash);
      if (FILE* file = std::fopen(path.c_str(), "rb")) {
        std::fseek(file, 0, SEEK_END);
        const long bytes = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        if (bytes > 0 && bytes % 4 == 0) {
          spirv.assign(size_t(bytes) / 4, 0);
          if (std::fread(spirv.data(), 1, size_t(bytes), file) == size_t(bytes)) {
            size = size_t(bytes);
            REXLOG_INFO("Renut: Plume shader {:016X} replaced by {} ({} bytes)", entry->hash, path, bytes);
          }
        }
        std::fclose(file);
      }
    }
    // The boolean registers from the shared block (set 4, binding 8, where the
    // host writes them every draw) instead of specialization constants: one
    // pipeline for every combination of them a scene shows was most of what a
    // first visit compiled. A module of another shape keeps them specialized.
    if (REXCVAR_GET(renut_plume_bool_spec) == 0) {
      std::vector<uint32_t> dynamic = renut::MakeBooleansDynamic(spirv, 4, 8, RENUT_SHARED_BOOLEANS_OFFSET / 16,
                                                                 RENUT_SPEC_BOOLS_BASE);
      if (!dynamic.empty()) {
        spirv = std::move(dynamic);
        size = spirv.size() * 4;
        compiled.dynamic_bools = true;
      }
    }
    compiled.module = device->createShader(spirv.data(), size, "main", RenderShaderFormat::SPIRV);
    if (!compiled.module) {
      compiled.failed = true;
      return false;
    }
    if (vertex) compiled.inputs = ReflectInputs(spirv);
    std::vector<uint32_t> specialized;
    if (fetch_spec) {
      // The table starts after the loop constants in the cold block (set 4,
      // binding 9), whose first entry sits right after the hot block.
      specialized = renut::SpecializeFetchTable(
          spirv, 4, 9, (RENUT_SHARED_FETCH_OFFSET - RENUT_SHARED_HOT_SIZE) / 16, RENUT_LIVE_FETCH_COUNT,
          kFetchSpecBase);
      if (!specialized.empty()) {
        compiled.fetch_spec_module =
            device->createShader(specialized.data(), specialized.size() * 4, "main", RenderShaderFormat::SPIRV);
      }
    }
    if (vertex) {
      std::vector<uint32_t> stripped = renut::StripVertexOutputs(spirv);
      if (!stripped.empty()) {
        compiled.depth_module =
            device->createShader(stripped.data(), stripped.size() * 4, "main", RenderShaderFormat::SPIRV);
        if (!specialized.empty()) {
          std::vector<uint32_t> both = renut::StripVertexOutputs(specialized);
          if (!both.empty()) {
            compiled.depth_fetch_spec_module =
                device->createShader(both.data(), both.size() * 4, "main", RenderShaderFormat::SPIRV);
          }
        }
      }
    }
    return true;
  }

 private:
  const ShaderCacheEntry* Find(uint64_t ucode_hash, uint64_t loose_hash, uint64_t tag) const {
    if (const ShaderCacheEntry* entry = Lookup(by_ucode_, ucode_hash, tag)) return entry;
    return Lookup(by_loose_, loose_hash, tag);
  }

  const ShaderCacheEntry* Lookup(const std::unordered_map<uint64_t, uint64_t>& by_key, uint64_t key,
                                 uint64_t tag) const {
    auto container = by_key.find(key);
    if (container == by_key.end()) return nullptr;
    auto entry = by_container_.find(container->second ^ tag);
    return entry == by_container_.end() ? nullptr : entry->second;
  }

  std::unordered_map<uint64_t, const ShaderCacheEntry*> by_container_;
  std::unordered_map<uint64_t, uint64_t> by_ucode_;
  std::unordered_map<uint64_t, uint64_t> by_loose_;
  std::unordered_map<const ShaderCacheEntry*, CompiledShader> compiled_;
};

// ---------------------------------------------------------------------------
// Pipeline recipes
// ---------------------------------------------------------------------------

// Every pipeline a draw asks for is appended to <files>/plume-recipes.bin, its
// shaders named by container hash, and the next start builds them all before a
// scene may load - UnleashedRecomp's method: it holds its loading screen until
// the pipelines it predicted are compiled, so a scene is never seen before they
// exist. N&B's engine is not known well enough to predict them, so what the
// game used before stands in for the prediction. The file is not part of the
// driver's cache, which "Apagar cache de shaders" deletes; this list survives it.

// Bump when what a recipe holds changes: the file is then started again.
constexpr uint32_t kRecipeVersion = 1;
constexpr uint32_t kRecipeMagic = 0x52504E52;  // "RNPR"

struct RecipeHeader {
  uint32_t magic = kRecipeMagic;
  uint32_t version = kRecipeVersion;
};

// Which of a vertex shader's modules (CompiledShader) the pipeline was made with.
enum : uint8_t { kRecipeModule, kRecipeFetchSpec, kRecipeDepth, kRecipeDepthFetchSpec };

struct Recipe {
  uint64_t vs = 0;     // container hash of the vertex shader, or of its domain form
  uint64_t ps = 0;     // 0: no fragment shader
  uint8_t domain = 0;  // vs is a domain form (built without vertex inputs)
  uint8_t form = kRecipeModule;
  uint8_t gs = 0;      // 0 none, 1 rectangle list, 2 point list
  RenderGraphicsPipelineDesc desc;  // shader, layout and array pointers unset
  std::vector<RenderInputElement> elements;
  std::vector<RenderInputSlot> slots;
  std::vector<RenderSpecConstant> constants;
};

struct RecipeWriter {
  static constexpr bool kReading = false;
  std::string& out;
  template <typename T>
  void operator()(T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
  }
};

struct RecipeReader {
  static constexpr bool kReading = true;
  const uint8_t* at;
  const uint8_t* end;
  bool ok = true;
  template <typename T>
  void operator()(T& value) {
    if (size_t(end - at) < sizeof(T)) {
      ok = false;
      return;
    }
    std::memcpy(&value, at, sizeof(T));
    at += sizeof(T);
  }
};

// Field by field rather than the desc's bytes: its padding is not zeroed, and
// the bytes written are also the key a draw finds its built pipeline by.
template <typename Io>
bool VisitRecipe(Io& io, Recipe& r) {
  io(r.vs);
  io(r.ps);
  io(r.domain);
  io(r.form);
  io(r.gs);
  RenderGraphicsPipelineDesc& d = r.desc;
  io(d.depthFunction);
  io(d.depthClipEnabled);
  io(d.depthBias);
  io(d.depthBiasClamp);
  io(d.slopeScaledDepthBias);
  io(d.dynamicDepthBiasEnabled);
  io(d.depthEnabled);
  io(d.depthWriteEnabled);
  io(d.stencilEnabled);
  io(d.stencilReadMask);
  io(d.stencilWriteMask);
  io(d.stencilReference);
  for (RenderStencilFaceDesc* face : {&d.stencilFrontFace, &d.stencilBackFace}) {
    io(face->passOp);
    io(face->failOp);
    io(face->depthFailOp);
    io(face->compareFunction);
  }
  io(d.multisampling.sampleCount);
  io(d.multisampling.sampleLocationsEnabled);
  for (RenderMultisamplingLocation& location : d.multisampling.sampleLocations) {
    io(location.x);
    io(location.y);
  }
  io(d.alphaToCoverageEnabled);
  io(d.primitiveTopology);
  io(d.cullMode);
  io(d.frontFace);
  io(d.renderTargetCount);
  if (d.renderTargetCount > RenderGraphicsPipelineDesc::MaxRenderTargets) return false;
  for (uint32_t i = 0; i < d.renderTargetCount; ++i) {
    io(d.renderTargetFormat[i]);
    RenderBlendDesc& b = d.renderTargetBlend[i];
    io(b.blendEnabled);
    io(b.srcBlend);
    io(b.dstBlend);
    io(b.blendOp);
    io(b.srcBlendAlpha);
    io(b.dstBlendAlpha);
    io(b.blendOpAlpha);
    io(b.renderTargetWriteMask);
  }
  io(d.logicOpEnabled);
  io(d.logicOp);
  io(d.depthTargetFormat);
  auto array = [&](auto& items, uint32_t limit, auto&& each) {
    uint32_t count = uint32_t(items.size());
    io(count);
    if constexpr (Io::kReading) {
      if (!io.ok || count > limit) return false;
      items.resize(count);
    }
    for (auto& item : items) each(item);
    return true;
  };
  if (!array(r.elements, 64, [&](RenderInputElement& e) {
        io(e.semanticIndex);
        io(e.location);
        io(e.format);
        io(e.slotIndex);
        io(e.alignedByteOffset);
      })) {
    return false;
  }
  if (!array(r.slots, 64, [&](RenderInputSlot& s) {
        io(s.index);
        io(s.stride);
        io(s.classification);
      })) {
    return false;
  }
  if (!array(r.constants, 1024, [&](RenderSpecConstant& c) {
        io(c.index);
        io(c.value);
      })) {
    return false;
  }
  if constexpr (Io::kReading) return io.ok;
  return true;
}

// What the driver compiles for a recipe: recipes that share it differ only in
// state it links (Turnip's cache is keyed by shaders and specialization), so
// one compile serves them all and the rest are found in the cache.
uint64_t RecipeProgram(const Recipe& r);

// A scene tag in the file: a record whose size has this bit set holds the
// recipe's key (64 bits) and the scene it was drawn in (32) instead of a recipe.
constexpr uint32_t kRecipeTagBit = 0x80000000u;
constexpr uint32_t kRecipeTagSize = 12;

inline uint64_t SceneTagKey(uint64_t recipe, uint32_t scene) {
  uint64_t key = recipe;
  HashCombine(key, scene);
  return key;
}

// The precompilation, shared by its threads and the loading-screen hold
// (PlumeSceneRequested / PlumeSceneReady, driven by game_fixes.cpp). A group is
// the recipes of one program; a scene's groups are those its pipelines are in.
struct PrecompileState {
  enum : uint8_t { kPending, kTaken, kDone };
  std::mutex mutex;
  std::condition_variable changed;
  // Under mutex: each group's state and size in recipes, the groups a scene
  // load asked for first, the next group in file order, and each scene's groups.
  std::vector<uint8_t> state;
  std::vector<uint32_t> group_size;
  std::deque<uint32_t> urgent, background;
  std::unordered_map<uint32_t, std::vector<uint32_t>> scene_groups;
  // Under mutex: the groups of the scene being loaded that are not built yet,
  // how many recipes it has and has left, when it was asked for and when one
  // of its groups was last finished.
  std::vector<uint32_t> scene_missing;
  uint32_t scene_total = 0, scene_left = 0;
  std::chrono::steady_clock::time_point scene_asked, scene_moved;
  std::atomic<uint32_t> done{0}, total{0};
  std::atomic<bool> running{false};
  // From a scene request until the game's loading screen lets go: the threads
  // build at full speed then, behind the game otherwise (UnleashedRecomp
  // raises its compile threads while loading and lowers them in play).
  std::atomic<bool> loading{false};
  // The threads run and take predicted programs (StartRecipes).
  std::atomic<bool> predicting{false};
  // Programs a draw made a new pipeline of, and how many of them had been
  // predicted or recorded ahead (TakeRecipe), for the log.
  std::atomic<uint32_t> draw_programs{0}, draw_programs_ahead{0};
  // The scene the game asked for last (0 before the first: the boot); the
  // pipelines drawn are tagged with it.
  std::atomic<uint32_t> scene{0};
  // Under mutex: every recipe to build and the groups of their indices (deques:
  // a group added while the threads run moves nothing), whether a group is
  // built only to fill the driver's cache (a predicted program: no draw asks
  // for the fixed state it was built with, so its pipeline is dropped), the
  // programs already in a group, the threads building one, and the batch the
  // threads are on (when it started, how many failed).
  std::deque<std::pair<uint64_t, Recipe>> recipes;
  std::deque<std::vector<uint32_t>> groups;
  std::vector<uint8_t> warm_only;
  std::unordered_set<uint64_t> programs;
  uint32_t busy = 0;
  std::chrono::steady_clock::time_point batch_start;
  uint32_t batch_failed = 0;

  // Under mutex: a group of one program's recipes. Returns its index.
  uint32_t AddGroup(std::vector<std::pair<uint64_t, Recipe>>&& items, bool warm) {
    const uint32_t g = uint32_t(groups.size());
    groups.emplace_back();
    for (auto& item : items) {
      groups.back().push_back(uint32_t(recipes.size()));
      recipes.push_back(std::move(item));
    }
    state.push_back(kPending);
    group_size.push_back(uint32_t(groups.back().size()));
    warm_only.push_back(warm);
    total += group_size.back();
    if (!running) {
      running = true;
      batch_start = std::chrono::steady_clock::now();
      batch_failed = 0;
    }
    return g;
  }

  // Under mutex. What a scene being loaded asks for (urgent) - its recorded
  // groups and those predicted from the models it loads - and, for the one
  // thread that works during play (`play`), the programs of models streamed in
  // meanwhile (background). The rest of the recipe list waits for the scene
  // that needs it: built all at start, it took the game's CPU for minutes of
  // the first play. UnleashedRecomp likewise compiles only what the level
  // being loaded needs. UINT32_MAX when there is nothing to take.
  uint32_t TakeGroup(bool play) {
    for (std::deque<uint32_t>* queue : {&urgent, play ? &background : nullptr}) {
      if (!queue) continue;
      while (!queue->empty()) {
        const uint32_t g = queue->front();
        queue->pop_front();
        if (state[g] == kPending) {
          state[g] = kTaken;
          return g;
        }
      }
    }
    return UINT32_MAX;
  }
};
inline PrecompileState g_precompile;

// Shader pairs of loaded models waiting for the command thread
// (PlumePredictPipelines -> PredictPipelines); `queued` counts them until
// they are made into recipes, and a scene does not start while it is not 0.
struct PredictQueue {
  std::mutex mutex;
  std::vector<PlumeShaderPair> pairs;
  std::atomic<uint32_t> queued{0};
};
inline PredictQueue g_predict;

// Decodes the vertex layout a bound vertex shader really fetches. Direct3D
// Which texture fetch constants a shader reads, as a bit per slot. The draw
// path used to ask every one of the 32 slots for its texture, sampler and
// resolved target, on every draw; a shader samples two or three. The constant a
// tfetch names sits in the same bits as a vfetch's, and Direct3D does not
// rewrite it.
uint32_t TextureMask(const gfx::Shader& shader);

// patched the vfetch instructions for the current vertex declaration; the
// recompiler recorded which instruction reads each element (fetchAddress).
//
// Direct3D patches the destination swizzle as well - .yxwz for a 16-bit
// element, which puts back the component order the k8in32 swap reversed,
// .zyxw for a D3DCOLOR, and the constants 0 and 1 for components the vertex
// declaration does not have - and a shader the store found by its loose key
// was recompiled from microcode without that patch (the containers extracted
// from the game's bundles). Such a shader read the halves of every 16-bit pair
// exchanged: the falling leaves' spin axis came out of unit length, so their
// rotation quaternion grew each frame and the leaves with it, into sheets
// across the screen. Colours came out with red and blue exchanged, and alpha
// from memory where the hardware reads 1.
//
// The recompiler records the swizzle it compiled each fetch with (dstSwizzle).
// Every register lane the recompiled code fills from fetched component c the
// live code fills from component l, or with a constant, so component c is
// remapped to l before the recompiled swizzle reads it. Returns the remap as
// the shaders store it (xored with the identity, 0 for none), or ~0u when two
// lanes want one component two ways.
uint32_t FetchRemap(uint32_t compiled, uint32_t live);

void DecodeLiveElements(HostShader& shader, const ShaderCacheEntry* entry,
                        std::vector<LiveElement>& out_elements,
                        std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>>& out_fetches);

void DecodeLiveElements(HostShader& shader);

// ---------------------------------------------------------------------------
// Format and state translation
// ---------------------------------------------------------------------------

// A pipeline the driver would not build: Plume still hands back the object, with
// a null Vulkan handle, and binding that hangs some drivers. The callers tell
// failure from success by the pointer.
std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(RenderDevice* device, const RenderGraphicsPipelineDesc& desc);

// A texture's subresource into a readback buffer, `row_texels` texels to a row.
// Plume's copyTextureRegion goes the other way only, so this is Vulkan on the
// command buffer itself; the texture is in COPY_SOURCE (transfer source).
void CopyTextureToBuffer(RenderCommandList* list, RenderBuffer* buffer, RenderTexture* texture, uint32_t width,
                         uint32_t height, uint32_t row_texels, uint32_t mip, uint32_t layer, const RenderBox* box);

RenderFormat ColorFormat(xenos::ColorRenderTargetFormat format);

bool IsBlendable(RenderFormat format);

// How a vertex element reaches the recompiled shader, whose inputs are all
// float4 like the hardware's vfetch results.
enum class InputDecode { kNone, kInteger, kPacked1010102, kPacked111110 };

// The host format for a guest vertex format, and what unpackInput() (or
// tfetchR11G11B10) must still do in the shader: integers read as numbers
// arrive as raw bits, and the packed formats have no host equivalent.
//
// Plume has no names for the converted-to-float formats a vertex shader that
// takes float inputs needs (UBYTE4, SHORT2, SHORT4 as numbers, and the packed
// 2_10_10_10), so they are asked for as the integer formats of the same width
// and the Vulkan layer (plume_renderer.cpp, LayerVertexFormat) creates the
// pipeline with the converting one: the 8 and 16-bit UINT/SINT formats become
// USCALED/SSCALED, and the two 8-bit two-channel formats nothing else uses as an
// input stand for the 2_10_10_10 ones.
constexpr RenderFormat kVertexUnorm1010102 = RenderFormat::R8G8_UINT;
constexpr RenderFormat kVertexUscaled1010102 = RenderFormat::R8G8_SINT;

RenderFormat VertexFormat(uint32_t format, bool is_signed, bool normalized, InputDecode& decode);

bool IsSixteenBitFormat(uint32_t format);

// Xenos k_11_11_10 (vertex format 17) keeps X in the low 10 bits and Y and Z
// in 11 bits each; k_10_11_11 (16) is the other way round (X and Y 11 bits, Z
// 10). tfetchR11G11B10 in the recompiled shaders decodes only the second
// layout, so a format 17 vector - the ground decals' tangent and binormal -
// came out a different, non-unit vector for every small change of its bits,
// and the tread ribbon's relief and light jumped from quad to quad. Move the
// same vector into the layout the shader reads.
uint32_t RepackFormat17(uint32_t word);

RenderBlend Blend(xenos::BlendFactor factor);

RenderBlendOperation BlendOp(xenos::BlendOp op);

RenderComparisonFunction Compare(xenos::CompareFunction f);

// ---------------------------------------------------------------------------
// Command processor
// ---------------------------------------------------------------------------

struct Target {
  std::unique_ptr<RenderTexture> texture;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  bool depth = false;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // Serial of the last draw that rendered to it.
  uint64_t last_used = 0;
  // The D3DSurface it is, its RB_COLOR_INFO / RB_DEPTH_INFO, and the frame it
  // was last used in.
  uint32_t surface = 0, surface_info = 0;
  uint64_t used_frame = 0;
  // Quarters of the guest's resolution the host draws it at: 4 is the guest's
  // size, less is a shadow map under the shadow quality option (width and
  // height are host pixels; the guest's viewport, scissor, clears and resolve
  // rectangles are scaled by this).
  uint32_t scale = 4;
  // A depth resolve took this target's image for its texture (AliasDepthResolve):
  // the key of the resolve holding it and the image itself, so a draw that uses
  // the surface again without clearing all of it gets the contents copied back.
  uint64_t restore_key = 0;
  const RenderTexture* restore_image = nullptr;
  // How often this surface was resolved in place and then cleared whole before
  // anything else used it (the pattern of a shadow map), and whether it is
  // between the two now. Two surfaces that alternate into one texture need their
  // old contents copied back only until the pattern has been seen twice;
  // after that the contents are not worth a copy a frame (depth_undefined).
  uint32_t alias_cycles = 0;
  bool aliased_since_clear = false;
  bool depth_undefined = false;
};

// A guest pixel coordinate in a target's host pixels, rounded down or up.
inline uint32_t HostFloor(const Target* target, uint32_t value) {
  return target ? uint32_t(uint64_t(value) * target->scale / 4) : value;
}
inline uint32_t HostCeil(const Target* target, uint32_t value) {
  return target ? uint32_t((uint64_t(value) * target->scale + 3) / 4) : value;
}

// Keys of the render targets: one per surface (see SurfaceTarget).
constexpr uint64_t kSurfaceTargetKey = 1ull << 63;

// A native resolve's source and the depth its clear goes to, by surface, and
// the rectangle in the source's pixels.
struct ResolveSurfaces {
  Target* source = nullptr;
  Target* depth = nullptr;
  uint32_t x = 0, y = 0, width = 0, height = 0;
  // The destination texture object's level 0 (GPU address, 0 if the copy goes
  // elsewhere), its size and pitch, and where the source corner lands in it.
  uint32_t object_base = 0, object_width = 0, object_height = 0, object_pitch = 0;
  int32_t dest_dx = 0, dest_dy = 0;
};

struct Resolved {
  std::unique_ptr<RenderTexture> texture;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t descriptor = 0;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // Bumped by every resolve into this texture.
  uint64_t generation = 0;
  // Serial of the last resolve into it (see FindResolved).
  uint64_t written = 0;
  // The guest memory under it when last resolved. Resolves are not written
  // back to guest memory, so if these bytes change the CPU has put something
  // else there (a texture streamed into freed render memory) and this image
  // is stale.
  uint64_t guest_hash = 0;
  uint32_t guest_bytes = 0;
  uint32_t guest_pitch = 0, guest_height = 0, guest_bpp = 0;
  uint64_t written_frame = ~0ull;
  uint64_t checked_frame = ~0ull;
  // The texture is sized to the copy's aligned extent (720 rows become 736),
  // but a fetch normalizes coordinates to its own size, so a fetch smaller
  // than the texture samples this copy of its top-left corner instead.
  std::unique_ptr<RenderTexture> fitted;
  uint32_t fitted_width = 0, fitted_height = 0, fitted_descriptor = 0;
  uint64_t fitted_generation = UINT64_MAX;
  RenderTextureLayout fitted_layout = RenderTextureLayout::UNKNOWN;
  // A depth resolve (R32_FLOAT, depth in x) seen through the fetch
  // constant's swizzle, keyed by swizzle | fitted << 12. A Xenos depth
  // texture returns the depth in every channel; the game's depth restore
  // reads it as .w, which an R32_FLOAT view gives as 1.
  struct SwizzledView {
    const RenderTexture* texture = nullptr;
    std::unique_ptr<RenderTextureView> view;
    uint32_t descriptor = 0;
  };
  std::map<uint32_t, SwizzledView> swizzled;
  // Last written by a depth resolve.
  bool depth = false;
  // A draw has fetched this resolve (or it was presented). One that nothing ever samples is a
  // copy the CPU reads, like Photo Mode's picture (see ReadbackResolve).
  bool sampled = false;
  // Quarters of the guest's resolution it was resolved at (Target::scale of
  // the surface it came from): width and height are host pixels.
  uint32_t scale = 4;
  // `texture` is a depth target's own image, not a copy of it: a depth map
  // resolved whole is sampled in place (AliasDepthResolve). `format` is then the
  // depth format, and the views and the fitted copy are made accordingly.
  bool alias = false;
};

struct GuestTexture {
  std::unique_ptr<RenderTexture> texture;
  uint64_t checked_frame = ~0ull;
  uint32_t width = 0, height = 0;
  std::unique_ptr<RenderTextureView> view;  // cube maps
  uint32_t descriptor = 0;
  uint32_t levels = 1, depth = 1;
  uint64_t content = 0;
  bool failed = false;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // 2D textures: the guest memory under the base levels and under the mips,
  // write-watched while `watched` (see GetGuestTexture).
  std::array<uint32_t, 2> watch_begin{}, watch_end{};
  bool watched = false;
  // 2D textures: the block format the host image holds (kNone: RGBA8 decoded on
  // the CPU), and whether the compressed upload was tried and did not fit.
  BcKind bc = BcKind::kNone;
  bool bc_blocked = false;
  // Guest mips below the host image's top level (max dimension and quality).
  uint32_t skip = 0;
};

// Bytes a 2D guest texture takes on the host, its mip chain included.
inline uint64_t HostTextureBytes(const GuestTexture& t) {
  const uint64_t base = t.bc != BcKind::kNone
                            ? uint64_t((t.width + 3) / 4) * ((t.height + 3) / 4) * BcBlockBytes(t.bc)
                            : uint64_t(t.width) * t.height * 4;
  return t.levels > 1 ? base * 4 / 3 : base;
}

uint64_t ContentHash(const uint8_t* data, uint32_t span);

// Bytes per element of a memory export format, 0 for the ones renutMemExport
// does not store (narrower than a dword).
uint32_t ExportElementBytes(uint32_t format);

struct Stats {
  uint64_t textures_decoded = 0, texture_failures = 0, textures_bc = 0;
  uint64_t draws = 0, drawn = 0, no_vs = 0, no_ps = 0, bad_shader = 0, skipped_prim = 0,
           no_target = 0, resolves = 0, clears = 0, pipelines = 0, overflow = 0;
  // Render passes begun. A tiler resolves and reloads the whole target at each
  // one, so this says how much of the frame is spent moving tile memory.
  uint64_t passes = 0;
  // Render passes ended early by a layout change, by destination layout.
  std::array<uint64_t, 8> split_by{};
  // vkCmdPipelineBarrier calls for texture layouts, each a wait for idle on Turnip.
  uint64_t barriers = 0;
  // Per-draw GPU work: pipeline changes, vertices submitted, and the draws
  // that take a geometry shader (rectangle lists) or are CPU-tessellated
  // patches (the water) and the vertices those patches expand to.
  uint64_t pipeline_binds = 0, vertices = 0, gs_draws = 0, patch_draws = 0, patch_vertices = 0;
  uint64_t adaptive_patch_draws = 0;  // patch draws laid out on the GPU (vertices not counted)
  // Wall clock, in microseconds: whole frames, the pipelines built inside them
  // and the submit that ends them. What is left is the rest of the draw path.
  uint64_t frame_us = 0, pipeline_us = 0, submit_us = 0;
  // Constant blocks a draw found already in the ring, out of three per draw.
  uint64_t constants_reused = 0;
  // Memory export: draws that export, ranges created, streams read from an
  // exported range whole or pieced together over guest memory, ranges the
  // CPU wrote into again, and the barriers ordering all of that.
  uint64_t export_draws = 0, export_ranges = 0, export_reads = 0, export_merges = 0,
           export_refreshes = 0, export_barriers = 0;
  // Whether the frame is bound by this thread or by the game: the CPU time the
  // command processor's thread actually ran (not wall clock), the part of it
  // spent waiting for the GPU to hand a frame slot back, and the bytes it
  // copied into the ring - guest vertex and index streams, constant blocks.
  uint64_t cp_cpu_us = 0, slot_wait_us = 0, stream_bytes = 0, constant_bytes = 0;
  // Streams served from a kept buffer, bytes copied into new kept buffers, and
  // kept streams the guest wrote over.
  uint64_t kept_hits = 0, kept_bytes_made = 0, kept_drops = 0, kept_unwatchable = 0;
  uint64_t band_skipped = 0, band_moved = 0, band_widened = 0;
  // Draws that waited in a window, the windows, and the pipeline changes
  // those windows had in game order against the order they were recorded in.
  uint64_t deferred_draws = 0, defer_windows = 0, defer_binds_before = 0, defer_binds_after = 0;
  uint64_t shadow_culled = 0;  // plume-shadowmin
  // Depth resolves that handed their image over instead of copying it, the
  // ones whose surface was used again and got the contents copied back, and
  // the ones a full clear made unnecessary.
  uint64_t depth_aliased = 0, depth_restored = 0, depth_dropped = 0, depth_abandoned = 0;
  uint64_t twin_skipped = 0;   // MSAA off: per-band copies of a draw already drawn
  // New pipelines by how they were made: found in the driver's cache at the
  // draw, found by a worker, or compiled (and of those, how many waited for a
  // sibling's compile of the same program); the lookups at the draw that
  // missed; and the time spent on lookups at the draw and on compiles.
  uint64_t pipelines_found_here = 0, pipelines_found_ahead = 0, pipelines_compiled = 0, pipelines_waited = 0,
           lookups_missed = 0, lookup_us = 0, compile_us = 0;
  // New pipelines the precompilation had built before their draw came.
  uint64_t pipelines_prebuilt = 0;
  // Hi-Z (hiz_model.h): colour/depth-only draws looked at, the ones with a
  // model and a box, the ones that could not be dropped for lack of either,
  // and the ones whose box lay wholly outside the frustum or the scissor.
  uint64_t hiz_seen[2] = {}, hiz_eligible[2] = {}, hiz_no_model[2] = {}, hiz_no_box[2] = {};
  uint64_t hiz_frustum[2] = {}, hiz_scissor[2] = {}, hiz_skipped[2] = {};
  uint64_t hiz_boxes_made = 0, hiz_box_us = 0, hiz_box_hits = 0, hiz_models = 0, hiz_models_valid = 0,
           hiz_model_rejected = 0, hiz_model_us = 0;
  uint64_t hiz_culled_total = 0;
  // Windows closed, and the draws they held, by reason: 0 elsewhere, 1 state change, 2 full, 3 an immediate
  // draw, 4 something the draw itself needed (a texture, a barrier).
  uint64_t flush_windows[8] = {}, flush_draws[8] = {};
  uint64_t hiz_boxed_draws = 0, hiz_why_not[4] = {}, hiz_small_windows = 0, hiz_small_draws = 0, hiz_windows_all = 0;
  uint64_t hiz_cull_ns = 0;
  uint64_t stream_memo_hits = 0;
  uint64_t hiz_stage_ns[4] = {};  // model lookup, box lookup, constants evaluation, plane/scissor tests (sampled, scaled)
  // Draws with no model by why (renut::hiz::Why), depth-only [0] and colour [1].
  uint64_t hiz_why_model[2][12] = {};
  uint64_t hiz_verified = 0, hiz_violations = 0, hiz_missed_hidden = 0, hiz_missed_empty = 0;
  uint64_t hiz_windows_tested = 0, hiz_records = 0, hiz_held = 0, hiz_test_failed = 0, hiz_answered = 0,
           hiz_occluded = 0, hiz_held_skipped = 0, hiz_held_visible = 0, hiz_predicated_draws = 0;
  // Why a draw was not considered: no indices, primitive, memory export, clip/viewport state, constant base.
  uint64_t hiz_why[5] = {};
};

// The GPU as the app hosts it: no SDK graphics system or command processor
// stands behind it. The game's Direct3D records lists (native_d3d.cpp) and
// this runs them on its own worker thread; what the SDK's command processor
// kept for us - the register file the CPU sees, the ring's write pointer and
// read-pointer write-back, the scratch-register write-back, the vertical
// blank counter - is the few members below.

class PlumeCommandProcessor final {
 public:
  PlumeCommandProcessor(rex::memory::Memory* memory, rex::system::KernelState* kernel,
                        std::function<void(uint32_t, uint32_t)> dispatch_interrupt)
      : memory_(memory),
        kernel_state_(kernel),
        dispatch_interrupt_(std::move(dispatch_interrupt)),
        worker_running_(true),
        write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
        write_ptr_index_(0) {
    register_file_ = &registers_;
  }

  // Starts the worker ("GPU Commands"): SetupContext runs on it, then the loop.
  bool Initialize() {
    worker_running_ = true;
    worker_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          if (!SetupContext()) {
            rex::FatalError("Unable to setup command processor internal state");
            return 0;
          }
          RunWorkerLoop();
          ShutdownContext();
          return 0;
        }));
    worker_thread_->set_name("GPU Commands");
    worker_thread_->Create();
    return true;
  }
  void Shutdown() {
    worker_running_ = false;
    write_ptr_index_event_->Set();
    if (worker_thread_) {
      worker_thread_->Wait(0, 0, 0, nullptr);
      worker_thread_.reset();
    }
  }
  // The ring the game's Direct3D sets up. It carries nothing the lists do not
  // (a write pointer that moves is consumed and the read pointer written
  // back), but Direct3D reads that pointer to know there is room.
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    read_ptr_index_ = 0;
    primary_buffer_ptr_ = ptr;
    primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t) { read_ptr_writeback_ptr_ = ptr; }
  void UpdateWritePointer(uint32_t value) {
    write_ptr_index_ = value;
    write_ptr_index_event_->Set();
  }
  uint32_t counter() const { return counter_; }
  void increment_counter() { counter_++; }
  gfx::RegisterFile* registers() { return &registers_; }
  gfx::Shader* active_vertex_shader() const { return active_vertex_shader_; }
  gfx::Shader* active_pixel_shader() const { return active_pixel_shader_; }
  uint32_t ReadRegisterValue(uint32_t index) const {
    if (index < gfx::RegisterFile::kRegisterCount) return registers_.values[index];
    auto it = extended_register_values_.find(index);
    return it != extended_register_values_.end() ? it->second : 0;
  }

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };
  rex::memory::Memory* memory_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  std::function<void(uint32_t, uint32_t)> dispatch_interrupt_;
  // The GPU's registers as the CPU sees them. register_file_ points here, or at
  // the lists' own file while they run (NativeSwapIn/NativeSwapOut).
  gfx::RegisterFile registers_;
  gfx::RegisterFile* register_file_ = nullptr;
  std::atomic<bool> worker_running_;
  rex::system::object_ref<rex::system::XHostThread> worker_thread_;
  std::queue<std::function<void()>> pending_fns_;
  uint32_t counter_ = 0;
  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;
  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;
  std::unique_ptr<rex::thread::Event> write_ptr_index_event_;
  std::atomic<uint32_t> write_ptr_index_;
  std::unordered_map<uint32_t, uint32_t> extended_register_values_;
  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  gfx::Shader* active_vertex_shader_ = nullptr;
  gfx::Shader* active_pixel_shader_ = nullptr;

  bool SetupContext();
  void ShutdownContext();
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height);
  gfx::Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                          const uint32_t* host_address, uint32_t dword_count);
  // Register writes. The shader constants (ALU, fetch, boolean and loop,
  // 0x4000-0x4927) have no side effects in the base class, and the guest
  // writes thousands of them a frame through one virtual call each: those
  // ranges are swapped in bulk here, and every write into a stage's float
  // constants marks that stage's block as needing a look (CommitConstants).
  void WriteRegister(uint32_t index, uint32_t value);
  // Marks the constant blocks (kConstantChunks per stage, each bound on its
  // own - see set 4 in shader_common.h) that a write lands in.
  void MarkConstantsWritten(uint32_t first, uint32_t count) {
    // The two files are contiguous (vertex c0 at 0x4000, pixel c0 at 0x4400)
    // and so are the flags, one per 256 registers: the block is the offset
    // shifted. This runs for every constant register the guest writes.
    static_assert(XE_GPU_REG_SHADER_CONSTANT_256_X - XE_GPU_REG_SHADER_CONSTANT_000_X ==
                  kConstantChunks * (kConstantChunkSize / 4));
    constexpr uint32_t kShift = 8;
    static_assert((1u << kShift) == kConstantChunkSize / 4);
    constexpr uint32_t kRegisters = 2 * kConstantChunks << kShift;
    if (first <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31 &&
        uint64_t(first) + count > XE_GPU_REG_SHADER_CONSTANT_LOOP_00) {
      loop_constants_dirty_ = true;
    }
    const uint32_t lo = first - XE_GPU_REG_SHADER_CONSTANT_000_X;
    if (first < XE_GPU_REG_SHADER_CONSTANT_000_X || lo >= kRegisters || !count) return;
    const uint32_t hi = std::min(lo + count, kRegisters) - 1;
    bool* written = &constants_written_[0][0];
    for (uint32_t block = lo >> kShift; block <= hi >> kShift; ++block) written[block] = true;
  }
  // A write of `count` registers from `values`: the registers that already hold
  // these bytes are left alone, and only the blocks a different one lands in are
  // marked. The guest sets the same view/projection/lighting constants before
  // draw after draw, and a marked block costs a compare of the whole 1 KB (and
  // an upload when it differs) at the next draw that reads it.
  void StoreConstants(uint32_t first, const uint32_t* values, uint32_t count) {
    uint32_t* dst = &register_file_->values[first];
    uint32_t lo = 0;
    while (lo < count && dst[lo] == values[lo]) ++lo;
    if (lo == count) return;
    uint32_t hi = count;
    while (dst[hi - 1] == values[hi - 1]) --hi;
    std::memcpy(dst + lo, values + lo, size_t(hi - lo) * sizeof(uint32_t));
    MarkConstantsWritten(first + lo, hi - lo);
  }

  static constexpr uint32_t kConstantChunks = 4;
  static constexpr uint32_t kConstantChunkSize = 1024;
  bool constants_written_[2][kConstantChunks] = {{true, true, true, true}, {true, true, true, true}};
  // A draw left out for a reason no other counter names: counted by reason
  // and shader for the periodic report, and named in a trace.
  bool Drop(const char* reason, const HostShader* vs, const HostShader* ps);
  // Debug: a frame trace lists the draws the bin check drops, and
  // <files>/plume-pred-all "1" runs every predicated packet.
  bool OnPredicatedSkip(uint32_t opcode) {
    if (trace_ && (opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2)) {
      std::fprintf(trace_, "predicated %s op=%X select=%llX mask=%llX\n", pred_pass_all_ ? "passed" : "skip",
                   opcode, (unsigned long long)bin_select_, (unsigned long long)bin_mask_);
    }
    return pred_pass_all_;
  }
  // ---- Native Direct3D lists ----
  // All drawing comes from the front end's lists (native_d3d.cpp), which run
  // with a register file of their own as the command processor reaches the
  // ring segments they go with. The ring's own register file is the GPU's as
  // the CPU sees it (scratch registers, the writeback the callbacks read).
  struct ExecutorBusy {
    ExecutorBusy() { native_d3d::SetExecutorBusy(true); }
    ~ExecutorBusy() { native_d3d::SetExecutorBusy(false); }
  };
  // Whether the draw being run came from a predicated packet: the draws come
  // from the native lists, which carry no such packet, so this stays false.
  bool packet_predicated_ = false;
  // The command processor's work loop. The SDK runs its own on the worker
  // thread it creates, which calls SetupContext() first: this one runs from
  // there until the processor is shut down, so the SDK's loop is never entered.
  // It does what the SDK's does and, around the ring packets, the native work.
  void RunWorkerLoop();
  // GetResolveInfo for a rectangle that is already known (NativeResolve).
  bool ResolveInfoForRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, draw_util::ResolveInfo& info);
  std::unique_ptr<RegisterFile> resolve_regs_;
  uint32_t resolve_vertices_ = 0;
  bool native_presenting_ = false;
  uint64_t native_fences_ = 0, native_presents_ = 0, native_callbacks_ = 0;
  uint64_t native_waits_ = 0, native_wait_timeouts_ = 0, native_wait_us_ = 0;
  // A dword the GPU writes back (big-endian), at a GPU address.
  void StoreBig(uint32_t physical, uint32_t value) {
    const uint32_t big = __builtin_bswap32(value);
    std::memcpy(memory_->TranslatePhysical<uint8_t*>(physical), &big, 4);
  }
  // The vertex buffer behind each stream and the index buffer (kOpStreams,
  // kOpIndices): a stream inside one is kept as that whole buffer.
  struct BufferRange {
    uint32_t base = 0, size = 0;
  };
  std::array<BufferRange, 18> native_streams_{};
  BufferRange native_indices_{};
  BufferRange stream_object_{};  // the hint GuestStream reads
  uint64_t object_streams_ = 0, window_streams_ = 0;
  bool native_checked_ = false;
  // ---- PM4-less: work that does not come through the ring ----
  // Lists the front end published are run by the worker itself.
  bool NativeWorkPending() { return native_d3d::WorkPending(); }
  void RunNativeWork() {
    native_d3d::TakeWorkPending();
    if (!native_checked_) {
      native_on_ = native_d3d::GetMode() == native_d3d::Mode::kNative;
      native_checked_ = true;
    }
    if (!native_on_) return;
    ExecutorBusy busy;
    RunNative(UINT32_MAX);
  }
  void NativeEffects(const uint32_t* pairs, uint32_t count);
  uint64_t native_effects_applied_ = 0;
  bool wake_registered_ = false;
  // Runs the published records until the end, or until the opaque-call
  // marker `until` (consumed; returns true), never past another marker.
  bool RunNative(uint32_t until);
  void NativeSwapIn();
  void NativeSwapOut();
  void NativeWrite(uint32_t first, const uint32_t* values, uint32_t count);
  void NativeDraw(uint32_t flags, uint32_t initiator, uint32_t dma_base, uint32_t dma_size);
  void NativeClear(const uint32_t* record);
  bool native_on_ = false;
  bool native_started_ = false;
  bool native_swapped_ = false;    // the lists' state is in register_file_ & co.
  std::unique_ptr<gfx::RegisterFile> native_regs_;
  gfx::RegisterFile* ring_regs_ = nullptr;
  gfx::Shader* native_vs_ = nullptr;
  gfx::Shader* native_ps_ = nullptr;
  gfx::Shader* ring_vs_ = nullptr;
  gfx::Shader* ring_ps_ = nullptr;
  uint64_t native_bin_mask_ = 0xFFFFFFFFull, native_bin_select_ = 0xFFFFFFFFull;
  uint64_t ring_bin_mask_ = 0, ring_bin_select_ = 0;
  struct NativeFrame {
    std::shared_ptr<const native_d3d::List> list;
    size_t pos = 0;
  };
  std::vector<NativeFrame> native_stack_;
  std::deque<std::shared_ptr<const native_d3d::List>> native_queue_;
  uint64_t native_draws_ = 0, native_ring_draws_dropped_ = 0, native_bad_ = 0;
  bool pred_pass_all_ = false;
  bool band_fragpos_ = false;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit);
  bool ExecuteResolve(const draw_util::ResolveInfo& info, const ResolveSurfaces& surfaces);
  // The picture Photo Mode takes: the game's capture routine (sub_82535F20, hooked in
  // guest/game_fixes.cpp) names the resolve it is about to read with the CPU, and only
  // that one is run, waited for and written into its tiled destination. Everything
  // else the scene resolves stays on the GPU.
  void ReadbackResolve(Resolved& dest, const draw_util::ResolveInfo& info, uint32_t base_address, uint32_t dest_x,
                       uint32_t dest_y, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, uint32_t scale = 4);
  // Debug: appends a line to <files>/plume-resolve.log while <files>/plume-resolve-log exists.
  void LogResolveLine(const std::string& line);
  uint32_t photo_captures_ = 0;
  bool capture_this_resolve_ = false;
  bool resolve_log_ = false;
  void NativeResolve(const uint32_t* record);
  // The surfaces bound when the lists last sent the target registers
  // (kOpSurfaces): render targets 0-3 and depth.
  struct ListSurface {
    uint32_t object = 0, surface_info = 0, info = 0, dims = 0;
  };
  std::array<ListSurface, 5> native_surfaces_{};
  // The target of a slot's surface; `info_register` is what the register
  // file holds for it, which must be the surface's.
  Target* NativeTarget(uint32_t slot, uint32_t info_register);
  Target* SurfaceTarget(uint32_t slot);
  // A framebuffer is as big as its first attachment: the others grow to it.
  void MatchSurfaceSizes(std::array<Target*, 4>& colors, uint32_t color_count, Target*& depth);
  bool GrowSurfaceTarget(Target& target, uint32_t width, uint32_t height);
  void RetireFramebuffers();
  void EvictSurfaceTargets();
  std::string SurfaceReport();
  uint64_t surface_created_ = 0, surface_grown_ = 0, surface_evicted_ = 0, surface_mismatch_ = 0;
  // Native resolves into their texture object, and the ones that go by
  // address (3D slices).
  uint64_t object_resolves_ = 0, address_resolves_count_ = 0;
  uint64_t surface_missing_ = 0;
  // Copies a rectangle of a depth target's depth into an R32_FLOAT texture, or
  // its depth and stencil into an RG32F one.
  bool CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RenderTexture* dest,
                 uint32_t dest_x, uint32_t dest_y, bool with_stencil);
  std::unique_ptr<RenderBuffer> depth_staging_;
  // The stencil half of a k_24_8_FLOAT resolve (CopyDepth): the S8 aspect,
  // then depth_stencil_pack.comp joining both into RG32F texels.
  std::unique_ptr<RenderBuffer> stencil_staging_, packed_staging_;
  VkShaderModule pack_module_ = VK_NULL_HANDLE;
  VkPipelineLayout pack_layout_ = VK_NULL_HANDLE;
  VkPipeline pack_pipeline_ = VK_NULL_HANDLE;
  bool CreatePackPipeline();

  // Adaptive triangle patches (ExpandAdaptivePatches): patch_levels.comp and
  // patch_expand.comp write each draw's points, indices and indirect
  // arguments into the slot's buffer, filled from its start every frame.
  struct AdaptiveDraw {
    RenderBuffer* buffer = nullptr;
    uint64_t points_offset = 0, points_bytes = 0, args_offset = 0;
  };
  bool CreatePatchPipelines();
  bool ExpandAdaptivePatches(uint32_t factors_base, xenos::Endian endian, uint32_t patches, uint32_t min_level,
                             uint32_t max_level, AdaptiveDraw& out);
  VkShaderModule patch_modules_[2] = {};
  VkPipelineLayout patch_layout_ = VK_NULL_HANDLE;
  VkPipeline patch_pipelines_[2] = {};
  bool patch_pipelines_failed_ = false;
  std::unique_ptr<RenderBuffer> patch_out_[kPlumeFramesInFlight];
  uint64_t patch_out_capacity_[kPlumeFramesInFlight] = {};
  uint64_t patch_out_used_ = 0;
  // The last expansion, which the same draw repeated for the next band of a
  // predicated frame takes as it is while no export has happened in between.
  struct {
    uint64_t frame = UINT64_MAX, factors = 0, exports = 0;
    uint32_t patches = 0, levels = 0;
    AdaptiveDraw draw;
  } patch_reuse_;
  uint64_t export_draws_total_ = 0;
  uint32_t adaptive_mode_ = 1;

 private:
  // Upload ring (one frame; reset after every swap).
  uint64_t Allocate(uint64_t size, uint64_t alignment = 16);
  uint8_t* Pointer(uint64_t offset) { return upload_data_ + offset; }
  uint64_t Address(uint64_t offset) const { return upload_address_ + offset; }
  bool UploadGuest(uint32_t address, uint32_t size, xenos::Endian endian, uint64_t& offset);
  // A stream copied into the ring for this draw alone, with the format 17
  // vectors at the given (offset, stride) pairs moved into the layout the
  // shaders decode (RepackFormat17).
  bool UploadRepacked(uint32_t address, uint32_t size, xenos::Endian endian,
                      const std::pair<uint32_t, uint32_t>* elements, uint32_t element_count, uint64_t& offset);
  bool ExportOverlaps(uint32_t address, uint32_t size) const;
  // A guest vertex or index stream, from a buffer of its own that outlives the
  // frame while the guest leaves that memory alone, or copied into the ring.
  //
  // Nearly all of the game's geometry never changes, and copying and swapping
  // every stream into the ring every frame was the command thread's largest
  // single cost after the draw itself (~48 MB a frame in a busy scene). The
  // guest's pages under a kept stream are write-watched (rexglue's physical
  // memory callbacks, the mechanism xenia's shared memory uses): the first
  // write to one drops every stream on it, and a stream dropped often is
  // treated as dynamic and goes back to the ring each frame, unwatched.
  bool GuestStream(uint32_t address, uint32_t size, xenos::Endian endian, RenderBuffer*& buffer,
                   uint64_t& offset);
  void DrainWatchHits();
  static std::pair<uint32_t, uint32_t> WatchHitThunk(void* context, uint32_t start, uint32_t length,
                                                     bool exact_range);
  struct KeptStream {
    std::unique_ptr<RenderBuffer> buffer;
    uint32_t base = 0, size = 0;
    uint64_t last_used = 0, dropped_frame = 0, made_frame = 0;
    uint32_t drops = 0, endian = 0;
    bool valid = false;
  };
  // Debug (files/plume-validate): the check at the moment of a draw, on the bytes it is about to read: a kept range that
  // differs from guest memory while no write hit is pending is one the GPU would read wrong.
  void ValidateRange(const char* what, uint64_t draw_hash, RenderBuffer* buffer, uint64_t offset, uint32_t guest_address,
                     uint32_t size, xenos::Endian endian);
  bool validate_windows_ = false;
  uint32_t validate_logged_ = 0;
  uint64_t kept_drop_events_ = 0, validate_ranges_ = 0, validate_stale_ = 0;
  std::unordered_map<uint64_t, KeptStream> kept_streams_;
  // What the write watch has to look at when the guest writes: the kept streams
  // and the watched textures by 64 KB block of guest memory, so a write finds
  // its few instead of every one (entries of what is gone are skipped, and the
  // index is rebuilt when it has grown far past what lives).
  std::unordered_multimap<uint32_t, uint64_t> kept_block_index_, texture_block_index_;
  static void IndexBlocks(std::unordered_multimap<uint32_t, uint64_t>& index, uint32_t base, uint32_t size,
                          uint64_t key) {
    if (!size) return;
    for (uint32_t block = base >> 16; block <= uint32_t((uint64_t(base) + size - 1) >> 16); ++block) {
      index.emplace(block, key);
    }
  }
  void DropKept(KeptStream& kept);
  // Bumped whenever a kept stream's buffer can have gone: what a draw found for
  // a vertex stream last time is good while this, the ring generation and the
  // stream's fetch constant are the same (see the stream loop of IssueDraw).
  uint64_t stream_epoch_ = 1;
  // Bumped whenever the set of export ranges changes (what overlaps what).
  uint64_t export_epoch_ = 1;
  struct StreamMemo {
    uint32_t words[2] = {};
    uint64_t native_base = 0, native_size = 0;
    uint64_t repack_signature = 0;
    uint64_t epoch = 0, generation = 0, export_epoch = 0;
    RenderBuffer* buffer = nullptr;
    uint64_t offset = 0;
    bool bound = false;
  };
  std::array<StreamMemo, 17> stream_memo_{};
  // A format-17 stream (see RepackFormat17) kept already repacked, like a kept
  // stream; falls back to UploadRepacked when it cannot be kept.
  bool RepackedStream(uint32_t address, uint32_t size, xenos::Endian endian,
                      const std::pair<uint32_t, uint32_t>* elements, uint32_t element_count,
                      RenderBuffer*& buffer, uint64_t& offset);
  LookupMemo<KeptStream, 13> kept_memo_;
  uint64_t kept_bytes_ = 0;
  // Host memory profile (see InitializeContext). A phone with 3 GB of RAM
  // shares it with the GPU: the ring, the textures and the kept streams are
  // the whole of what the driver holds, and left at the desktop-class sizes
  // they pushed the game past what Android lets a process keep - the system
  // killed it (or the Mali lost its device) as the first scene loaded.
  bool low_memory_ = false;
  uint64_t upload_slice_size_ = kUploadSize;
  uint64_t kept_budget_ = 384ull << 20;
  uint32_t max_texture_dim_ = 0;
  // BC textures: the device samples them, and files/plume-bc / the cvar want them.
  bool bc_supported_ = false;
  uint32_t bc_mode_ = 1;
  // 0 full, 1 half, 2 quarter: mips of a mip-filtered 2D texture not kept on the host.
  uint32_t texture_quality_ = 0;
  // Quarters of the guest's resolution the dynamic shadow maps (depth D24S8
  // surfaces of 640 px and more) are drawn at: 4, 3 or 2.
  uint32_t shadow_scale_ = 4;
  // The same for the scene's own 1280x720 colour and depth surfaces.
  uint32_t internal_scale_ = 4;
  uint64_t texture_budget_ = 0;  // bytes of decoded 2D textures; 0 = unlimited
  // Filled by the guest thread that tripped a watch (inside its fault
  // handler), drained by this one.
  std::mutex watch_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> watch_hits_;
  std::atomic<uint32_t> watch_serial_{0};
  uint32_t watch_seen_ = 0;
  void* watch_handle_ = nullptr;
  // Debug: <files>/plume-no-persist sends every stream through the ring again.
  bool keep_streams_ = true;
  // Memory export (renutMemExport in shader_common.h). A range of guest memory
  // a vertex shader has written lives in a buffer of its own from then on, and
  // later draws that fetch from it read that buffer: guest memory never sees
  // what the GPU wrote. The CPU's own writes there still win - each frame the
  // range is compared with the guest bytes it was last in step with, and what
  // changed is copied in.
  struct ExportRange {
    uint32_t base = 0, size = 0;
    xenos::Endian endian = xenos::Endian::k8in32;
    std::unique_ptr<RenderBuffer> buffer;
    uint64_t address = 0;
    std::vector<uint8_t> snapshot;
    uint64_t checked_frame = UINT64_MAX, exported_frame = 0;
    // Since the last ExportBarrier: written by a shader or a copy, read by a draw.
    bool written = false, read = false;
  };
  ExportRange* GetExport(uint32_t base, uint32_t size, xenos::Endian endian);
  void RefreshExport(ExportRange& range);
  bool BindStream(uint32_t address, uint32_t size, xenos::Endian endian, RenderBuffer*& buffer,
                  uint64_t& offset);
  void ExportBarrier();
  // What a frame's shaders exported goes back to guest memory once the frame
  // has run, as the hardware would have left it there: the CPU may read it,
  // and a CPU that clears a range again has to be seen doing so - zeros
  // written over the zeros guest memory still held would otherwise go
  // unnoticed and leave the GPU's old contents in place.
  void RecordWritebacks();
  void ApplyWritebacks(uint32_t slot);
  struct Writeback {
    uint32_t base = 0, size = 0;
    uint64_t offset = 0;
    const RenderBuffer* buffer = nullptr;
  };
  std::array<std::unique_ptr<RenderBuffer>, kPlumeFramesInFlight> writeback_buffers_;
  std::array<uint64_t, kPlumeFramesInFlight> writeback_capacity_{};
  std::array<std::vector<Writeback>, kPlumeFramesInFlight> writebacks_;
  bool CommitConstants(uint8_t* shadow, const void* source, size_t size, uint64_t& offset,
                       bool* written = nullptr);
  // The expansion of a patch draw, per shape and level: every point of a
  // patch's grid once, and the triangles over them as indices. It depends on
  // nothing but the shape, the level and how many patches there are, so it is
  // built once and kept, for as many patches as the largest draw has asked.
  // Built afresh every draw it was the command list thread's largest write
  // (tens of MB a frame in the water) and, unindexed, ran the guest's domain
  // shader six times for every point two triangles share.
  struct PatchGrid {
    std::unique_ptr<RenderBuffer> points, indices;
    uint32_t patches = 0;
    uint32_t points_per_patch = 0, indices_per_patch = 0;
  };
  const PatchGrid* GetPatchGrid(bool quad, uint32_t level, uint32_t patches);
  std::unordered_map<uint32_t, PatchGrid> patch_grids_;

  void BeginFrame();
  void EvictIdleTextures();
  void StartList();
  void FlushImagesForNextFrame();
  void Flush();
  void Transition(RenderTexture* texture, RenderTextureLayout& current, RenderTextureLayout layout);
  RenderFramebuffer* GetFramebuffer(const std::array<Target*, 4>& colors, uint32_t color_count,
                                    Target* depth);
  void SetFramebuffer(RenderFramebuffer* framebuffer);
  uint32_t FittedResolved(Resolved& resolved, uint32_t width, uint32_t height);
  Resolved& GetResolved(uint32_t address, RenderFormat format, uint32_t width, uint32_t height);
  // A depth map resolved whole into a texture object, without the copy: the
  // surface's image becomes the resolved texture and the surface takes another
  // (files/plume-depthalias, default on). nullptr when no image was available.
  Resolved* AliasDepthResolve(Target& source, uint32_t address);
  // Copies the contents back into a surface whose image went to a resolve, for a
  // draw or a partial clear that uses them. A clear of the whole surface drops
  // them instead (true: the image is fresh, so its stencil is undefined too).
  bool RestoreAliasedDepth(Target& target, bool full_clear);
  void NoteAliasedUse(Target& target, bool whole_clear);
  bool depth_alias_ = true;
  // Everything LayoutOf works out from a fetch constant, kept per constant.
  struct FetchLayout {
    uint64_t key = 0;
    uint32_t levels = 0, top_level = 0;
    std::array<TextureLevel, 14> levels_layout{};
    std::array<uint32_t, 14> starts{};
    bool usable = false;
    // The guest reads the top level alone (interface, fonts, post): never reduced.
    bool base_map = false;
  };
  const FetchLayout* LayoutOf(const xenos::xe_gpu_texture_fetch_t& fetch);
  const FetchLayout* LayoutOfByContents(const xenos::xe_gpu_texture_fetch_t& fetch);
  gfx::Shader* LoadShaderByContents(xenos::ShaderType shader_type, const uint32_t* host_address,
                                    uint32_t dword_count);
  uint32_t GetGuestTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestCubeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestStackedTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetGuestVolumeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch);
  void Report();
  // Host memory the cache holds, by category. Printed when the driver's block
  // total moved by 32 MB since the last line (and never twice within 15 frames).
  void ReportMemory();
  uint64_t reported_memory_blocks_ = 0;

  RenderDevice* device_ = nullptr;
  RenderCommandQueue* queue_ = nullptr;
  std::unique_ptr<ShaderStore> store_;
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> shaders_;
  // LoadShader's answers this frame, by microcode address: {dwords, shader}.
  // Across frames (renut_plume_shader_cache): the shader found at a guest
  // address and length stays that shader until the guest writes under it. The
  // pages are write-watched like a kept stream's, and an entry written over
  // three times is treated as dynamic (the ring a patched shader is copied
  // into) and goes back to hashing, unwatched.
  struct ShaderWatch {
    HostShader* shader = nullptr;
    uint32_t base = 0, bytes = 0;
    bool valid = false;
    uint32_t drops = 0;
    uint64_t dropped_frame = 0;
  };
  std::unordered_map<uint64_t, ShaderWatch> shader_watch_;
  std::unordered_multimap<uint32_t, uint64_t> shader_block_index_;
  LookupMemo<ShaderWatch, 12> shader_watch_memo_;
  bool shader_cache_ = true;
  bool key_reuse_ = true;
  uint64_t shader_cache_hits_ = 0, shader_cache_drops_ = 0;
  std::unordered_map<const uint32_t*, std::pair<uint32_t, HostShader*>> shader_lookup_;
  // In front of shader_lookup_, by address and length; cleared with it.
  LookupMemo<HostShader, 12> shader_memo_;
  uint64_t shader_lookup_frame_ = UINT64_MAX;
  std::vector<RenderInputElement> draw_input_elements_;
  std::vector<RenderInputSlot> draw_input_slots_;
  std::vector<RenderSpecConstant> draw_spec_constants_;
  std::unordered_set<uint64_t> missed_;
  std::unordered_map<uint64_t, uint64_t> missed_draws_;
  // Never invalidated: a fetch constant always describes the same layout. The
  // contents behind it are checked separately, once a frame, in GetGuestTexture.
  std::unordered_map<uint64_t, FetchLayout> fetch_layouts_;
  LookupMemo<FetchLayout, 12> layout_memo_;
  std::unordered_set<uint64_t> missed_pairs_;
  std::string miss_dump_directory_;
  // One-frame trace: touching <files>/plume-trace-request records the next
  // frame's draws and resolves into <files>/plume-trace.txt.
  FILE* trace_ = nullptr;
  // The guest bytes each traced draw's streams held when it was drawn, checked
  // again at the swap: a stream the CPU rewrote after the draw was read early.
  struct TracedStream {
    uint32_t draw, stream, address;
    std::vector<uint8_t> bytes;
  };
  std::vector<TracedStream> traced_streams_;
  // Pixel shaders whose shadow-cascade constants this trace already holds.
  std::unordered_set<uint64_t> traced_cascade_ps_;
  uint64_t trace_ps_ = 0;
  // Armed trace, for effects too short to hit by request: while
  // <files>/plume-trace-arm exists, the shader pairs of 150 frames are learned,
  // and then a frame that draws a pair not seen before has the next 3 frames
  // traced into <files>/plume-trace-arm-<burst>-<frame>.txt (6 bursts at most).
  bool arm_ = false;
  uint32_t arm_learn_ = 0, arm_burst_ = 0, arm_left_ = 0;
  std::unordered_set<uint64_t> arm_pairs_;
  std::vector<std::pair<uint64_t, uint64_t>> arm_new_;
  // A shader hash (hex) in the arm file: no learning, only draws with that
  // vertex or pixel shader fire a burst.
  uint64_t arm_target_ = 0;
  // Where the traced frame's exported ranges go: "exportdump", or a folder of
  // its own for an armed frame.
  std::string trace_dump_name_ = "exportdump";
  // Debug: <files>/plume-skip holds a bit mask of draw kinds to drop (see IssueDraw).
  uint32_t skip_mask_ = 0;
  // Predicated tiling, done once. The 2x MSAA scene does not fit the 10 MB of
  // EDRAM, so Direct3D replays it for a lower band with the window offset
  // moved up by the band's height, and the game draws a few objects per band
  // itself (window offset 0, a viewport the size of the band). Host targets
  // have no size limit: the upper band's full-screen draws get the whole
  // screen, the lower band's replay is dropped, the game's own lower-band
  // draws move down by the band's height, and the resolves follow. Learned
  // from a frame that replayed a band, applied from the next one on.
  // Off by default: the water then reflects up close instead of in the
  // distance (it reads something per band that the merge does not give back
  // yet). <files>/plume-merge-bands "1" turns it on: ~30 FPS in Showdown Town.
  // Debug: <files>/plume-bind, per-draw state the draw path leaves as it was
  // bound, to price each on the GPU (the picture is wrong). 1 constants
  // descriptor set, 2 push constants, 4 vertex buffers, 8 viewport and scissor,
  // 16 drops depth-only draws.
  uint32_t bind_skip_ = 0;
  bool shadow_native_constants_ = true;
  // What exporting shaders write stays on the GPU: the draws that read it are
  // served from the export buffers. Copying it back to guest memory a few
  // frames late overwrote memory the game had freed and reused (its asserts
  // fired, then it wrote through garbage pointers or fed the CP garbage), so
  // it is only done, for debugging, while <files>/plume-writeback exists.
  bool writeback_ = false;
  bool writeback_fast_ = true;
  // Debug: <files>/plume-drawskip holds "<first> <end>": draws of each frame
  // with an index in that range are dropped (to bisect a rendering bug).
  uint32_t drawskip_first_ = 0, drawskip_end_ = 0, frame_draw_ = 0;
  // Debug: <files>/plume-skip-shader holds a shader hash (hex); draws with
  // that vertex or pixel shader are dropped.
  uint64_t skip_shader_ = 0;
  // Debug: <files>/plume-debug-shader holds a shader hash (hex); draws with
  // that vertex or pixel shader lose depth testing, blending, alpha test and
  // culling, to show where they land and what colour they come out.
  uint64_t debug_shader_ = 0;
  // "<hash> <mode>": 1 drops the depth test, 2 blending, 4 alpha test and
  // culling (7 when left out); 8 draws a point list's sprite coordinate; 16
  // and 32 whiten textures (see the shared block).
  uint32_t debug_mode_ = 7;
  // Debug: <files>/plume-white-slots holds a hex mask of 2D fetch slots that
  // every draw samples as white - e.g. 8000 whitens slot 15 (a shadow map reads
  // as fully lit), to see which texture a lighting artifact comes from.
  uint32_t white_slots_ = 0;
  // Debug: <files>/plume-pixel-center holds 1 to drop the D3D9 half-pixel
  // shift from depth-only draws (the shadow maps), 2 from every draw.
  uint32_t pixel_center_debug_ = 0;
  std::map<std::tuple<std::string, uint64_t, uint64_t, bool>, uint64_t> drops_;
  // Debug: <files>/plume-snap holds "<hex address> <n>": the n-th resolve of
  // the frame into that address is copied aside and presented instead of the
  // front buffer, to see an intermediate pass on screen.
  uint32_t snap_address_ = 0, snap_ordinal_ = 0, snap_seen_ = 0;
  // The tallest a draw has rendered, which bounds what a clear may grow.
  std::chrono::steady_clock::time_point frame_start_{};
  uint64_t cpu_at_swap_ = 0;
  uint64_t reported_guest_swaps_ = 0;
  std::unique_ptr<RenderTexture> snap_;
  uint32_t snap_width_ = 0, snap_height_ = 0;
  RenderFormat snap_format_ = RenderFormat::UNKNOWN;
  RenderTextureLayout snap_layout_ = RenderTextureLayout::UNKNOWN;
  bool snap_taken_ = false;
  // Debug: <files>/plume-snap-shader holds a pixel shader hash (hex). With
  // plume-snap-dump present too, the first colour resolve one of its draws
  // samples is snapshotted and dumped, then both files go - for a texture
  // whose resolve address changes every run, like the world portal's photo.
  uint64_t snap_shader_ = 0;
  void SnapshotResolved(Resolved& dest);
  // Debug: while <files>/plume-snap-dump exists, the next snapshot is also
  // read back into <files>/plume-snap.rgba (a text header, then the texels) -
  // the blit that presents it cannot show the alpha channel.
  std::unique_ptr<RenderBuffer> snap_dump_;
  uint32_t snap_dump_row_ = 0;
  // Debug: <files>/plume-const holds lines "ps|vs <register> x y z w", float
  // constants written over the guest's before every draw - to try another
  // value (the water picks its tile layout from ps c222) without a rebuild.
  // A line "bool <word> <set hex> <clear hex>" ors and clears bits of that
  // boolean-register word instead.
  struct ConstOverride {
    bool pixel;
    uint32_t reg;
    float value[4];
    bool bool_word = false;
    uint32_t set = 0, clear = 0;
  };
  std::vector<ConstOverride> const_overrides_;
  // MSAA off: the game still records some draws once per band mask - the
  // screen-space shadow multiply as masks ...03 then ...0C, the water's patches
  // as 3 then C - and with a single tile every predicated packet runs
  // (SetSingleBinTiling), so the second copy drew the same thing over the first:
  // shadows applied twice. A predicated draw whose whole state matches an
  // earlier one of the frame under a mask that shares no tile with its own is
  // that copy, and is dropped. <files>/plume-dedupe "0" turns it off.
  struct TwinEntry {
    uint64_t sig = 0, mask = 0, frame = UINT64_MAX;
  };
  bool twin_dedupe_ = true;
  // <files>/plume-refractfix "0" turns off the refraction sprite bounds fix.
  bool refraction_fix_ = true;
  // The game's polygon offset (PA_SU_POLY_OFFSET_*): ground decals - footprints,
  // the tread blocks and skid marks a vehicle leaves - are quads lying on the
  // terrain and ask to be pulled a little toward the camera. Applying it made
  // no visible difference to those marks (measured 04/10, scales 1 and 16), and
  // other scenes were not looked at, so it is off unless
  // <files>/plume-polyoffset "<on> [scale]" turns it on.
  bool poly_offset_ = false;
  float poly_offset_scale_ = 1.0f;
  std::array<TwinEntry, 128> twin_ring_{};
  uint32_t twin_pos_ = 0;
  // Debug: <files>/plume-depth-dump holds a pixel shader hash (hex). The first
  // depth resolve one of its draws samples is read back, as the shader sees it,
  // into <files>/plume-depth.f32 (a text header "width height row format
  // generation frame", then row-padded texels: R32G32_FLOAT is depth, stencil),
  // and the trigger goes. The splash's soft fade reads it and nothing else.
  uint64_t depth_dump_ps_ = 0;
  std::unique_ptr<RenderBuffer> depth_dump_;
  uint32_t depth_dump_w_ = 0, depth_dump_h_ = 0, depth_dump_row_ = 0, depth_dump_format_ = 0;
  uint64_t depth_dump_generation_ = 0, depth_dump_frame_ = 0;
  // Debug: touching <files>/plume-texdump-request re-decodes every texture
  // the next frame uses and writes each to <files>/texdump/ (raw RGBA).
  std::string texdump_directory_;
  std::unordered_set<uint64_t> dumped_;
  void DumpTexture(const xenos::xe_gpu_texture_fetch_t& fetch, const std::vector<uint32_t>& rgba,
                   uint32_t width, uint32_t height, const char* kind);
  template <typename... Args>
  void Trace(fmt::format_string<Args...> format, Args&&... args) {
    if (trace_) {
      fmt::print(trace_, format, std::forward<Args>(args)...);
      std::fputc(10, trace_);
    }
  }

  std::unique_ptr<RenderPipelineLayout> layout_;
  std::unique_ptr<RenderDescriptorSet> set_2d_, set_3d_, set_cube_, set_samplers_;
  std::array<std::unique_ptr<RenderSampler>, kMaxSamplers> samplers_;
  std::unordered_map<uint32_t, uint32_t> sampler_indices_;
  // Nothing is ever erased from sampler_indices_, so its nodes stay put.
  LookupMemo<uint32_t, 8> sampler_memo_;
  uint32_t next_sampler_ = 0;
  std::unique_ptr<RenderTexture> white_2d_, white_3d_, white_cube_;
  std::unique_ptr<RenderTextureView> white_cube_view_;
  std::unique_ptr<RenderShader> rect_list_gs_;
  std::unique_ptr<RenderShader> point_list_gs_;

  // Keyed by guest base. Never overlapping: a new range swallows the ones it touches.
  std::map<uint32_t, ExportRange> exports_;
  std::array<std::vector<std::unique_ptr<RenderBuffer>>, kPlumeFramesInFlight> retired_buffers_;
  // Debug: files/plume-retire-delay (frames) keeps retired buffers alive that much longer than the
  // frames in flight need, to tell a read past a buffer's end from a read of one freed too soon.
  std::deque<std::pair<uint64_t, std::shared_ptr<void>>> delayed_buffers_;
  uint64_t retire_delay_ = 0;
  uint32_t retire_mask_ = 15;  // which classes: 1 framebuffers, 2 textures, 4 views, 8 buffers

  std::unique_ptr<RenderBuffer> upload_;
  uint8_t* upload_data_ = nullptr;
  uint64_t upload_address_ = 0;
  uint64_t upload_used_ = 0;
  uint64_t zero_offset_ = 0;
  std::unordered_map<uint64_t, uint64_t> uploaded_streams_;
  // Counted up every time the ring restarts, so a draw can tell that the
  // blocks it placed there were dropped underneath it.
  uint64_t upload_generation_ = 0;
  // The three constant blocks a draw hands the shaders, kept between draws.
  //
  // The guest rewrites a few registers between draws and leaves the rest
  // alone, so nearly every draw was copying the same 10 KB into a fresh
  // stretch of the ring. Pointing it at the block the last draw left there
  // saves the copy, and - what this is really for - lets the GPU read lines it
  // already has: every constant a recompiled shader reads is a load from this
  // buffer (cReg in shader_common.h), so a block per draw turns every one of
  // those loads into a miss.
  //
  // The shadow copies are what the comparison reads. The ring itself is
  // write-combined memory, which is slow to read back.
  static constexpr size_t kStageConstantSize = 4096;
  alignas(16) uint8_t vs_shadow_[kStageConstantSize] = {};
  alignas(16) uint8_t ps_shadow_[kStageConstantSize] = {};
  alignas(16) uint8_t shared_shadow_[RENUT_SHARED_SIZE] = {};
  // Where a draw assembles its shared block before it is compared.
  alignas(16) uint8_t shared_built_[RENUT_SHARED_SIZE] = {};
  // Where each stage's constant blocks were last placed in the ring.
  uint64_t constant_offsets_[2][kConstantChunks] = {
      {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
  uint64_t shared_offset_ = UINT64_MAX;
  // And its second half, from RENUT_SHARED_HOT_SIZE on (binding 9).
  uint64_t shared_cold_offset_ = UINT64_MAX;
  // The loop constants go into that half only when the guest wrote one.
  bool loop_constants_dirty_ = true;
  // The addresses last pushed, to leave the push constants alone when the
  // three blocks are the ones already bound.
  uint64_t pushed_constants_[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
  // The same three blocks as uniform buffers (set 4 in shader_common.h): one
  // descriptor set over the whole ring, bound with each draw's offsets. The
  // recompiled shaders read every constant through these; the push constants
  // stay for the geometry shaders, which read the shared block's address.
  VkDescriptorSetLayout constants_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool constants_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet constants_set_ = VK_NULL_HANDLE;
  // Set 4: the vertex blocks, the pixel blocks, then the shared block's two
  // halves, then twice the vertex file as one block (g_VertexShaderConstantRel:
  // bindings 10 and 11 read the same bytes, 11 declared far larger).
  static constexpr uint32_t kSharedHotBinding = 2 * kConstantChunks;
  static constexpr uint32_t kSharedColdBinding = kSharedHotBinding + 1;
  static constexpr uint32_t kRelBinding = kSharedColdBinding + 1;
  static constexpr uint32_t kConstantBindings = kRelBinding + 2;
  uint32_t bound_constant_offsets_[kConstantBindings];
  // files/plume-relconst (or the cvar): how a vertex shader's relative constant
  // reads (a0, aL) are served - 0 by the four blocks, 1 by the whole file as one
  // block, 2 by the same block declared too large for the driver to promote
  // into the constant file. Part of the pipeline key through spec constant 12.
  // Default 2: measured on identical frames (tools/quick-ab.py --interleave) mode
  // 1 costs +8% on the 108 vertex shaders that read by a0/aL, mode 2 -27%, and a
  // frame costs 22% fewer GPU cycles in the heavy plaza (tools/quick-fps.py).
  uint32_t rel_constants_mode_ = 0;
  // Modes 11 and 12 alternate between mode 0 and mode 1 or 2 from one draw of a
  // shader to the next, and mark the alternate's vertex shader hash in the pair
  // profile (bit 60 and 61 xored in), so a single capture holds both variants
  // of the same draws: tools/quick-ab.py --interleave.
  uint32_t rel_toggle_[256] = {};
  // Where the whole vertex file was last placed in the ring, and what it held.
  uint64_t vs_all_offset_ = UINT64_MAX;
  alignas(16) uint8_t vs_all_shadow_[4096] = {};
  // What the list has bound, to leave unchanged state alone (StartList resets).
  RenderViewport bound_viewport_;
  RenderRect bound_scissor_;
  bool viewport_bound_ = false, scissor_bound_ = false;
  struct VertexBinding {
    const RenderBuffer* buffer = nullptr;
    uint64_t offset = UINT64_MAX;
  };
  std::array<VertexBinding, 17> bound_vertex_buffers_{};
  const RenderBuffer* bound_index_buffer_ = nullptr;
  RenderFormat bound_index_format_ = RenderFormat::UNKNOWN;
  void BindIndexBuffer(RenderBuffer* buffer, RenderFormat format) {
    if (bound_index_buffer_ == buffer && bound_index_format_ == format) return;
    // Vulkan binds an index buffer by offset alone; the size is not used.
    RenderIndexBufferView view(RenderBufferReference(buffer, 0), 0, format);
    L()->setIndexBuffer(&view);
    bound_index_buffer_ = buffer;
    bound_index_format_ = format;
  }
  bool CreateConstantBuffers();

  // One command list per frame in flight, and `list_` is the one being
  // recorded. The slot also owns the slice of the upload ring that list reads
  // from and the host resources it retired, all released together once the
  // renderer's fence for that slot has been waited on.
  // GPU time per render pass, from Vulkan timestamps written at every pass
  // boundary. setFramebuffer ends the active pass before it binds the new one
  // and the pass only reopens at the next draw, so those points are outside a
  // render pass - which is where a timestamp on a tiler means anything. Read
  // back once the slot's fence has been waited on, so nothing stalls for it.
  static constexpr uint32_t kMaxTimestamps = 512;
  VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
  double timestamp_ns_ = 0.0;
  struct PassMark {
    uint32_t width = 0, height = 0, colors = 0, draws = 0;
    bool depth = false;
    // A resolve's copies run between two framebuffer marks, so without a mark
    // of their own their cost is charged to whatever pass preceded them.
    // 1 = the barrier that ends the open pass, 2 = the copies themselves.
    uint8_t resolve = 0;
  };
  std::array<std::vector<PassMark>, kPlumeFramesInFlight> pass_marks_;
  std::array<uint32_t, kPlumeFramesInFlight> timestamp_count_{};
  uint32_t timestamps_used_ = 0;
  uint32_t color_count_of_pass_ = 0;
  bool depth_of_pass_ = false;
  struct PassTotal {
    uint64_t ns = 0, passes = 0, draws = 0;
  };
  std::unordered_map<uint64_t, PassTotal> pass_totals_;
  uint64_t gpu_ns_ = 0;
  void WriteTimestamp();
  void ReadTimestamps(uint32_t slot);

  // Where the GPU's time goes, per (vertex, pixel) shader pair. Debug only:
  // <files>/plume-pairprof holds "<mode> <frames>" and is consumed when read.
  // The next `frames` frames get two timestamps around every draw - from before
  // its state changes to after the draw call - and the sums per pair are
  // written to <files>/plume-pairprof.txt once the last of them has been read
  // back. Mode 1 stamps the top of the pipe, which reads the command
  // processor's clock without waiting for the GPU: when the GPU is the
  // bottleneck the processor stalls on whichever draw cannot be taken in, so
  // the spans add up to throughput cost with the draws still overlapping. Mode
  // 2 stamps the bottom, which drains the pipe at both ends and so measures each
  // draw alone, latency included. The per-pass timestamps above are separate.
  static constexpr uint32_t kPairQueries = 32768;  // per slot: two per draw
  VkQueryPool pair_pool_ = VK_NULL_HANDLE;
  struct PairMark {
    uint64_t vs = 0, ps = 0;
    uint32_t count = 0;
    uint8_t colors = 0, depth = 0, kind = 0, switched = 0;
  };
  std::array<std::vector<PairMark>, kPlumeFramesInFlight> pair_marks_;
  std::array<uint32_t, kPlumeFramesInFlight> pair_count_{};
  std::array<bool, kPlumeFramesInFlight> pair_pending_{};
  struct PairTotal {
    uint64_t ns = 0, draws = 0, vertices = 0, switches = 0;
    uint8_t colors = 0, depth = 0;
  };
  std::map<std::pair<uint64_t, uint64_t>, PairTotal> pair_totals_;
  uint32_t pairprof_mode_ = 0, pairprof_frames_left_ = 0;
  bool pair_capture_ = false;
  // Whether each recorded draw leaves its entry in recent_draws_, for the report
  // of a lost device. A hash lookup, ~200 bytes of stream addresses and a few
  // buffer-address calls a draw: on by default only where the GPU is a Mali,
  // which has lost its device on a draw's data; files/plume-trail overrides.
  bool trail_ = false;
  bool trail_device_default_ = false;
  uint32_t pair_used_ = 0, pair_frames_ = 0;
  uint64_t pair_span_ns_ = 0, pair_gap_ns_ = 0, pair_frame_ns_ = 0;
  void PairStamp();
  void ReadPairTimestamps(uint32_t slot);
  void WritePairReport();

  std::array<std::unique_ptr<RenderCommandList>, kPlumeFramesInFlight> lists_;
  RenderCommandList* list_ = nullptr;
  uint32_t slot_ = kPlumeFramesInFlight - 1;
  // Where the current slot's slice of the ring starts.
  uint64_t slice_base_ = 0;
  // Permanent allocations (white texel, zeros) live before the first slice.
  uint64_t slice_origin_ = 0;
  std::unique_ptr<RenderCommandFence> fence_;
  bool list_open_ = false;
  RenderFramebuffer* bound_framebuffer_ = nullptr;
  const RenderPipeline* bound_pipeline_ = nullptr;
  uint64_t bound_interface_key_ = 0;
  bool state_bound_ = false;

  // Deferred draws. IssueDraw gathers what the Vulkan calls of a draw need into
  // a packet. A draw that cannot change the picture when it is reordered
  // against its neighbours waits in a window, and the window is recorded
  // grouped by pipeline when it closes, so that the GPU swaps programs once per
  // group instead of once per draw. Everything else that records into the list
  // goes through L(), which closes the window first, so the command stream
  // differs from the immediate one only inside a window and nothing is ever
  // recorded after a barrier, copy or clear it used to precede.
  struct DeferredSlot {
    RenderInputSlot slot;
    RenderVertexBufferView view;
  };
  struct DrawPacket {
    // kPatchIndirect: an adaptive patch draw, whose index count the GPU wrote
    // at indirect_offset of index_buffer (ExpandAdaptivePatches).
    enum class Kind : uint8_t { kNone, kIndexed, kPatch, kPlain, kPatchIndirect };
    RenderFramebuffer* framebuffer = nullptr;
    uint32_t pass_colors = 0;
    bool pass_depth = false;
    RenderPipeline* pipeline = nullptr;
    uint64_t interface_key = 0;
    bool blend_constants = false;
    bool stencil = false;
    float blend_factor[4] = {};
    uint32_t stencil_ref = 0, stencil_mask = 0, stencil_write_mask = 0;
    uint64_t push[3] = {};
    uint32_t instances = 1;
    uint32_t constant_offsets[kConstantBindings] = {};
    RenderViewport viewport;
    RenderRect scissor;
    uint32_t first_slot = 0, slot_count = 0;
    Kind kind = Kind::kNone;
    RenderBuffer* index_buffer = nullptr;
    RenderFormat index_format = RenderFormat::R16_UINT;
    uint32_t count = 0, first = 0;
    int32_t base_vertex = 0;
    uint64_t indirect_offset = 0;
    // Only for the pair profile, and the trail a lost device prints.
    uint64_t vs_hash = 0, ps_hash = 0;
    uint64_t s0_addr = 0, s10_addr = 0;
    uint32_t s0_size = 0, s10_size = 0;
    // The device address and size of each of the 17 streams the shaders read by address.
    uint64_t stream_addr[17] = {};
    uint32_t stream_size[17] = {};
    // Hi-Z (see HizFlushWindow): the depth target this draw can be tested
    // against, its image when the packet was made, the box's rectangle (target
    // pixels) and nearest depth, the draw's identity from frame to frame, and,
    // once tested, where its answer is in the slot's predicate buffer.
    Target* hiz_depth = nullptr;
    const RenderTexture* hiz_texture = nullptr;
    float hiz_rect[4] = {};
    float hiz_z = 0.f;
    uint64_t hiz_key = 0;
    uint32_t hiz_pred_offset = UINT32_MAX;
    uint32_t hiz_query = UINT32_MAX;
  };
  // The draws recorded last, for the report a lost device prints (see
  // ReportDeviceLost). Written by this thread only; the watcher reads it after
  // the GPU has already faulted, so a torn entry costs a wrong line at worst.
  struct RecentDraw {
    uint64_t frame = 0, vs = 0, ps = 0, pipeline = 0, framebuffer = 0, s0_addr = 0, s10_addr = 0;
    uint64_t pair_first_frame = 0;  // the frame this vertex/pixel pair was first recorded in
    uint32_t kind = 0, count = 0, first = 0, s0_size = 0, s10_size = 0;
    // Everything the GPU reads for this draw by address: the streams, the vertex
    // buffers the input assembler walks and the index range.
    uint64_t stream_addr[17] = {};
    uint32_t stream_size[17] = {};
    uint64_t index_addr = 0;
    uint32_t index_bytes = 0, index_size = 0;
    int32_t base_vertex = 0;
    struct VertexRange {
      uint64_t addr = 0;
      uint32_t size = 0, slot = 0;
    } vertex[6];
    uint32_t vertex_count = 0;
  };
  static constexpr size_t kRecentDraws = 6144;
  std::vector<RecentDraw> recent_draws_ = std::vector<RecentDraw>(kRecentDraws);
  std::atomic<uint64_t> recent_draw_count_{0};
  std::unordered_map<uint64_t, uint64_t> pair_first_frame_;  // frame a pair was first recorded
  std::atomic<bool> device_watch_stop_{false};
  // Joins on destruction, so a processor that never reaches ShutdownContext
  // does not terminate the process with a running thread.
  struct WatchThread {
    std::atomic<bool>& stop;
    std::thread thread;
    explicit WatchThread(std::atomic<bool>& flag) : stop(flag) {}
    ~WatchThread() {
      stop = true;
      if (thread.joinable()) thread.join();
    }
  };
  WatchThread device_watch_{device_watch_stop_};
  void ReportDeviceLost();
  bool PairBegin(const DrawPacket& pk);
  // Debug: <files>/plume-syncdraws submits and waits (3 s at most) after every
  // draw, so a draw that takes the GPU down is the last one named in the log.
  // Draws stop being deferred while it is on.
  bool sync_draws_ = false;
  // files/plume-syncwatch (hex hash): a synced draw whose vertex or pixel shader is this one is logged whole.
  uint64_t sync_watch_ = 0;
  uint32_t sync_watch_count_ = 0;
  uint64_t sync_draw_count_ = 0;
  uint64_t sync_stale_streams_ = 0;  // streams, over all synced draws, whose kept window differed from guest memory
  uint32_t sync_stale_logged_ = 0;
  // Debug (files/plume-skin-skip: hex vertex shader hashes): draws of those skinned shaders are dropped,
  // and every skinned vertex shader seen is counted and listed (LogSkinned) - a bisect of the culprit.
  struct SkinnedSeen {
    uint64_t draws = 0, first_frame = 0, last_frame = 0;
  };
  bool skin_log_ = false;
  std::unordered_set<uint64_t> skin_skip_;
  std::unordered_map<uint64_t, SkinnedSeen> skinned_seen_;
  std::mutex skinned_mutex_;
  void LogSkinned();
  std::unordered_set<uint64_t> sync_seen_;
  // The first draw of each pair that ran to completion, for the line a failure prints.
  std::unordered_map<uint64_t, std::string> sync_first_ok_;
  std::unordered_map<uint64_t, uint32_t> sync_diag_;
  std::string sync_draw_info_;
  std::array<std::string, 6> sync_recent_;
  void SyncAfterDraw(const DrawPacket& pk);
  // files/plume-syncevery (N > 1): the list is submitted and waited for after every Nth draw, with
  // none of plume-syncdraws' bookkeeping - how many draws in flight the failure needs.
  void SyncEvery();
  uint32_t sync_every_ = 0, sync_every_counter_ = 0;
  static constexpr size_t kMaxDeferred = 384;
  static constexpr size_t kMaxDeferredHiz = 1536;
  std::vector<DrawPacket> deferred_, flushing_;
  std::vector<DeferredSlot> deferred_slots_, flushing_slots_, draw_slots_;
  std::vector<uint32_t> defer_order_, defer_group_start_;
  std::unordered_map<const RenderPipeline*, uint32_t> defer_groups_;
  uint64_t deferred_signature_ = 0;
  uint32_t defer_mode_ = 1;
  // files/plume-fetchspec (or the cvar): 1 hands the live fetch table to the
  // driver as specialization constants. Part of the pipeline key, so a change
  // builds the pipelines of the shaders it touches afresh.
  uint32_t fetch_spec_mode_ = 1;
  // files/plume-depthstrip (or the cvar): 1 draws the draws that have no
  // fragment shader with the vertex shader's output-less form. Part of the
  // pipeline key.
  uint32_t depth_strip_mode_ = 1;
  // files/plume-patchlevel: percent of the guest's tessellation level the water
  // grid uses (100 = as the game sets it).
  uint32_t patch_level_percent_ = 100;
  // files/plume-shadowmin: shadow casters (depth-only draws into a shadow map)
  // with fewer indices than this are dropped; 0 keeps them all.
  uint32_t shadow_min_indices_ = 0;
  bool no_shadows_ = false, no_bloom_ = false;  // cvars renut_plume_no_shadows / _no_bloom
  void FlushDeferred(uint32_t reason = 0);
  bool in_issue_draw_ = false;
  void EmitDraw(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count);

  // ---- Hi-Z (hiz_model.h) ----
  // files/plume-hiz: 0 off, 1 counts what could be dropped, 2 drops the draws
  // whose box lies wholly outside the frustum or the scissor (exact).
  uint32_t hiz_mode_ = 0;
  // files/plume-hizskip: 0 tests every draw, 1 (default) samples the meshes that are
  // almost never culled.
  bool hiz_skip_ = true;
  struct HizModelEntry {
    std::shared_ptr<renut::hiz::Model> model;
    bool validated = false;
  };
  std::unordered_map<uint64_t, HizModelEntry> hiz_models_;
  // An object-space box of the fetched element over the vertices a draw reads,
  // kept while guest memory under it is not written.
  struct HizBox {
    bool valid = false;
    float lo[4] = {}, hi[4] = {};
    uint32_t vertex_base = 0, vertex_size = 0, index_base = 0, index_size = 0;
    uint32_t first_vertex = 0, last_vertex = 0;
    uint64_t checksum = 0;
    uint64_t checked_frame = 0;
  };
  // How often draws of a mesh (all of its instances together) were found outside
  // the frustum, decayed: a mesh that is almost never culled is only sampled
  // (HizCullDraw), because its tests cost more than they save. Direct-mapped by a
  // key of the model, the fetch range and the index range; a collision restarts it.
  struct HizCold {
    uint64_t key = 0;
    uint32_t seen = 0, culled = 0, phase = 0;
  };
  std::array<HizCold, 4096> hiz_cold_{};
  std::unordered_map<uint64_t, HizBox> hiz_boxes_;
  // 64 KB blocks of guest memory -> keys of the boxes made from them, so a write
  // finds its boxes without walking all of them (entries of boxes already gone
  // are left and ignored).
  std::unordered_multimap<uint32_t, uint64_t> hiz_box_blocks_;
  void HizIndexBox(uint64_t key, const HizBox& box);
  void HizDropBoxes(uint32_t start, uint32_t length);
  HizBox hiz_scratch_box_;
  // A direct-mapped front for hiz_boxes_: the same mesh and index range come
  // back draw after draw and frame after frame. Entries die with a generation
  // bump whenever a box is erased.
  struct HizFront {
    uint64_t key = 0;
    const HizBox* box = nullptr;
    uint32_t generation = 0;
  };
  std::array<HizFront, 8192> hiz_front_{};
  uint32_t hiz_box_generation_ = 1;
  std::unordered_map<uint64_t, uint64_t> hiz_clip_states_;
  HizModelEntry* HizModelFor(HostShader& vs);
  const HizBox* HizBoxFor(const renut::hiz::Model& model, const xenos::xe_gpu_vertex_fetch_t& fetch,
                          const IndexBufferInfo* index_info, uint32_t index_count, int32_t base_vertex);
  bool HizValidate(HostShader& vs, renut::hiz::Model& model, const xenos::xe_gpu_vertex_fetch_t& fetch,
                   uint32_t vertex_a, uint32_t vertex_b);
  // True when nothing of the draw can reach the target.
  bool HizCullDraw(HostShader& vs, xenos::PrimitiveType prim_type, uint32_t index_count,
                   const IndexBufferInfo* index_info, bool color_pass);
  // What HizCullDraw learned about the draw it did not drop, for the occlusion
  // test: the screen rectangle (guest pixels) and nearest depth of its box.
  struct HizTest {
    bool valid = false;
    float rect[4] = {};
    float z = 0.f;
    uint64_t key = 0;
  } hiz_test_;
  uint64_t hiz_last_box_key_ = 0;
  uint32_t hiz_timer_phase_ = 0;
  bool hiz_stage_timing_ = false;
  std::unordered_map<uint64_t, uint64_t> hiz_nomodel_draws_;
  std::unordered_map<uint64_t, uint32_t> hiz_nomodel_why_;
  // ---- Hi-Z occlusion (see HizFlushWindow) ----
  bool hiz_cond_ = false;  // the device has conditional rendering
  static constexpr uint32_t kHizMaxRecords = 16384;
  VkPipeline hiz_pipelines_[3] = {};
  VkShaderModule hiz_modules_[3] = {};
  VkPipelineLayout hiz_layout_ = VK_NULL_HANDLE;
  bool hiz_pipelines_failed_ = false;
  std::unique_ptr<RenderBuffer> hiz_pyramid_;
  std::array<std::unique_ptr<RenderBuffer>, kPlumeFramesInFlight> hiz_pred_, hiz_read_;
  std::array<uint32_t, kPlumeFramesInFlight> hiz_pred_used_{};
  bool hiz_debug_ = false;
  // Debug, from the environment: RENUT_HIZ_FORCE holds every testable draw back,
  // RENUT_HIZ_VERIFY runs the held draws regardless inside occlusion queries.
  bool hiz_force_ = false, hiz_verify_ = false;
  static constexpr uint32_t kHizMaxQueries = 4096;
  VkQueryPool hiz_query_pool_ = VK_NULL_HANDLE;
  std::array<uint32_t, kPlumeFramesInFlight> hiz_queries_used_{};
  // Tests paid for this frame (windows with held draws, and learning ones), the
  // first kind among them; the learning frames and how often they come.
  uint32_t hiz_tests_frame_ = 0, hiz_tests_remembered_ = 0;
  uint32_t hiz_learn_left_ = 0, hiz_learn_period_ = 8;
  uint64_t hiz_gain_answered_ = 0, hiz_gain_hidden_ = 0, hiz_total_answered_ = 0, hiz_total_hidden_ = 0;
  std::array<uint32_t, kPlumeFramesInFlight> hiz_debug_pending_{};
  std::array<std::vector<std::array<float, 5>>, kPlumeFramesInFlight> hiz_debug_rects_;
  struct HizRecord {
    uint64_t key;
    float rect[4];
    float z;
    bool held;
    uint64_t vs = 0, ps = 0;
  };
  struct HizPending {
    uint64_t key;
    bool held;
    uint32_t query;  // occlusion query of the verification run, or UINT32_MAX
    uint64_t vs = 0, ps = 0;
  };
  // Verification runs: the shader pairs of the held draws that put out no
  // samples although the test could not call them hidden, with their counts.
  std::unordered_map<uint64_t, std::array<uint64_t, 3>> hiz_zero_pairs_;
  struct HizHistory {
    bool hidden = false;
    uint64_t frame = 0;
  };
  std::vector<HizRecord> hiz_records_;
  std::vector<uint32_t> hiz_held_;
  std::array<std::vector<HizPending>, kPlumeFramesInFlight> hiz_pending_;
  std::unordered_map<uint64_t, HizHistory> hiz_history_;
  bool HizCreatePipelines();
  bool HizEnsureResources();
  bool HizRunTest(Target& target, uint32_t count, uint32_t* base_out);
  bool HizFlushWindow(size_t n);
  void HizReadResults(uint32_t slot);
  void HizDestroy();
  // The command list, after recording any draws that are waiting.
  RenderCommandList* L() {
    // The recording thread's own calls get the list as it is.
    if (OnRecorder()) return list_;
    if (!deferred_.empty()) FlushDeferred(in_issue_draw_ ? 4 : 0);
    if (rec_posted_.load(std::memory_order_relaxed) != rec_completed_.load(std::memory_order_relaxed) || rec_pending_) {
      SyncRecorder();
    }
    return list_;
  }

  // ---- Recording thread (files/plume-recthread) ----
  // The draws of a frame are decoded on this thread (IssueDraw makes a packet
  // out of the guest's state) and recorded on another: EmitDraw's Vulkan calls,
  // most of which are the driver copying its state, were about a third of this
  // thread. Packets go to the recorder in batches, in the order they would have
  // been recorded. Everything else that records, or that reads what recording
  // writes (the bound state, the pass marks), goes through L() or SyncRecorder(),
  // which waits for the recorder to be done: from this thread's point of view
  // the list is still recorded in order and by one thread at a time.
  struct RecJob {
    std::vector<DrawPacket> packets;
    std::vector<DeferredSlot> slots;
  };
  static constexpr size_t kRecBatch = 48;
  bool rec_enabled_ = true;
  std::mutex rec_mutex_;
  std::condition_variable rec_cv_, rec_done_cv_;
  std::deque<RecJob*> rec_queue_;
  std::atomic<uint32_t> rec_queued_{0};
  std::vector<RecJob*> rec_free_;
  std::vector<std::unique_ptr<RecJob>> rec_owned_;
  RecJob* rec_pending_ = nullptr;  // being filled by this thread
  std::atomic<uint64_t> rec_posted_{0}, rec_completed_{0};
  bool rec_stop_ = false;
  std::thread::id rec_thread_id_;
  // What EmitDraw counts: written by the recorder, folded into stats_ at the
  // report (after a sync).
  struct RecStats {
    uint64_t drawn = 0, pipeline_binds = 0, predicated = 0, passes = 0;
  } rec_stats_;
  // The pass attachments of the draw being recorded, on the recorder's side
  // (color_count_of_pass_ is this thread's, read by IssueDraw).
  uint32_t emit_colors_ = 0;
  bool emit_depth_ = false;
  struct RecWorker {
    std::thread thread;
    std::mutex* mutex = nullptr;
    std::condition_variable* cv = nullptr;
    bool* stop = nullptr;
    ~RecWorker() {
      if (!thread.joinable()) return;
      {
        std::lock_guard<std::mutex> lock(*mutex);
        *stop = true;
      }
      cv->notify_all();
      thread.join();
    }
  } rec_worker_;
  bool OnRecorder() const { return std::this_thread::get_id() == rec_thread_id_; }
  bool RecActive() const {
    return rec_enabled_ && hiz_mode_ < 3 && !pair_capture_ && !sync_draws_ && sync_every_ <= 1 && !trace_;
  }
  void RecQueue(const DrawPacket& pk, const DeferredSlot* slots, uint32_t slot_count);
  void RecPost();
  void SyncRecorder();
  void RecThreadMain();

  std::unordered_map<uint64_t, Target> targets_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderFramebuffer>> framebuffers_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines_;
  LookupMemo<std::unique_ptr<RenderPipeline>, 13> pipeline_memo_;
  struct PipelineJob {
    RenderGraphicsPipelineDesc desc;
    std::vector<RenderInputElement> elements;
    std::vector<RenderInputSlot> slots;
    std::vector<RenderSpecConstant> constants;
    std::unique_ptr<RenderPipeline> result;
    std::atomic<bool> done{false};
    uint64_t duration_us = 0;
    // Pipelines that differ only in the boolean registers share one.
    uint64_t family = 0;
    // The shaders and their specialization: what the driver compiles and
    // caches. Pipelines that differ in anything else (blending, depth, targets,
    // vertex layout) share the compiled program (ProgramKey).
    uint64_t program = 0;
    // The driver's cache was already asked for it and did not have it.
    bool looked_up = false;
    // Nothing stands in for its draw, which is missing from the screen until
    // the pipeline is ready.
    bool urgent = false;
    // How it ended: found in the cache rather than compiled; and whether it
    // waited for a sibling's compile of its program first.
    bool found = false, waited = false;
    // How much its draw is missed while it waits: the draws that went without
    // it, weighed by kind and size, and the last frame one did. The urgent queue is served by these rather than in order.
    std::atomic<uint32_t> demand{0};
    std::atomic<uint64_t> wanted_frame{0};
    // For the log when the driver refuses to build it.
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
  };
  std::mutex pipeline_mutex_;
  std::condition_variable pipeline_ready_;
  // Jobs that only need a look in the driver's cache go first: each takes
  // about a millisecond, and behind a compile it would wait fifty. Then the
  // compiles whose draw has nothing to stand in for it, then the rest.
  std::deque<std::shared_ptr<PipelineJob>> pipeline_lookup_queue_;
  std::deque<std::shared_ptr<PipelineJob>> pipeline_urgent_queue_;
  std::deque<std::shared_ptr<PipelineJob>> pipeline_queue_;
  // The programs workers are compiling, each with the jobs of the same program
  // that wait to find it in the cache instead of compiling it again: several
  // pipelines of one program are usually asked for in the same frame.
  // Under pipeline_mutex_.
  std::unordered_map<uint64_t, std::vector<std::shared_ptr<PipelineJob>>> compiling_programs_;
  // The frame the command thread is on, for the workers' choice of job.
  std::atomic<uint64_t> pipeline_frame_{0};
  // The urgent job a worker takes next: one whose draw was missed in the last
  // few frames before one that has left the screen, then the one missed most.
  // A scene's first frames queue hundreds at once, and in order the walls and
  // floors waited behind every effect queued before them. Under pipeline_mutex_.
  std::shared_ptr<PipelineJob> TakeUrgentJob() {
    const uint64_t now = pipeline_frame_.load(std::memory_order_relaxed);
    auto best = pipeline_urgent_queue_.begin();
    uint64_t best_score = 0;
    for (auto it = pipeline_urgent_queue_.begin(); it != pipeline_urgent_queue_.end(); ++it) {
      const PipelineJob& job = **it;
      const uint64_t recent = now - std::min(now, job.wanted_frame.load(std::memory_order_relaxed)) <= 3;
      const uint64_t score = (recent << 32) | job.demand.load(std::memory_order_relaxed);
      if (score > best_score) {
        best_score = score;
        best = it;
      }
    }
    std::shared_ptr<PipelineJob> job = std::move(*best);
    pipeline_urgent_queue_.erase(best);
    return job;
  }
  // Programs with a job queued or running, for this thread: the cache cannot
  // have those yet, so a lookup at the draw would be wasted.
  std::unordered_map<uint64_t, uint32_t> pending_programs_;
  // Lookups at the draw (renut_plume_pipeline_lookup_ms): whether they are
  // made, the budget per frame and what this frame has spent of it, and the
  // lookups that took a compile's time (a driver ignoring the request).
  bool lookup_here_ = false;
  uint64_t lookup_budget_us_ = 0, lookup_spent_us_ = 0;
  uint32_t slow_lookups_ = 0;
  bool lookup_on_workers_ = false;
  static uint64_t ProgramKey(const RenderGraphicsPipelineDesc& desc);
  // Takes a finished job's numbers into the stats and out of pending_programs_.
  void CountJob(const PipelineJob& job);
  // The newest ready pipeline of each family, drawn in place of a sibling
  // that is still compiling. Points into pipelines_, which never erases.
  std::unordered_map<uint64_t, RenderPipeline*> standins_;
  std::unordered_map<uint64_t, std::shared_ptr<PipelineJob>> pending_pipelines_;
  std::unordered_set<uint64_t> failed_pipelines_;
  std::vector<std::thread> pipeline_workers_;
  bool stop_pipeline_worker_ = false;
  void StartPipelineWorker();
  void StopPipelineWorker();
  // Moves every finished job into pipelines_, whether or not its draw came back.
  void ReapPipelines();
  // Pipeline recipes (see Recipe): whether they are kept, the keys already in
  // the file and the file being appended to; the pipelines the precompilation
  // built, each taken by the first draw that asks for it; and its threads.
  bool recipes_ = false;
  std::unordered_set<uint64_t> recorded_recipes_;
  FILE* recipe_file_ = nullptr;
  std::mutex prebuilt_mutex_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> prebuilt_;
  std::vector<std::thread> precompile_threads_;
  std::atomic<bool> stop_precompile_{false};
  void StartRecipes();
  void StopRecipes();
  // Pipeline prediction (PredictPipelines): the fixed state predicted
  // programs are built with, from the first real draw with a pixel shader
  // [0] and the first without [1]; and counts for the log.
  RenderGraphicsPipelineDesc predict_desc_[2];
  bool predict_desc_set_[2] = {};
  uint64_t predict_added_ = 0, predict_known_ = 0, predict_unknown_ = 0;
  std::unordered_set<uint64_t> drawn_programs_;
  void PredictPipelines();
  // The pipeline built ahead for this recipe, if there is one; a recipe that
  // is not in the file yet is written to it. `key` is the draw's pipeline key.
  std::unique_ptr<RenderPipeline> TakeRecipe(Recipe& recipe, uint64_t key);
  // Scene tags: which scenes each recipe is drawn in, so a scene load waits
  // for its own pipelines only (PlumeSceneRequested). In the file, a hash of
  // recipe and scene per tag; this run's recipe of each draw key; and the
  // draw keys already seen in the current scene, by their low bits (a
  // collision only misses a tag), so the check per draw is one bit.
  std::unordered_set<uint64_t> scene_tags_;
  std::unordered_map<uint64_t, uint64_t> recipe_of_;
  uint32_t tag_scene_ = 0;
  std::bitset<65536> scene_seen_;
  void NoteSceneUse(uint64_t key);
  void WriteRecord(uint32_t size, const void* data);
  // Keyed by guest address and host format: N&B reuses an address within a
  // frame for different things (a 768x768 shadow map, then the bloom chain),
  // and draws recorded earlier must keep the texture they sampled.
  std::unordered_map<uint64_t, Resolved> resolved_;
  // Textures replaced while the frame's commands may still use them; freed
  // once the frame has run (IssueSwap waits for it).
  std::array<std::vector<std::unique_ptr<RenderTexture>>, kPlumeFramesInFlight> retired_slots_;
  std::array<std::vector<std::unique_ptr<RenderTextureView>>, kPlumeFramesInFlight> retired_views_;
  // A recorded draw stores a bindless index, not a snapshot of that binding.
  // Keep old image bindings alive until the submission has finished as well.
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> retired_descriptor_slots_;
  std::vector<uint32_t> free_descriptors_;
  uint32_t AllocateTextureDescriptor() {
    uint32_t index = 0;
    if (!free_descriptors_.empty()) {
      index = free_descriptors_.back();
      free_descriptors_.pop_back();
    } else if (next_descriptor_ < kMaxTextures2D) {
      index = next_descriptor_++;
    }
    written_2d_[index] = {};
    return index;
  }
  // Debug (files/plume-dedupe-desc): a 2D descriptor already holding this texture and view is not
  // written again. The set is bound whole and read by the GPU when the draws run, so a write into
  // it while an earlier frame is still running races with that frame's reads, whatever it writes.
  struct WrittenDescriptor {
    const RenderTexture* texture = nullptr;
    const RenderTextureView* view = nullptr;
  };
  std::array<WrittenDescriptor, kMaxTextures2D> written_2d_ = {};
  bool dedupe_descriptors_ = false;
  uint64_t descriptor_writes_ = 0, descriptor_writes_skipped_ = 0;
  void SetTexture2D(uint32_t index, const RenderTexture* texture, const RenderTextureView* view = nullptr) {
    WrittenDescriptor& written = written_2d_[index];
    if (dedupe_descriptors_ && written.texture == texture && written.view == view) {
      ++descriptor_writes_skipped_;
      return;
    }
    written = {texture, view};
    ++descriptor_writes_;
    set_2d_->setTexture(index, texture, RenderTextureLayout::SHADER_READ, view);
  }
  // Volumes and stacks share the 3D set, cube maps have their own; both are far
  // smaller than the 2D one and recycled the same way.
  std::vector<uint32_t> free_3d_descriptors_, free_cube_descriptors_;
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> retired_3d_descriptor_slots_;
  std::array<std::vector<uint32_t>, kPlumeFramesInFlight> retired_cube_descriptor_slots_;
  uint32_t Allocate3DDescriptor() {
    if (!free_3d_descriptors_.empty()) {
      uint32_t index = free_3d_descriptors_.back();
      free_3d_descriptors_.pop_back();
      return index;
    }
    return next_3d_descriptor_ < kMaxTextures3D ? next_3d_descriptor_++ : 0;
  }
  uint32_t AllocateCubeDescriptor() {
    if (!free_cube_descriptors_.empty()) {
      uint32_t index = free_cube_descriptors_.back();
      free_cube_descriptors_.pop_back();
      return index;
    }
    return next_cube_descriptor_ < kMaxTexturesCube ? next_cube_descriptor_++ : 0;
  }
  uint64_t last_eviction_ = 0;
  void RetireTexture(std::unique_ptr<RenderTexture>& texture, uint32_t& descriptor) {
    if (texture) retired_slots_[slot_].push_back(std::move(texture));
    if (descriptor) retired_descriptor_slots_[slot_].push_back(descriptor);
    descriptor = 0;
  }
  // A 2D guest texture's image, its view (a swizzled BC texture has one) and descriptor.
  void RetireGuestTexture(GuestTexture& texture) {
    if (texture.view) retired_views_[slot_].push_back(std::move(texture.view));
    RetireTexture(texture.texture, texture.descriptor);
  }
  void RetireSwizzled(Resolved& r) {
    for (auto& [key, s] : r.swizzled) {
      if (s.view) retired_views_[slot_].push_back(std::move(s.view));
      if (s.descriptor) retired_descriptor_slots_[slot_].push_back(s.descriptor);
    }
    r.swizzled.clear();
  }
  uint32_t SwizzledResolved(Resolved& r, bool fitted, uint32_t swizzle, bool stencil_first);
  std::array<std::vector<std::unique_ptr<RenderFramebuffer>>, kPlumeFramesInFlight>
      retired_framebuffer_slots_;
  static uint64_t ResolvedKey(uint32_t address, RenderFormat format) {
    return uint64_t(address) | (uint64_t(format) << 32);
  }
  // The texture a fetch at `address` sees: whichever resolve wrote there
  // last, as the memory would hold. The same address is a depth resolve in one
  // phase and a colour resolve in another (the depth-of-field pass also reads
  // the scene depth as 8_8_8_8), and a stale entry of the other kind must not
  // shadow the current one.
  //
  // Asked for every sampled slot of every draw, and nearly always to hear that
  // the texture is not a resolve at all, it walked the whole map each time.
  // The answer only changes when a resolve is made, written or dropped, so it
  // is kept per address until then (InvalidateResolvedLookups), and per frame,
  // where the content check below runs once.
  Resolved* FindResolved(uint32_t address) {
    if (resolved_lookup_frame_ != swaps_) {
      resolved_lookup_frame_ = swaps_;
      ++resolved_lookup_serial_;
    }
    // Direct-mapped by address, valid for this frame and this serial: a map
    // that was emptied every frame was two hash lookups a sampled slot.
    ResolvedLookup& cached = resolved_lookup_[(address >> 12) & (kResolvedLookupSize - 1)];
    if (cached.serial == resolved_lookup_serial_ && cached.address == address) return cached.value;
    Resolved* latest = nullptr;
    for (auto& [key, r] : resolved_) {
      if (uint32_t(key) == address && r.texture && (!latest || r.written > latest->written)) latest = &r;
    }
    if (latest && latest->guest_bytes && latest->checked_frame != swaps_) {
      latest->checked_frame = swaps_;
      const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(address);
      if (ContentHash(memory, latest->guest_bytes) != latest->guest_hash) {
        // Something else lives here now; forget every resolve at the address.
        for (auto it = resolved_.begin(); it != resolved_.end();) {
          if (uint32_t(it->first) == address) {
            RetireSwizzled(it->second);
            if (it->second.alias) RetireFramebuffers();  // a framebuffer may still name its image
            RetireTexture(it->second.texture, it->second.descriptor);
            RetireTexture(it->second.fitted, it->second.fitted_descriptor);
            it = resolved_.erase(it);
          } else {
            ++it;
          }
        }
        InvalidateResolvedLookups();
        return nullptr;
      }
    }
    if (latest) latest->sampled = true;
    cached = {address, resolved_lookup_serial_, latest};
    if (resolve_log_ && latest) LogResolveLine(fmt::format("f={} sampled {:08X}", swaps_, address));
    return latest;
  }
  void InvalidateResolvedLookups() { ++resolved_lookup_serial_; }
  struct ResolvedLookup {
    uint32_t address = 0;
    uint64_t serial = 0;
    Resolved* value = nullptr;
  };
  static constexpr uint32_t kResolvedLookupSize = 1024;
  std::array<ResolvedLookup, kResolvedLookupSize> resolved_lookup_{};
  uint64_t resolved_lookup_serial_ = 1;
  uint64_t resolved_lookup_frame_ = UINT64_MAX;
  uint64_t resolve_serial_ = 0;
  uint64_t target_serial_ = 0;
  std::unordered_map<uint64_t, GuestTexture> guest_textures_;
  LookupMemo<GuestTexture, 12> texture_memo_;
  // Object identity (files/plume-objkeys): what the six words of a texture
  // fetch constant - the D3D texture's header as the guest bound it - resolved
  // to, kept per fetch slot. The game binds the same few textures to the same
  // slots draw after draw, and hashing the six words and walking two maps to
  // learn that was a visible slice of the command thread. Valid while
  // object_epoch_ is what it was: bumped wherever a layout, a texture or a
  // framebuffer these point at can go away.
  struct SlotObject {
    uint32_t words[6] = {};
    uint64_t epoch = 0;
    const FetchLayout* layout = nullptr;
    GuestTexture* texture = nullptr;
  };
  std::array<SlotObject, 32> slot_objects_{};
  int fetch_slot_ = -1;  // the slot IssueDraw is resolving, for LayoutOf and GetGuestTexture
  uint64_t object_epoch_ = 1;
  bool objkeys_ = true;
  // The last framebuffer asked for and the images it was made from.
  struct LastFramebuffer {
    const RenderTexture* images[5] = {};
    uint32_t color_count = UINT32_MAX;
    uint64_t epoch = 0;
    RenderFramebuffer* framebuffer = nullptr;
  } last_framebuffer_;
  // Streams by what their fetch constant says, across slots and draws: the
  // per-slot memo only helps a slot whose buffer does not change between
  // draws, and a mesh drawn in the colour pass and in each shadow cascade
  // comes back to different slots' memos. An entry is trusted for the frame it
  // was made in, so the kept stream's age is still refreshed once a frame.
  struct PooledStream {
    StreamMemo memo;
    uint64_t frame = UINT64_MAX;
  };
  static constexpr uint32_t kStreamPoolSize = 2048;
  std::vector<PooledStream> stream_pool_ = std::vector<PooledStream>(kStreamPoolSize);
  std::unordered_set<uint32_t> unsupported_formats_;
  uint32_t next_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_cubes_;
  uint32_t next_cube_descriptor_ = 1;
  std::unordered_map<uint64_t, GuestTexture> guest_stacks_;
  std::unordered_map<uint64_t, GuestTexture> guest_volumes_;
  uint32_t next_3d_descriptor_ = 1;

  Stats stats_;
  // Vertex shaders whose patch draws were dropped for want of a domain form, logged once each.
  std::unordered_set<uint64_t> patch_drop_logged_;
  uint64_t swaps_ = 0;
  uint64_t reported_silent_frames_ = 0;
};

// Debug dumps of a built pipeline (RENUT_PIPELINE_STATS, RENUT_PIPELINE_IR), in shaders.cpp.
void LogPipelineStatistics(RenderPipeline& pipeline, const HostShader* vs, const HostShader* ps);
void DumpPipelineInternals(RenderPipeline& pipeline, const HostShader* vs, const HostShader* ps,
                           const std::string& dir);

}  // namespace renut::gpu
